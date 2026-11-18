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
#include "patch/receiver.h"
#include "patch/user_bank.h"
#include "ram_flash.h"

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

// Descriptor tables are checked at compile time: default CCs must be
// unreserved and unique.
constexpr ParamDescriptor kOnNrpnDataCc[] = {
    { 0, "X", "G", 0, 1, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, 6 },
};
constexpr ParamDescriptor kDuplicateCc[] = {
    { 0, "X", "G", 0, 1, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, 20 },
    { 1, "Y", "G", 0, 1, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, 20 },
};
static_assert(descriptor_table_valid(StubModule::kDescriptors), "stub table is valid");
static_assert(!descriptor_table_valid(kOnNrpnDataCc), "a default CC on Data Entry (CC6) is rejected");
static_assert(!descriptor_table_valid(kDuplicateCc), "two parameters on one default CC are rejected");

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

// A second factory bank: BRIGHT first.
const StoredPatchRef kBank1[] = {
    { kBright, sizeof kBright },
};

const FactoryBank kFactoryBanks[] = {
    { kBank, sizeof kBank / sizeof kBank[0] },
    { kBank1, sizeof kBank1 / sizeof kBank1[0] },
};

// --- Wiring: what a real module's Input subsystem provides ---

struct Wiring {
    Platform *platform;
};

// Program Change: from the bank the channel's Bank Select MSB picks.
void set_patch(Wiring &w, const InputValue &value) {
    w.platform->load_program(value.channel, midi_channel_bank_msb(value.channel), value.index);
}

// A module's own CC Handler, ahead of the platform in the table.
int g_mod_wheel = -1;
void set_mod_wheel(Wiring &, const InputValue &value) { g_mod_wheel = (int)value.scalar; }

// Every other CC goes to the platform (NRPN, descriptor default CCs).
void set_param_cc(Wiring &w, const InputValue &value) {
    w.platform->midi_cc(value.channel, value.id, (uint8_t)value.scalar);
}

constexpr InputMapEntryT<Wiring> kTable[] = {
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0, set_patch },
    { InputCategory::MODIFIER, 1, 1, 0xFF, 0, set_mod_wheel },
    { InputCategory::MODIFIER, 0, 127, 0xFF, 0, set_param_cc },
};

void feed(Wiring &w, MidiParser &parser, const uint8_t *bytes, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!parser.feed(bytes[i], ev)) continue;
        if (ev.type == MIDI_PROGRAM_CHANGE) {
            midi_dispatch_program_change(w, kTable, ev.channel, ev.data1);
        } else if (ev.type == MIDI_CC && ev.data1 == 0) {
            midi_bank_select_msb(ev.channel, ev.data2);  // as the generic MIDI loop does
        } else if (ev.type == MIDI_CC) {
            midi_dispatch_cc(w, kTable, ev.channel, ev.data1, ev.data2);
        }
    }
}

// A fresh platform with every Part on factory patch 0, and the init commits
// already drained from Core 1's side.
struct Rig {
    PartSlotT<StubCooked> slots[StubModule::kPartCount];
    RamFlash flash;
    UserBank user{StubModule::kModuleId, user_flash(flash)};
    Platform platform{slots, kFactoryBanks, 2, &user};
    Wiring wiring{&platform};
    MidiParser parser{};

