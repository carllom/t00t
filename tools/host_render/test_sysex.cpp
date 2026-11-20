// Host test for the t00t SysEx protocol at the platform seam: raw MIDI bytes
// in -- through the shared parse-and-dispatch loop, as both transports feed
// it -- and SysEx replies, cooked-patch commits and ordinary MIDI handling
// out. Runs the real subtractive module (descriptors, cook, factory bank).
//
// Expected byte vectors come from golden/sysex_subtractive.txt, written by
// tools/sysex_vectors.py -- an independent implementation of the framing and
// 7-bit packing -- so these tests don't compare the codec with itself.

#include "factory_bank.h"
#include "patch.h"
#include "midi/midi_controller_generic.h"
#include "patch/platform.h"
#include "patch/multi.h"
#include "patch/sysex.h"
#include "patch/user_bank.h"
#include "ram_flash.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

PartSlotT<SubCookedPatch> sub_part_slots[SUB_PART_COUNT];
PartSlotT<SubMultiCooked> sub_multi_slot;

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

using Bytes = std::vector<uint8_t>;

std::map<std::string, Bytes> load_vectors(const char *path) {
    std::map<std::string, Bytes> out;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return out; }
    char line[16384];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *p = line;
        std::string name;
        while (*p && *p != ' ' && *p != '\n') name += *p++;
        Bytes bytes;
        unsigned b;
        int n;
        while (sscanf(p, " %x%n", &b, &n) == 1) { bytes.push_back((uint8_t)b); p += n; }
        out[name] = bytes;
    }
    fclose(f);
    return out;
}

std::map<std::string, Bytes> g_vec;

const Bytes &vec(const char *name) {
    auto it = g_vec.find(name);
    if (it == g_vec.end()) {
        fprintf(stderr, "missing golden vector %s\n", name);
        static Bytes empty;
        g_ok = false;
        return empty;
    }
    return it->second;
}

// --- The module wiring under test: the generic loop, a NOTE Handler that
// counts, the platform, and SysEx replies captured instead of sent. ---

const FactoryBank kFactoryBanks[] = { { kSubFactoryBank, kSubFactoryCount } };
RamFlash g_flash;
UserBank g_user(ModuleId::SUBTRACTIVE, user_flash(g_flash));
PatchPlatformT<SubtractiveModule> g_platform(sub_part_slots, kFactoryBanks, 1, &g_user);
MultiT<SubtractiveModule> g_multi(g_platform, &g_user, sub_multi_slot);
int g_notes_on = 0;
Bytes g_sent;

// The outgoing queue: unlimited unless a test sets a byte budget, standing in
// for a TX queue that only drains between Core 0 passes.
int64_t g_out_budget = -1;

void set_note(VoiceParamBlock &, const InputValue &v) { if (v.note_on) g_notes_on++; }
bool capture(const uint8_t *data, uint32_t len) {
    if (g_out_budget >= 0) {
        if ((int64_t)len > g_out_budget) return false;
        g_out_budget -= len;
    }
    g_sent.insert(g_sent.end(), data, data + len);
    return true;
}
PatchSysExT<SubtractiveModule> g_sysex(g_platform, g_multi, &g_user, capture);
void on_sysex(const uint8_t *data, uint32_t len) { g_sysex.handle(data, len); }

constexpr InputMapEntryT<VoiceParamBlock> kTable[] = {
    { InputCategory::NOTE, 0, 127, 0xFF, 0, set_note },
};

ParamExchange g_px;

void feed(MidiSource source, const Bytes &bytes) {
    midi_parser.select(source);
    midi_controller_process_generic(bytes.data(), (uint32_t)bytes.size(), &g_px, kTable, on_sysex);
}
void feed(MidiSource source, std::initializer_list<uint8_t> bytes) { feed(source, Bytes(bytes)); }

