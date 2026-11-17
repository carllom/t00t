// Host test for the patch platform core (src/patch/), at the platform seam:
// raw MIDI bytes in, cooked-patch commits out, observed from the Core 1 side
// of each Part's cooked slot. A stub module stands in for a real engine --
// its own descriptors, cook function and factory bank, wired to MIDI the
// same way a real module's Input subsystem is (parser -> Router ->
// Configuration Handler -> platform).
//
// Stored-patch byte vectors are written out literally, so the test pins the
// stored format itself rather than round-tripping through the encoder.

#include "midi/midi_dispatch.h"
#include "midi/midi_parser.h"
#include "patch/platform.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

bool g_ok = true;

void check(bool cond, const char *what) {
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    g_ok = g_ok && cond;
}

// --- Stub module ---

struct StubCooked {
    int32_t level_x10;   // level * 10
    int32_t detune;      // as stored
    int32_t cutoff;      // as stored
};

struct StubModule {
    static constexpr ModuleId kModuleId = ModuleId::SUBTRACTIVE;
    static constexpr uint8_t kSchemaVersion = 2;
    static constexpr uint32_t kPartCount = 2;
    using Cooked = StubCooked;

    static constexpr ParamDescriptor kDescriptors[] = {
        // id name      group   min   max    default unit             curve               class               quick  cc
        { 0, "LEVEL",  "AMP",   0,    100,   50,     ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    true,  kNoDefaultCc },
        { 1, "DETUNE", "OSC",  -100,  100,   0,      ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc },
        { 2, "CUTOFF", "FILT",  20,   18000, 1000,   ParamUnit::HZ,   ParamCurve::EXPONENTIAL, ParamClass::LIVE, true, 74 },
    };

    static void cook(const int32_t *v, Cooked &out) {
        out.level_x10 = v[0] * 10;
        out.detune = v[1];
        out.cutoff = v[2];
    }
};

using Platform = PatchPlatformT<StubModule>;

// Factory patch "BRIGHT": level 80, detune -20, cutoff 5000.
// Payload: level u8 (80), detune u8 offset from -100 (80), cutoff u16 LE
// offset from 20 (4980 = 0x1374).
const uint8_t kBright[] = {
    'T', '0', 'P', 'T', 0x01, 0x02,
    'B', 'R', 'I', 'G', 'H', 'T', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x04, 0x00,
    80, 80, 0x74, 0x13,
};

// Factory patch "DARK": level 30, detune 0, cutoff 300.
const uint8_t kDark[] = {
    'T', '0', 'P', 'T', 0x01, 0x02,
    'D', 'A', 'R', 'K', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x04, 0x00,
    30, 100, 0x18, 0x01,
};

// "OLD": stored by schema 1, before CUTOFF existed -- level 60, detune +5.
const uint8_t kOld[] = {
    'T', '0', 'P', 'T', 0x01, 0x01,
    'O', 'L', 'D', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x02, 0x00,
    60, 105,
};

// "FMPATCH": a valid patch, but for module 3 (FM).
const uint8_t kForeign[] = {
    'T', '0', 'P', 'T', 0x03, 0x02,
    'F', 'M', 'P', 'A', 'T', 'C', 'H', 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x04, 0x00,
    99, 99, 0x00, 0x00,
};

const StoredPatchRef kBank[] = {
    { kDark, sizeof kDark },
    { kBright, sizeof kBright },
    { kOld, sizeof kOld },
    { kForeign, sizeof kForeign },
};

// --- Wiring: what a real module's Input subsystem provides ---

struct Wiring {
    Platform *platform;
};

void set_patch(Wiring &w, const InputValue &value) {
    w.platform->load_factory(value.channel, value.index);
}

constexpr InputMapEntryT<Wiring> kTable[] = {
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0, set_patch },
};

void feed(Wiring &w, MidiParser &parser, const uint8_t *bytes, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!parser.feed(bytes[i], ev)) continue;
        if (ev.type == MIDI_PROGRAM_CHANGE) {
            midi_dispatch_program_change(w, kTable, ev.channel, ev.data1);
        }
    }
}

// A fresh platform with every Part on factory patch 0, and the init commits
// already drained from Core 1's side.
struct Rig {
    PartSlotT<StubCooked> slots[StubModule::kPartCount];
    Platform platform{slots, kBank, sizeof kBank / sizeof kBank[0]};
    Wiring wiring{&platform};
    MidiParser parser{};

    Rig() {
        parser.init();
        for (uint8_t p = 0; p < StubModule::kPartCount; p++) platform.load_factory(p, 0);
        StubCooked drain;
        for (auto &s : slots) s.consume(drain);
    }

    void midi(std::initializer_list<uint8_t> bytes) {
        feed(wiring, parser, bytes.begin(), (uint32_t)bytes.size());
    }
};

// --- Tests ---