    Rig() {
        parser.init();
        midi_bank_select_init();
        for (uint8_t p = 0; p < StubModule::kPartCount; p++) platform.load_factory(p, 0, 0);
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
    check(rig.platform.load_factory(0, 0, 3) == PatchLoadResult::WRONG_MODULE,
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

void test_core0_view_is_the_latest_edit_buffer_cook() {
    Rig rig;
    rig.midi({ 0xC0, 0x01, 0xC0, 0x00 });  // BRIGHT published, DARK held back
    check(is_dark(rig.platform.cooked(0)), "Core 0's note-on view is DARK at once, though Core 1 still has BRIGHT");
    check(strcmp(rig.platform.name(0), "DARK") == 0, "Edit buffer holds DARK");
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
    check(rig.platform.load_factory(0, 0, 99) == PatchLoadResult::NO_SUCH_PATCH, "out-of-range program reports NO_SUCH_PATCH");
    StubCooked got{};
    check(!rig.slots[0].consume(got), "and commits nothing");
}

// NRPN select (CC99 MSB, CC98 LSB), then Data Entry MSB (CC6) [+ LSB (CC38)].
void test_nrpn_sets_a_parameter_and_commits_the_recooked_patch() {
    Rig rig;  // both Parts on DARK: level 30, detune 0, cutoff 300
    rig.midi({ 0xB1, 99, 0, 0xB1, 98, 0, 0xB1, 6, 127 });  // Part 1, LEVEL = full scale

    StubCooked got{};
    check(rig.slots[1].consume(got), "NRPN commits a re-cooked patch to the channel's Part");
    check(got.level_x10 == 1000 && got.detune == 0 && got.cutoff == 300,
          "only LEVEL changed, to its max (100 -> 1000 cooked)");
    check(!rig.slots[0].consume(got), "the other Part is untouched");
}

// CUTOFF is 20..18000 Hz, NRPN 2.
void test_nrpn_value_scales_onto_the_range_and_lsb_refines() {
    Rig rig;
    StubCooked got{};
    rig.midi({ 0xB0, 99, 0, 0xB0, 98, 2, 0xB0, 6, 0 });
    check(rig.slots[0].consume(got) && got.cutoff == 20, "Data Entry 0 -> range min (20 Hz)");
    rig.midi({ 0xB0, 6, 127 });
    check(rig.slots[0].consume(got) && got.cutoff == 18000, "Data Entry MSB 127 alone -> range max (18000 Hz)");
    rig.midi({ 0xB0, 6, 64 });
    check(rig.slots[0].consume(got) && got.cutoff == 9081, "MSB 64 alone -> 9081 Hz (MSB replicated into LSB)");
    rig.midi({ 0xB0, 38, 0 });
    check(rig.slots[0].consume(got) && got.cutoff == 9011, "LSB 0 then refines to 9011 Hz (8192/16383 of the range)");
}

void test_descriptor_default_cc_sets_its_parameter() {
    Rig rig;
    StubCooked got{};
    rig.midi({ 0xB0, 74, 127 });
    check(rig.slots[0].consume(got) && got.cutoff == 18000, "CUTOFF's default CC74 at 127 -> 18000 Hz");
    rig.midi({ 0xB0, 74, 0 });
    check(rig.slots[0].consume(got) && got.cutoff == 20, "CC74 at 0 -> 20 Hz");
}

void test_module_cc_handlers_keep_precedence() {
    Rig rig;
    StubCooked got{};
    g_mod_wheel = -1;
    rig.midi({ 0xB0, 1, 99 });
    check(g_mod_wheel == 99, "CC1 still reaches the module's own Handler");
    rig.midi({ 0xB0, 20, 99 });
    check(!rig.slots[0].consume(got), "a CC no descriptor claims changes nothing");
}

void test_unselected_or_unknown_nrpn_changes_nothing() {
    Rig rig;
    StubCooked got{};
    rig.midi({ 0xB0, 6, 100 });
    check(!rig.slots[0].consume(got), "Data Entry with no NRPN selected changes nothing");
    rig.midi({ 0xB0, 99, 0x7F, 0xB0, 98, 0x00, 0xB0, 6, 100 });
    check(!rig.slots[0].consume(got), "NRPN for an unknown parameter ID changes nothing");
    rig.midi({ 0xB0, 99, 0, 0xB0, 98, 0, 0xB0, 101, 0, 0xB0, 100, 0, 0xB0, 6, 100 });
    check(!rig.slots[0].consume(got), "an RPN select deselects the NRPN (RPN pitch-bend range doesn't hit LEVEL)");
}

void test_burst_of_edits_in_one_buffer_lands_on_the_latest_value() {
    Rig rig;
    StubCooked got{};
    rig.midi({ 0xB0, 74, 10, 0xB0, 74, 20, 0xB0, 74, 127 });  // three edits before Core 1 renders
    rig.slots[0].consume(got);
    rig.platform.task();
    check(rig.slots[0].consume(got) && got.cutoff == 18000, "after Core 0's next pass, Core 1 has the latest edit");
    rig.platform.task();
    check(!rig.slots[0].consume(got), "and nothing older follows it");
}

// --- Stub engine voices ---
// DETUNE is the stub's note-on parameter: Core 0 copies it into the voice at
// note-on, from the Edit buffer's latest cook. LEVEL and CUTOFF are live:
// Core 1 reads them from the Part's newest cooked patch every buffer.
struct StubVoice {
    int32_t detune;    // note-on copy
    uint16_t load_id;  // Patch load the note belongs to
};

StubVoice start_voice(const Platform &platform, uint8_t part) {
    return { platform.cooked(part).detune, platform.load_id(part) };
}

void test_note_on_edit_reaches_the_next_note_even_while_core1_is_behind() {
    Rig rig;
    PartReceiverT<StubCooked> rx;
    rig.platform.load_factory(0, 0, 0);
    rx.take(rig.slots[0]);

    // CUTOFF edit occupies the slot; the DETUNE (note-on) edit right behind
    // it is still pending when the next note starts.
    rig.midi({ 0xB0, 74, 64, 0xB0, 99, 0, 0xB0, 98, 1, 0xB0, 6, 127 });
    StubVoice v = start_voice(rig.platform, 0);
    check(v.detune == 100, "a note started after a note-on edit gets it, though it isn't published yet");
}

void test_live_edit_reaches_core1_from_the_next_buffer() {
    Rig rig;
    PartReceiverT<StubCooked> rx;
    rig.platform.load_factory(0, 0, 0);
    rx.take(rig.slots[0]);
    rig.midi({ 0xB0, 99, 0, 0xB0, 98, 0, 0xB0, 6, 127 });  // LEVEL (live) to 100
    check(rx.take(rig.slots[0]) == PartTake::EDIT && rx.current().level_x10 == 1000,
          "a live LEVEL edit is Core 1's current patch from the next buffer");
}

void test_core1_tells_a_patch_load_from_an_edit() {
    Rig rig;
    PartReceiverT<StubCooked> rx;
    rig.platform.load_factory(0, 0, 0);
    check(rx.take(rig.slots[0]) == PartTake::LOAD, "a loaded Patch arrives as a new load");
    rig.midi({ 0xB0, 74, 64 });
    check(rx.take(rig.slots[0]) == PartTake::EDIT, "a parameter edit arrives as an edit of the same Patch");
    rig.midi({ 0xC0, 0x01 });
    check(rx.take(rig.slots[0]) == PartTake::LOAD, "a Program Change arrives as a new load");
    check(rx.take(rig.slots[0]) == PartTake::NONE, "nothing new: NONE");
}

void test_notes_know_which_patch_load_they_belong_to() {
    Rig rig;
    PartReceiverT<StubCooked> rx;
    rig.platform.load_factory(0, 0, 0);  // DARK
    rx.take(rig.slots[0]);

    StubVoice on_dark = start_voice(rig.platform, 0);
    check(on_dark.load_id == rx.load_id(), "a note matches the load Core 1 holds");

    rig.midi({ 0xB0, 74, 64 });  // an edit keeps the load
    rx.take(rig.slots[0]);
    check(on_dark.load_id == rx.load_id(), "after an edit the note still belongs to the Part's Patch");

    rig.midi({ 0xC0, 0x01 });  // BRIGHT
    StubVoice on_bright = start_voice(rig.platform, 0);
    check(on_bright.load_id != on_dark.load_id, "a note after a Program Change belongs to the new load");
    rx.take(rig.slots[0]);
    check(on_dark.load_id == rx.previous_load_id() && on_dark.load_id != rx.load_id(),
          "Core 1 can tell the DARK note is on the previous load, BRIGHT's on the current one");
    check(on_bright.load_id == rx.load_id(), "and the BRIGHT note matches the current load");
}

// --- Banks: Bank Select MSB 0-63 = factory bank, 64+ = user bank; Program
// Change = slot in it. ---

void test_bank_select_msb_picks_a_factory_bank() {
    Rig rig;
    rig.midi({ 0xB0, 0, 1, 0xC0, 0 });  // MSB 1, program 0: factory bank 1's BRIGHT
    StubCooked got{};
    check(rig.slots[0].consume(got) && is_bright(got), "MSB 1 + PC 0 loads factory bank 1's first Patch");
    rig.midi({ 0xB0, 0, 0, 0xC0, 0 });  // back to bank 0: DARK
    check(rig.slots[0].consume(got) && is_dark(got), "MSB 0 + PC 0 loads factory bank 0's first Patch");
}

void test_program_change_loads_from_the_user_bank() {
    Rig rig;
    rig.user.store_patch(3, kBright, sizeof kBright);
    rig.midi({ 0xB1, 0, 64, 0xC1, 3 });  // Part 1: MSB 64 (user bank), program 3
    StubCooked got{};
    check(rig.slots[1].consume(got) && is_bright(got), "MSB 64 + PC 3 loads user slot 3");
    check(strcmp(rig.platform.name(1), "BRIGHT") == 0, "with its name");
}

void test_empty_slots_and_missing_banks_are_ignored() {
    Rig rig;
    StubCooked got{};
    rig.midi({ 0xB0, 0, 64, 0xC0, 7 });
    check(!rig.slots[0].consume(got), "an empty user slot loads nothing");
    rig.midi({ 0xB0, 0, 65, 0xC0, 0 });
    check(!rig.slots[0].consume(got), "a user bank past the last loads nothing");
    rig.midi({ 0xB0, 0, 2, 0xC0, 0 });
    check(!rig.slots[0].consume(got), "a factory bank the module doesn't have loads nothing");
    rig.midi({ 0xB0, 0, 1, 0xC0, 1 });
    check(!rig.slots[0].consume(got), "a program past a factory bank's end loads nothing");
    check(strcmp(rig.platform.name(0), "DARK") == 0, "the Part keeps its Patch throughout");
    check(rig.platform.load_program(0, 64, 7) == PatchLoadResult::NO_SUCH_PATCH, "an empty slot reports NO_SUCH_PATCH");
}

void test_store_writes_the_edit_buffer_to_a_user_slot() {
    Rig rig;  // Part 0 on DARK: level 30, detune 0, cutoff 300
    rig.midi({ 0xB0, 99, 0, 0xB0, 98, 0, 0xB0, 6, 127 });  // LEVEL = 100
    check(rig.platform.store(0, 10) == StoreResult::OK, "storing Part 0 into user slot 10 succeeds");

    // DARK with level 100: payload level 100, detune 100 (0 - -100), cutoff 280 (300 - 20).
    const uint8_t want[] = {
        'T', '0', 'P', 'T', 0x01, 0x02,
        'D', 'A', 'R', 'K', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x04, 0x00,
        100, 100, 0x18, 0x01,
    };
    const StoredPatchRef slot = rig.user.patch(10);
    check(memcmp(slot.data, want, sizeof want) == 0, "the slot holds the edited Edit buffer in stored format");

    rig.midi({ 0xB1, 0, 64, 0xC1, 10 });  // and Part 1 can load it
    StubCooked got{};
    check(rig.slots[1].consume(got) && got.level_x10 == 1000 && got.cutoff == 300, "Program Change loads it back");
    check(rig.platform.store(0, kUserPatchSlots) == StoreResult::NO_SUCH_SLOT, "a slot past the bank is refused");
    check(rig.platform.store(StubModule::kPartCount, 0) == StoreResult::NO_SUCH_SLOT, "so is an unknown Part");
}

void test_store_without_a_user_bank_is_refused() {
    PartSlotT<StubCooked> slots[StubModule::kPartCount];
    Platform platform{slots, kFactoryBanks, 2, nullptr};
    platform.load_factory(0, 0, 0);
    check(platform.store(0, 0) == StoreResult::NO_SUCH_SLOT, "a platform without a user bank can't store");
    check(platform.load_program(0, 64, 0) == PatchLoadResult::NO_SUCH_PATCH, "or load from one");
}

void test_user_slot_holding_another_modules_patch_is_rejected() {
    Rig rig;
    rig.flash.poke(user_patch_slot_offset(StubModule::kModuleId, 4), kForeign, sizeof kForeign);
    rig.midi({ 0xB0, 0, 64, 0xC0, 4 });
    StubCooked got{};
    check(!rig.slots[0].consume(got), "a user slot holding module 3's Patch loads nothing");
    check(rig.platform.load_program(0, 64, 4) == PatchLoadResult::WRONG_MODULE, "load reports WRONG_MODULE");
}

void test_older_schema_user_patch_takes_defaults() {
    Rig rig;
    rig.user.store_patch(0, kOld, sizeof kOld);
    rig.midi({ 0xB0, 0, 64, 0xC0, 0 });
    StubCooked got{};
    check(rig.slots[0].consume(got) && got.level_x10 == 600 && got.detune == 5 && got.cutoff == 1000,
          "a schema-1 user Patch loads, CUTOFF taking its descriptor default");
}

}  // namespace

int main() {
    test_program_change_commits_cooked_patch_to_part();
    test_older_schema_patch_takes_defaults_for_missing_params();
    test_patch_for_another_module_is_rejected();
    test_back_to_back_program_changes_deliver_whole_patches_latest_last();
    test_core0_view_is_the_latest_edit_buffer_cook();
    test_encoder_writes_the_stored_format();
    test_missing_factory_slot_is_reported();
    test_nrpn_sets_a_parameter_and_commits_the_recooked_patch();
    test_nrpn_value_scales_onto_the_range_and_lsb_refines();
    test_descriptor_default_cc_sets_its_parameter();
    test_module_cc_handlers_keep_precedence();
    test_unselected_or_unknown_nrpn_changes_nothing();
    test_burst_of_edits_in_one_buffer_lands_on_the_latest_value();
    test_note_on_edit_reaches_the_next_note_even_while_core1_is_behind();
    test_live_edit_reaches_core1_from_the_next_buffer();
    test_core1_tells_a_patch_load_from_an_edit();
    test_notes_know_which_patch_load_they_belong_to();
    test_bank_select_msb_picks_a_factory_bank();
    test_program_change_loads_from_the_user_bank();
    test_empty_slots_and_missing_banks_are_ignored();
    test_store_writes_the_edit_buffer_to_a_user_slot();
    test_store_without_a_user_bank_is_refused();
    test_user_slot_holding_another_modules_patch_is_rejected();
    test_older_schema_user_patch_takes_defaults();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