void reset() {
    midi_parser.init();
    g_px.init();
    g_notes_on = 0;
    g_sent.clear();
    g_out_budget = -1;
    g_sysex.task();  // finish any dump a previous test left running
    g_sent.clear();
    g_flash.reset();
    g_multi.init();
    for (uint8_t p = 0; p < SUB_PART_COUNT; p++) g_platform.load_factory(p, 0, 0);
    SubCookedPatch drain;
    for (auto &s : sub_part_slots) s.consume(drain);
}

// Descriptor tables the dump can't carry are rejected at compile time.
constexpr ParamDescriptor kNonAscii[] = {
    { 0, "CUT" "\xC3\x96" "FF", "G", 0, 1, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
};
constexpr ParamDescriptor kLongName[] = {
    { 0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      "G", 0, 1, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
};
constexpr ParamDescriptor kHugeRange[] = {
    { 0, "X", "G", 0, 0x200000, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
};
static_assert(sysex_descriptor_dump_fits(SubtractiveModule::kDescriptors), "subtractive's table can be dumped");
static_assert(!sysex_descriptor_dump_fits(kNonAscii), "a non-ASCII name is rejected");
static_assert(!sysex_descriptor_dump_fits(kLongName), "a name over 127 characters is rejected");
static_assert(!sysex_descriptor_dump_fits(kHugeRange), "a value outside int21 is rejected");

// --- Tests ---

void test_sysex_is_assembled_and_other_midi_flows_around_it() {
    reset();
    // Note on, then identify request with a MIDI Clock byte inside it, then a
    // note on (fresh status byte: SysEx cancels running status).
    feed(MIDI_SOURCE_USB, { 0x90, 60, 100, 0xF0, 0x7D, 0xF8, 0x54, 0x01, 0xF7, 0x90, 62, 100 });
    check(g_sent == vec("identify_reply"), "identify request with a real-time byte inside is answered");
    check(g_notes_on == 2, "notes around the SysEx are still handled");
}

void test_over_length_interrupted_and_foreign_sysex_are_ignored() {
    reset();
    Bytes huge = { 0xF0, 0x7D, 0x54, 0x01 };
    huge.insert(huge.end(), MIDI_SYSEX_MAX, 0x00);
    huge.push_back(0xF7);
    feed(MIDI_SOURCE_USB, huge);
    check(g_sent.empty(), "an over-length SysEx is discarded");

    feed(MIDI_SOURCE_USB, { 0xF0, 0x7D, 0x54, 0x90, 64, 100, 0x01, 0xF7 });
    check(g_sent.empty() && g_notes_on == 1, "a status byte inside SysEx abandons it and is handled as MIDI");

    feed(MIDI_SOURCE_USB, { 0xF0, 0x43, 0x10, 0x01, 0xF7, 0xF0, 0x7D, 0x55, 0x01, 0xF7, 0xF0, 0x7D, 0x54, 0x7E, 0xF7 });
    check(g_sent.empty(), "another manufacturer's, another sub-ID's and an unknown command are ignored");

    feed(MIDI_SOURCE_USB, { 0x90, 65, 100 });
    check(g_notes_on == 2, "MIDI after all that is unaffected");
}

// An identify request padded with data bytes the command ignores.
Bytes padded_identify(uint32_t between_f0_and_f7) {
    Bytes b = { 0xF0, 0x7D, 0x54, 0x01 };
    b.insert(b.end(), between_f0_and_f7 - 3, 0x00);
    b.push_back(0xF7);
    return b;
}

void test_sysex_limit_is_exact_and_stray_f7_is_harmless() {
    reset();
    feed(MIDI_SOURCE_USB, padded_identify(MIDI_SYSEX_MAX));
    check(g_sent == vec("identify_reply"), "a SysEx of exactly MIDI_SYSEX_MAX bytes is kept");
    g_sent.clear();
    feed(MIDI_SOURCE_USB, padded_identify(MIDI_SYSEX_MAX + 1));
    check(g_sent.empty(), "one byte more and it's discarded");

    feed(MIDI_SOURCE_USB, { 0x90, 60, 100, 0xF7, 61, 100, 0x90, 62, 100 });
    check(g_notes_on == 2, "a stray F7 ends running status but disturbs nothing else");
    feed(MIDI_SOURCE_USB, vec("identify_request"));
    check(g_sent == vec("identify_reply"), "and SysEx after it still works");
}

void test_usb_and_din_sysex_are_assembled_separately() {
    reset();
    const Bytes &req = vec("identify_request");
    // USB delivers the first half of a request, DIN a note, USB the rest.
    feed(MIDI_SOURCE_USB, Bytes(req.begin(), req.begin() + 3));
    feed(MIDI_SOURCE_UART, { 0x90, 60, 100 });
    feed(MIDI_SOURCE_USB, Bytes(req.begin() + 3, req.end()));
    check(g_sent == vec("identify_reply"), "a SysEx split around DIN traffic is still answered");
    check(g_notes_on == 1, "and the DIN note in between is handled");
}

void test_identify() {
    reset();
    feed(MIDI_SOURCE_USB, vec("identify_request"));
    check(g_sent == vec("identify_reply"), "identify reply: module, schema, protocol, Parts, parameter count");
}

void test_parameter_change_sets_exact_values() {
    reset();
    SubCookedPatch got{};
    feed(MIDI_SOURCE_USB, vec("param_change_cutoff_1234_part0"));
    check(sub_part_slots[0].consume(got) && got.live.filter_cutoff == 1234,
          "parameter change: Part 0 CUTOFF = 1234 Hz exactly, cooked and published");
    feed(MIDI_SOURCE_UART, vec("param_change_env_cutoff_minus500_part3"));
    check(sub_part_slots[3].consume(got) && got.live.filter_env_amount == -500,
          "parameter change over DIN: Part 3 ENV>CUT = -500 Hz (negative int21)");
}

void test_edit_buffer_request_answers_with_a_dump() {
    reset();
    feed(MIDI_SOURCE_USB, vec("edit_buffer_request_part0"));
    check(g_sent == vec("edit_buffer_dump_fairlite_part0"), "edit-buffer request: dump of Part 0's Fairlite");
}

void test_edit_buffer_dump_loads_into_the_part() {
    reset();
    feed(MIDI_SOURCE_USB, vec("edit_buffer_dump_saw_flt_part1"));

    SubCookedPatch got{}, want{};
    int32_t values[SUB_P_COUNT];
    char name[kPatchNameLen + 1];
    patch_decode(kSubFactoryBank[2].data, kSubFactoryBank[2].len, ModuleId::SUBTRACTIVE,
                 SubtractiveModule::kDescriptors, values, name);
    SubtractiveModule::cook(values, want);
    check(sub_part_slots[1].consume(got) && memcmp(&got, &want, sizeof got) == 0,
          "edit-buffer dump loads Saw-Flt into Part 1, cooked and published");
    check(strcmp(g_platform.name(1), "Saw-Flt") == 0, "with its name");

    feed(MIDI_SOURCE_USB, vec("edit_buffer_request_part1"));
    check(g_sent == vec("edit_buffer_dump_saw_flt_part1"), "requesting Part 1 back returns the same dump");
}

void test_dump_for_another_module_is_rejected() {
    reset();
    SubCookedPatch got{};
    feed(MIDI_SOURCE_USB, vec("edit_buffer_dump_fm_module_part1"));
    check(!sub_part_slots[1].consume(got), "a dump whose Patch is for another module is rejected");
    check(strcmp(g_platform.name(1), "Fairlite") == 0, "and Part 1 keeps its Patch");

    feed(MIDI_SOURCE_USB, vec("edit_buffer_dump_malformed_part1"));
    check(!sub_part_slots[1].consume(got), "a malformed dump is ignored");
    feed(MIDI_SOURCE_USB, vec("edit_buffer_dump_truncated_part1"));
    check(!sub_part_slots[1].consume(got), "a dump shorter than its Patch header says is rejected");
}

void test_descriptor_dump_sends_both_tables() {
    reset();
    feed(MIDI_SOURCE_USB, vec("descriptor_dump_request"));
    g_sysex.task();
    check(g_sent == vec("descriptor_dump"),
          "descriptor dump: module table, common table, labels, end -- one message per descriptor");
}

void test_descriptor_dump_is_paced_by_the_outgoing_queue() {
    reset();
    // Room for 100 bytes per Core 0 pass, about two descriptor messages.
    g_out_budget = 100;
    feed(MIDI_SOURCE_USB, vec("descriptor_dump_request"));
    int passes = 0;
    while (g_sent.size() < vec("descriptor_dump").size() && passes < 100) {
        g_out_budget = 100;
        g_sysex.task();
        passes++;
    }
    check(g_sent == vec("descriptor_dump"), "a dump through a small queue arrives whole, in order");
    check(passes > 5, "spread over several passes");

    g_out_budget = -1;
    size_t before = g_sent.size();
    g_sysex.task();
    check(g_sent.size() == before, "and stops once complete");
}

void test_other_sysex_is_answered_during_a_dump() {
    reset();
    g_out_budget = 0;  // the queue is full: the dump stalls...
    feed(MIDI_SOURCE_USB, vec("descriptor_dump_request"));
    feed(MIDI_SOURCE_USB, vec("identify_request"));  // ...and so does the identify reply
    check(g_sent.empty(), "nothing goes out while the queue is full");

    g_out_budget = -1;
    g_sysex.task();
    const Bytes &reply = vec("identify_reply");
    const Bytes &dump = vec("descriptor_dump");
    check(g_sent.size() == reply.size() + dump.size() &&
          Bytes(g_sent.begin(), g_sent.begin() + reply.size()) == reply &&
          Bytes(g_sent.begin() + reply.size(), g_sent.end()) == dump,
          "once there's room the held-back identify reply goes first, then the whole dump");
}

void test_store_request_writes_the_edit_buffer_and_replies() {
    reset();  // Part 0 holds Fairlite
    feed(MIDI_SOURCE_USB, vec("store_request_part0_slot10"));
    check(g_sent == vec("store_reply_part0_slot10_ok"), "store request: reply OK for Part 0, slot 10");
    const StoredPatchRef slot = g_user.patch(10);
    check(slot.data && memcmp(slot.data, kSubFactoryBank[0].data, kSubFactoryBank[0].len) == 0,
          "user slot 10 now holds Part 0's Edit buffer, Fairlite's stored bytes");
    check(g_flash.writes.size() == 1, "with one sector write");
}

void test_store_request_reports_a_refused_store() {
    reset();
    feed(MIDI_SOURCE_USB, vec("store_request_part0_slot200"));
    check(g_sent == vec("store_reply_part0_slot200_no_such_slot"), "a slot past the bank is answered NO_SUCH_SLOT");
    g_sent.clear();
    g_flash.fail = true;
    feed(MIDI_SOURCE_USB, vec("store_request_part0_slot10"));
    check(g_sent == vec("store_reply_part0_slot10_flash_failed"), "a failed flash write is answered FLASH_FAILED");
    g_sent.clear();
    feed(MIDI_SOURCE_USB, vec("store_request_part16_slot0"));
    check(g_sent.empty() && g_flash.writes.empty(), "a store for an unknown Part is ignored");
}

// Unpack a dump's packed payload (after F0 7D 54 <command>, up to F7).
Bytes unpacked_payload(const Bytes &msg) {
    Bytes out(MIDI_SYSEX_MAX);
    const uint32_t n = sysex_unpack(msg.data() + 4, (uint32_t)msg.size() - 5, out.data(), (uint32_t)out.size());
    out.resize(n);
    return out;
}

void test_multi_parameter_change_sets_multi_and_part_values() {
    reset();
    feed(MIDI_SOURCE_USB, vec("multi_param_change_fx_mix_100"));
    check(g_multi.common(COMMON_P_FX_MIX) == 100, "Multi parameter change: the Multi's FX MIX = 100");
    feed(MIDI_SOURCE_USB, vec("multi_param_change_part2_volume_64"));
    check(g_multi.part_param(2, PART_P_VOLUME) == 64, "and Part 2's VOLUME = 64");
}

void test_multi_edit_buffer_request_and_dump() {
    reset();
    feed(MIDI_SOURCE_USB, vec("multi_edit_buffer_request"));
    check(g_sent == vec("multi_edit_buffer_dump_default"), "Multi edit-buffer request: dump of the default Multi");
    feed(MIDI_SOURCE_USB, vec("multi_edit_buffer_dump_layer"));
    check(g_multi.parts_on_channel(0) == 3 && g_multi.common(COMMON_P_FX_TYPE) == FX_REVERB &&
          strcmp(g_multi.name(), "LAYER") == 0, "a Multi dump loads: Parts 0 and 1 layered on channel 1, reverb");
}

void test_multi_store_and_load_by_slot() {
    reset();
    feed(MIDI_SOURCE_USB, vec("multi_store_request_slot3"));
    check(g_sent == vec("multi_store_reply_slot3_ok"), "Multi store request: reply OK for slot 3");
    const Bytes want = unpacked_payload(vec("multi_edit_buffer_dump_default"));
    const StoredPatchRef slot = g_user.multi(3);
    check(slot.data && !want.empty() && memcmp(slot.data, want.data(), want.size()) == 0,
          "Multi slot 3 holds the default Multi");

    g_sent.clear();
    g_multi.set_common(COMMON_P_FX_TYPE, FX_OFF);
    feed(MIDI_SOURCE_USB, vec("multi_load_request_slot3"));
    check(g_sent == vec("multi_load_reply_slot3_ok") && g_multi.common(COMMON_P_FX_TYPE) == FX_DELAY,
          "Multi load request (select): reply OK, the stored Multi is back");
    g_sent.clear();
    feed(MIDI_SOURCE_USB, vec("multi_load_request_slot5"));
    check(g_sent == vec("multi_load_reply_slot5_empty"), "an empty slot is answered NO_SUCH_PATCH");
}

// The user bank the bank-dump vectors describe: Fairlite in Patch slot 0,
// Saw-Flt in slot 10, the default Multi in Multi slot 3.
void fill_user_bank() {
    g_user.store_patch(0, kSubFactoryBank[0].data, kSubFactoryBank[0].len);
    g_user.store_patch(10, kSubFactoryBank[2].data, kSubFactoryBank[2].len);
    g_multi.store(3);
}

void test_bank_dump_sends_every_slot() {
    reset();
    fill_user_bank();
    feed(MIDI_SOURCE_USB, vec("bank_dump_request"));
    g_sysex.task();
    check(g_sent == vec("bank_dump"),
          "bank dump: every Patch slot then every Multi slot in order, empty ones without data, then the end");
}

void test_bank_slot_is_written_byte_for_byte_and_answered() {
    reset();
    feed(MIDI_SOURCE_USB, vec("bank_slot_patch10_saw_flt"));
    check(g_sent == vec("bank_slot_reply_patch10_ok"), "bank slot: Saw-Flt into Patch slot 10, answered OK");
    const StoredPatchRef slot = g_user.patch(10);
    check(slot.data && memcmp(slot.data, kSubFactoryBank[2].data, kSubFactoryBank[2].len) == 0,
          "Patch slot 10 holds Saw-Flt's stored bytes");
    check(slot.data && slot.data[kSubFactoryBank[2].len] == 0xFF && slot.data[kUserPatchSlotSize - 1] == 0xFF,
          "padded with 0xFF to the slot's end");

    g_sent.clear();
    feed(MIDI_SOURCE_USB, vec("bank_slot_multi3_default"));
    const Bytes want = unpacked_payload(vec("multi_edit_buffer_dump_default"));
    check(g_sent == vec("bank_slot_reply_multi3_ok") && !want.empty() &&
          memcmp(g_user.multi(3).data, want.data(), want.size()) == 0,
          "a Multi slot message writes Multi slot 3, answered OK");
}

void test_empty_bank_slot_empties_the_slot() {
    reset();
    fill_user_bank();
    feed(MIDI_SOURCE_USB, vec("bank_slot_patch0_empty"));
    check(g_sent == vec("bank_slot_reply_patch0_ok"), "an empty bank slot message is answered OK");
    bool erased = true;
    const StoredPatchRef slot = g_user.patch(0);
    for (uint32_t i = 0; i < kUserPatchSlotSize; i++) erased = erased && slot.data[i] == 0xFF;
    check(erased && g_user.patch_empty(0), "and Patch slot 0 is erased");
    check(!g_user.patch_empty(10), "its sector neighbour slot 10 keeps Saw-Flt");
}

void test_refused_bank_slots_are_answered_and_leave_flash_alone() {
    reset();
    fill_user_bank();
    const std::vector<uint8_t> before = g_flash.mem;
    feed(MIDI_SOURCE_USB, vec("bank_slot_patch10_fm_module"));
    check(g_sent == vec("bank_slot_reply_patch10_bad_patch"), "a Patch for another module is answered BAD_PATCH");
    g_sent.clear();
    feed(MIDI_SOURCE_USB, vec("bank_slot_patch200_saw_flt"));
    check(g_sent == vec("bank_slot_reply_patch200_no_such_slot"), "a slot past the bank is answered NO_SUCH_SLOT");
    check(g_flash.mem == before, "neither touches flash");

    g_sent.clear();
    g_user.restore(SlotKind::MULTI, 3, nullptr, 0);  // so the write below has something to change
    g_flash.fail = true;
    feed(MIDI_SOURCE_USB, vec("bank_slot_multi3_default"));
    check(g_sent == vec("bank_slot_reply_multi3_flash_failed"), "a failed flash write is answered FLASH_FAILED");
}

// The messages of a concatenated stream, each F0 ... F7.
std::vector<Bytes> split_messages(const Bytes &stream) {
    std::vector<Bytes> out;
    for (size_t i = 0; i < stream.size(); i++) {
        if (stream[i] != 0xF0) continue;
        size_t j = i;
        while (j < stream.size() && stream[j] != 0xF7) j++;
        out.emplace_back(stream.begin() + i, stream.begin() + j + 1);
        i = j;
    }
    return out;
}

void test_restoring_a_bank_dump_round_trips_byte_for_byte() {
    reset();
    fill_user_bank();
    // An older-schema Patch (two parameters short) in slot 127 and a Multi in
    // slot 31: restored as they are, not re-encoded.
    Bytes old_schema(kSubFactoryBank[1].data, kSubFactoryBank[1].data + kSubFactoryBank[1].len - 2);
    old_schema[22] -= 2;
    g_flash.poke(user_patch_slot_offset(ModuleId::SUBTRACTIVE, 127), old_schema.data(), (uint32_t)old_schema.size());
    g_multi.store(31);
    const std::vector<uint8_t> backup_flash = g_flash.mem;

    feed(MIDI_SOURCE_USB, vec("bank_dump_request"));
    g_sysex.task();
    const std::vector<Bytes> dump = split_messages(g_sent);
    check(dump.size() == kUserPatchSlots + kUserMultiSlots + 1, "the dump is one message per slot plus the end");

    // A different bank on the device: other Patches in other slots.
    g_flash.reset();
    g_user.store_patch(5, kSubFactoryBank[3].data, kSubFactoryBank[3].len);
    g_user.store_patch(10, kSubFactoryBank[4].data, kSubFactoryBank[4].len);
    g_multi.store(0);

    bool all_ok = true;
    for (size_t i = 0; i + 1 < dump.size(); i++) {
        g_sent.clear();
        feed(MIDI_SOURCE_USB, dump[i]);
        // The reply echoes kind and slot, then OK.
        const Bytes want = { 0xF0, 0x7D, 0x54, 0x38, dump[i][4], dump[i][5], dump[i][6], 0x00, 0xF7 };
        all_ok = all_ok && g_sent == want;
    }
    check(all_ok, "each slot sent back is answered OK");
    check(g_flash.mem == backup_flash, "the region is byte for byte what was backed up, older-schema Patch included");
}

void test_bank_dump_is_paced_by_the_outgoing_queue() {
    reset();
    fill_user_bank();
    // Room for 200 bytes per pass: a Multi slot message (~155) fits, two don't.
    g_out_budget = 200;
    feed(MIDI_SOURCE_USB, vec("bank_dump_request"));
    int passes = 0;
    while (g_sent.size() < vec("bank_dump").size() && passes < 1000) {
        g_out_budget = 200;
        g_sysex.task();
        passes++;
    }
    check(g_sent == vec("bank_dump"), "a bank dump through a small queue arrives whole, in order");
    check(passes > 5, "spread over several passes");
}

void test_a_bank_dump_waits_for_a_descriptor_dump_in_progress() {
    reset();
    fill_user_bank();
    g_out_budget = 0;
    feed(MIDI_SOURCE_USB, vec("descriptor_dump_request"));
    feed(MIDI_SOURCE_USB, vec("bank_dump_request"));
    g_out_budget = -1;
    g_sysex.task();
    Bytes want = vec("descriptor_dump");
    want.insert(want.end(), vec("bank_dump").begin(), vec("bank_dump").end());
    check(g_sent == want, "requested during a descriptor dump, the bank dump follows it whole");
}

void test_a_held_reply_goes_out_before_a_bank_dump() {
    reset();
    fill_user_bank();
    g_out_budget = 0;
    feed(MIDI_SOURCE_USB, vec("identify_request"));  // the reply is held
    g_out_budget = -1;
    feed(MIDI_SOURCE_USB, vec("bank_dump_request"));
    g_sysex.task();
    Bytes want = vec("identify_reply");
    want.insert(want.end(), vec("bank_dump").begin(), vec("bank_dump").end());
    check(g_sent == want, "a reply held before a bank dump request goes out first, then the dump");
}

}  // namespace

int main() {
    g_vec = load_vectors(T00T_SYSEX_GOLDEN);
    test_sysex_is_assembled_and_other_midi_flows_around_it();
    test_over_length_interrupted_and_foreign_sysex_are_ignored();
    test_sysex_limit_is_exact_and_stray_f7_is_harmless();
    test_usb_and_din_sysex_are_assembled_separately();
    test_identify();
    test_parameter_change_sets_exact_values();
    test_edit_buffer_request_answers_with_a_dump();
    test_edit_buffer_dump_loads_into_the_part();
    test_dump_for_another_module_is_rejected();
    test_descriptor_dump_sends_both_tables();
    test_descriptor_dump_is_paced_by_the_outgoing_queue();
    test_other_sysex_is_answered_during_a_dump();
    test_store_request_writes_the_edit_buffer_and_replies();
    test_store_request_reports_a_refused_store();
    test_multi_parameter_change_sets_multi_and_part_values();
    test_multi_edit_buffer_request_and_dump();
    test_multi_store_and_load_by_slot();
    test_bank_dump_sends_every_slot();
    test_bank_slot_is_written_byte_for_byte_and_answered();
    test_empty_bank_slot_empties_the_slot();
    test_refused_bank_slots_are_answered_and_leave_flash_alone();
    test_restoring_a_bank_dump_round_trips_byte_for_byte();
    test_bank_dump_is_paced_by_the_outgoing_queue();
    test_a_bank_dump_waits_for_a_descriptor_dump_in_progress();
    test_a_held_reply_goes_out_before_a_bank_dump();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