void test_program_change_commits_cooked_patch_to_part() {
    Rig rig;
    rig.midi({ 0xC1, 0x01 });  // Program Change, channel 2 (Part 1), program 1 = BRIGHT

    StubCooked got{};
    check(rig.slots[1].consume(got), "PC on Part 1's channel commits a cooked patch to Part 1");
    check(got.level_x10 == 800 && got.detune == -20 && got.cutoff == 5000,
          "committed patch is BRIGHT, cooked (level 80 -> 800, detune -20, cutoff 5000)");
    check(!rig.slots[0].consume(got), "Part 0 sees no commit");
}

void test_older_schema_patch_takes_defaults_for_missing_params() {
    Rig rig;
    rig.midi({ 0xC0, 0x02 });  // Part 0, program 2 = OLD

    StubCooked got{};
    check(rig.slots[0].consume(got), "older-schema patch loads");
    check(got.level_x10 == 600 && got.detune == 5, "stored params keep their stored values");
    check(got.cutoff == 1000, "param missing from the older schema takes its descriptor default");
}

void test_patch_for_another_module_is_rejected() {
    Rig rig;
    rig.midi({ 0xC0, 0x01 });  // Part 0 -> BRIGHT
    StubCooked got{};
    rig.slots[0].consume(got);

    rig.midi({ 0xC0, 0x03 });  // Part 0 -> FMPATCH (module 3)
    check(!rig.slots[0].consume(got), "patch with a mismatched module ID commits nothing");
    check(rig.platform.load_factory(0, 3) == PatchLoadResult::WRONG_MODULE,
          "load reports WRONG_MODULE");

    rig.midi({ 0xC0, 0x00 });  // Part 0 -> DARK still works afterwards
    check(rig.slots[0].consume(got) && got.cutoff == 300, "Part keeps working after a rejected load");
}

bool is_bright(const StubCooked &c) { return c.level_x10 == 800 && c.detune == -20 && c.cutoff == 5000; }
bool is_dark(const StubCooked &c) { return c.level_x10 == 300 && c.detune == 0 && c.cutoff == 300; }

void test_back_to_back_program_changes_deliver_whole_patches_latest_last() {
    Rig rig;
    // Two Program Changes before Core 1 renders a buffer: Core 1 is still
    // holding the first publish, so the second can't overwrite it yet.
    rig.midi({ 0xC0, 0x01, 0xC0, 0x00 });  // BRIGHT, then DARK

    StubCooked got{};
    check(rig.slots[0].consume(got) && is_bright(got), "first buffer sees BRIGHT, whole");
    check(!rig.slots[0].consume(got), "nothing more until Core 0 retries");

    rig.platform.task();  // Core 0's next main-loop pass
    check(rig.slots[0].consume(got) && is_dark(got), "after task(), next buffer sees DARK, whole");

    rig.platform.task();
    check(!rig.slots[0].consume(got), "no duplicate commit once delivered");
}

void test_core0_view_tracks_what_core1_will_render() {
    Rig rig;
    rig.midi({ 0xC0, 0x01, 0xC0, 0x00 });  // BRIGHT published, DARK held back
    check(is_bright(rig.platform.cooked(0)), "Core 0 note-on view is BRIGHT while DARK is held back");
    check(strcmp(rig.platform.name(0), "DARK") == 0, "Edit buffer already holds DARK");

    StubCooked got{};
    rig.slots[0].consume(got);
    rig.platform.task();
    check(is_dark(rig.platform.cooked(0)), "Core 0 note-on view is DARK once published");
}

void test_encoder_writes_the_stored_format() {
    const int32_t values[] = { 80, -20, 5000 };
    uint8_t out[64];
    uint32_t n = patch_encode(StubModule::kModuleId, StubModule::kSchemaVersion, "BRIGHT",
                              StubModule::kDescriptors, values, out, sizeof out);
    check(n == sizeof kBright && memcmp(out, kBright, n) == 0, "encoding BRIGHT reproduces its stored bytes");
    check(n == patch_stored_size(StubModule::kDescriptors), "encoded size matches patch_stored_size()");
    check(patch_encode(StubModule::kModuleId, 1, "X", StubModule::kDescriptors, values, out, 10) == 0,
          "encoder refuses a too-small buffer");
}

void test_missing_factory_slot_is_reported() {
    Rig rig;
    check(rig.platform.load_factory(0, 99) == PatchLoadResult::NO_SUCH_PATCH, "out-of-range program reports NO_SUCH_PATCH");
    StubCooked got{};
    check(!rig.slots[0].consume(got), "and commits nothing");
}

}  // namespace

int main() {
    test_program_change_commits_cooked_patch_to_part();
    test_older_schema_patch_takes_defaults_for_missing_params();
    test_patch_for_another_module_is_rejected();
    test_back_to_back_program_changes_deliver_whole_patches_latest_last();
    test_core0_view_tracks_what_core1_will_render();
    test_encoder_writes_the_stored_format();
    test_missing_factory_slot_is_reported();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
