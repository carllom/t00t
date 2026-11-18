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
const char *sub_part_patch_name(uint8_t) { return ""; }

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
    char line[4096];
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
PatchSysExT<SubtractiveModule> g_sysex(g_platform, capture);
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
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
