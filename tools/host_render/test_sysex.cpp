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

PatchPlatformT<SubtractiveModule> g_platform(sub_part_slots, kSubFactoryBank, kSubFactoryCount);
int g_notes_on = 0;
Bytes g_sent;

void set_note(VoiceParamBlock &, const InputValue &v) { if (v.note_on) g_notes_on++; }
void capture(const uint8_t *data, uint32_t len) { g_sent.insert(g_sent.end(), data, data + len); }
void on_sysex(const uint8_t *data, uint32_t len) { patch_sysex_handle(g_platform, data, len, capture); }

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
    for (uint8_t p = 0; p < SUB_PART_COUNT; p++) g_platform.load_factory(p, 0);
    SubCookedPatch drain;
    for (auto &s : sub_part_slots) s.consume(drain);
}

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
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
