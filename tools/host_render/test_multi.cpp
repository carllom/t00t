// Host test for the Multi (src/patch/multi.h) at the platform seam: MIDI
// channel events into a Multi over a PatchPlatformT, its Parts' Patches,
// its cooked mix/FX on the Core 1 side of the Multi slot, and Multis stored
// into and loaded from user-bank Multi slots on RAM-backed flash
// (ram_flash.h). A stub module with four Parts stands in for a real engine.
//
// Stored-Multi byte vectors are written out literally, so the test pins the
// stored format itself.

#include "patch/multi.h"
#include "patch/multi_edit.h"
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

struct StubCooked {
    int32_t level;
};

struct StubModule {
    static constexpr ModuleId kModuleId = ModuleId::SUBTRACTIVE;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = 4;
    using Cooked = StubCooked;

    static constexpr ParamDescriptor kDescriptors[] = {
        { 0, "LEVEL", "AMP", 0, 100, 50, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, true, 20 },
    };

    static void cook(const int32_t *v, Cooked &out) { out.level = v[0]; }

    // The default Multi's Patch for each Part, from factory bank 0.
    static constexpr uint8_t default_program(uint32_t part) { return (uint8_t)(part % 2); }
};

using Platform = PatchPlatformT<StubModule>;
using Multi = MultiT<StubModule>;
using MultiCooked = Multi::Cooked;

// Patches: A (level 10), B (level 20) in factory bank 0; C (level 30) in bank 1.
#define STUB_PATCH(nm, c0, c1, lvl) { 'T', '0', 'P', 'T', 0x01, 0x01, nm, c0, c1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, lvl }
const uint8_t kA[] = STUB_PATCH('A', 0, 0, 10);
const uint8_t kB[] = STUB_PATCH('B', 0, 0, 20);
const uint8_t kC[] = STUB_PATCH('C', 0, 0, 30);
const StoredPatchRef kBank0[] = { { kA, sizeof kA }, { kB, sizeof kB } };
const StoredPatchRef kBank1[] = { { kC, sizeof kC } };
const FactoryBank kFactory[] = { { kBank0, 2 }, { kBank1, 1 } };

struct Rig {
    PartSlotT<StubCooked> slots[StubModule::kPartCount];
    PartSlotT<MultiCooked> multi_slot;
    RamFlash flash;
    UserBank user{ StubModule::kModuleId, user_flash(flash) };
    Platform platform{ slots, kFactory, 2, &user };
    Multi multi{ platform, &user, multi_slot };

    Rig() { multi.init(); }

    // Core 1's side: the newest cooked Multi, if one was published.
    bool take(MultiCooked &out) {
        bool got = false;
        while (multi_slot.consume(out)) { got = true; multi.task(); }
        return got;
    }
    int32_t level(uint8_t part) const { return platform.cooked(part).level; }
};

constexpr uint32_t ch(uint8_t c) { return 1u << c; }

// --- Tests ---

void test_the_default_multi_puts_part_n_on_channel_n() {
    Rig rig;
    check(rig.multi.parts_on_channel(0) == ch(0) && rig.multi.parts_on_channel(3) == ch(3),
          "Part n listens on channel n");
    check(rig.multi.parts_on_channel(4) == 0, "a channel past the Parts reaches none");
    check(rig.level(0) == 10 && rig.level(1) == 20 && rig.level(2) == 10, "each Part plays its default Patch");
    check(rig.multi.part_param(1, PART_P_BANK) == 0 && rig.multi.part_param(1, PART_P_PROGRAM) == 1,
          "and refers to it by bank and program");

    MultiCooked got{};
    check(rig.take(got), "the cooked Multi is published");
    check(got.fx.type == FX_DELAY && got.fx.mix == 0 && got.fx.p1 == 55 && got.fx.p2 == 36,
          "FX: delay, dry, the descriptor defaults");
    check(got.master_q15 == 32768 && got.part[2].volume_q15 == 32768 && got.part[2].send_q15 == 32768 &&
          got.part[2].pan_q15 == 0, "master, volume and send at unity, pan centred");
}

void test_program_change_loads_every_part_on_the_channel() {
    Rig rig;
    rig.multi.set_part(1, PART_P_CHANNEL, 0);  // layer Part 1 onto channel 1 (index 0)
    check(rig.multi.parts_on_channel(0) == (ch(0) | ch(1)) && rig.multi.parts_on_channel(1) == 0,
          "moving Part 1 to channel 0 layers it with Part 0");
    rig.multi.program_change(0, 1, 0);  // bank 1, program 0: C
    check(rig.level(0) == 30 && rig.level(1) == 30 && rig.level(2) == 10, "Program Change loads both layered Parts");
    check(rig.multi.part_param(0, PART_P_BANK) == 1 && rig.multi.part_param(1, PART_P_PROGRAM) == 0,
          "and the Parts now refer to bank 1, program 0");
    rig.multi.program_change(0, 0, 99);
    check(rig.level(0) == 30 && rig.multi.part_param(0, PART_P_PROGRAM) == 0,
          "a Program Change to a missing Patch changes nothing");
}

void test_a_part_switched_off_hears_no_channel() {
    Rig rig;
    rig.multi.set_part(3, PART_P_CHANNEL, kPartChannelOff);
    check(rig.multi.parts_on_channel(3) == 0, "a Part on OFF listens to no channel");
}

void test_program_change_never_changes_the_multi_fx() {
    Rig rig;
    rig.multi.set_common(COMMON_P_FX_TYPE, FX_REVERB);
    rig.multi.set_common(COMMON_P_FX_MIX, 90);
    MultiCooked before{}, after{};
    rig.take(before);
    rig.multi.program_change(0, 1, 0);
    rig.platform.load_factory(1, 0, 1);
    const bool republished = rig.take(after);
    check(!republished || (after.fx.type == FX_REVERB && after.fx.mix == 90), "Patch loads leave the FX alone");
    check(rig.multi.common(COMMON_P_FX_TYPE) == FX_REVERB && rig.multi.common(COMMON_P_FX_MIX) == 90,
          "the FX belong to the Multi");
}

void test_control_changes_reach_the_parts_on_the_channel() {
    Rig rig;
    rig.multi.set_part(1, PART_P_CHANNEL, 0);
    rig.multi.control_change(0, 7, 64);    // volume
    rig.multi.control_change(0, 10, 0);    // pan hard left
    rig.multi.control_change(0, 91, 0);    // FX send off
    rig.multi.control_change(0, 20, 127);  // the stub's LEVEL default CC: a Patch edit
    MultiCooked got{};
    rig.take(got);
    check(got.part[0].volume_q15 == 16513 && got.part[1].volume_q15 == 16513 && got.part[2].volume_q15 == 32768,
          "CC7 sets the volume of every Part on the channel (64 -> 16513)");
    check(got.part[0].pan_q15 == -32768 && got.part[1].pan_q15 == -32768, "CC10 sets their pan");
    check(got.part[0].send_q15 == 0 && got.part[1].send_q15 == 0, "CC91 sets their FX send");
    check(rig.level(0) == 100 && rig.level(1) == 100 && rig.level(2) == 10,
          "any other CC edits the Patch of every Part on the channel");
}

void test_a_multi_stores_and_loads_back() {
    Rig rig;
    rig.multi.set_part(1, PART_P_CHANNEL, 0);
    rig.multi.program_change(0, 1, 0);           // Parts 0, 1: C
    rig.multi.set_part(2, PART_P_VOLUME, 100);
    rig.multi.set_common(COMMON_P_FX_TYPE, FX_CHORUS);
    rig.multi.rename("MY MULTI");
    check(rig.multi.store(3) == StoreResult::OK, "Store into Multi slot 3");
    char name[kPatchNameLen + 1];
    check(rig.multi.user_slot_name(3, name) && strcmp(name, "MY MULTI") == 0, "the slot holds it by name");

    // Change everything, then load it back.
    rig.multi.set_part(1, PART_P_CHANNEL, 1);
    rig.multi.program_change(0, 0, 1);
    rig.multi.set_part(2, PART_P_VOLUME, 5);
    rig.multi.set_common(COMMON_P_FX_TYPE, FX_OFF);
    rig.multi.rename("OTHER");
    check(rig.multi.load(3) == PatchLoadResult::OK, "load Multi slot 3");
    check(rig.multi.parts_on_channel(0) == (ch(0) | ch(1)), "the channel routing comes back");
    check(rig.level(0) == 30 && rig.level(1) == 30, "each Part's Patch is loaded again by its reference");
    MultiCooked got{};
    rig.take(got);
    check(got.part[2].volume_q15 == level_q15(100) && got.fx.type == FX_CHORUS, "and so do the mix and the FX");
    check(strcmp(rig.multi.name(), "MY MULTI") == 0 && rig.multi.origin_slot() == 3, "with its name and slot");
    check(rig.multi.load(4) == PatchLoadResult::NO_SUCH_PATCH, "an empty Multi slot loads nothing");
}

void test_a_part_whose_patch_is_gone_falls_back_to_the_first_factory_patch() {
    Rig rig;
    rig.user.store_patch(5, kB, sizeof kB);
    rig.multi.program_change(2, 64, 5);  // Part 2 <- user slot 5
    check(rig.level(2) == 20 && rig.multi.part_param(2, PART_P_BANK) == 64, "Part 2 plays user slot 5");
    rig.multi.store(0);
    uint8_t erased[kUserPatchSlotSize];
    memset(erased, 0xFF, sizeof erased);
    rig.flash.poke(user_patch_slot_offset(StubModule::kModuleId, 5), erased, sizeof erased);
    rig.multi.program_change(2, 0, 1);  // B, so the fallback is visible
    rig.multi.load(0);
    check(rig.level(2) == 10 && rig.multi.part_param(2, PART_P_BANK) == 0 && rig.multi.part_param(2, PART_P_PROGRAM) == 0,
          "with the user Patch gone, Part 2 gets factory bank 0, slot 0");
}

// "OLD": a Multi from schema 1 with only two Parts, no MASTER and no SEND.
// Payload: common bytes 4 (FX TYPE reverb, MIX 40, P1 55, P2 36), 2 Parts of
// 5 bytes each (channel, bank, program, volume, pan).
const uint8_t kOldMulti[] = {
    'T', '0', 'M', 'T', 0x01, 0x01,
    'O', 'L', 'D', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    17, 0x00,
    4, FX_REVERB, 40, 55, 36,
    2, 5,
    0, 0, 1, 100, 64,
    0, 1, 0, 127, 0,
};

void test_an_older_multi_loads_with_defaults() {
    Rig rig;
    rig.flash.poke(user_multi_slot_offset(StubModule::kModuleId, 7), kOldMulti, sizeof kOldMulti);
    check(rig.multi.load(7) == PatchLoadResult::OK, "a Multi stored by an older schema loads");
    check(rig.multi.parts_on_channel(0) == (ch(0) | ch(1)) && rig.level(0) == 20 && rig.level(1) == 30,
          "its two Parts come back: both on channel 1, B and C");
    MultiCooked got{};
    rig.take(got);
    check(got.fx.type == FX_REVERB && got.fx.mix == 40 && got.master_q15 == 32768,
          "its FX load, MASTER takes its default");
    check(got.part[0].volume_q15 == level_q15(100) && got.part[1].pan_q15 == -32768 && got.part[0].send_q15 == 32768,
          "Part values load, SEND takes its default");
    check(rig.multi.parts_on_channel(2) == ch(2) && rig.multi.parts_on_channel(3) == ch(3) && rig.level(3) == 20,
          "Parts the Multi doesn't have get the default Multi's");
}

void test_another_modules_multi_is_rejected() {
    Rig rig;
    uint8_t foreign[sizeof kOldMulti];
    memcpy(foreign, kOldMulti, sizeof foreign);
    foreign[4] = (uint8_t)ModuleId::FM;
    rig.flash.poke(user_multi_slot_offset(StubModule::kModuleId, 7), foreign, sizeof foreign);
    check(rig.multi.load(7) == PatchLoadResult::WRONG_MODULE, "a Multi for another module is rejected");
    check(rig.multi.parts_on_channel(1) == ch(1) && rig.level(1) == 20, "and the Multi is unchanged");
}

void test_the_stored_format_round_trips_through_encode() {
    Rig rig;
    rig.multi.set_part(3, PART_P_CHANNEL, kPartChannelOff);
    rig.multi.set_part(0, PART_P_PAN, 10);
    uint8_t buf[kUserMultiSlotSize];
    const uint32_t n = rig.multi.encode(buf, sizeof buf);
    check(n > kPatchHeaderSize && memcmp(buf, "T0MT", 4) == 0, "encode writes a stored Multi");
    Rig other;
    check(other.multi.load_stored(StoredPatchRef{ buf, n }) == PatchLoadResult::OK &&
          other.multi.parts_on_channel(3) == 0 && other.multi.part_param(0, PART_P_PAN) == 10,
          "and another Multi loads it to the same state");
    check(rig.multi.encode(buf, 10) == 0, "encode refuses a too-small buffer");
}

// --- The MULTI Page: UI commands into Multi load / store / rename ---

using MultiEdit = MultiEditT<StubModule>;
constexpr UiCommand P = UiCommand::PLUS, M = UiCommand::MINUS, E = UiCommand::ENTER, X = UiCommand::EXIT;

int cmd(MultiEdit &me, std::initializer_list<UiCommand> commands) {
    int claimed = 0;
    for (UiCommand c : commands) claimed += me.command(c) ? 1 : 0;
    return claimed;
}

void test_the_multi_page_leaves_navigation_alone_until_entered() {
    Rig rig;
    MultiEdit me(rig.multi);
    check(cmd(me, { P, M, X }) == 0, "MULTI Page: +, - and long press are Page navigation while browsing");
    check(cmd(me, { E }) == 1 && me.mode() == MultiEditMode::SELECT && me.row() == MultiEdit::kLoadRow,
          "a short press selects the LOAD row");
    check(cmd(me, { P }) == 1 && me.row() == MultiEdit::kStoreRow && cmd(me, { P }) == 1 && me.row() == MultiEdit::kLoadRow,
          "+ toggles between LOAD and STORE");
    check(cmd(me, { X }) == 1 && me.mode() == MultiEditMode::BROWSE, "a long press backs out");
}

void test_load_from_the_multi_page() {
    Rig rig;
    rig.multi.set_common(COMMON_P_FX_TYPE, FX_PHASER);
    rig.multi.store(2);
    rig.multi.set_common(COMMON_P_FX_TYPE, FX_OFF);
    MultiEdit me(rig.multi);
    cmd(me, { E, E });  // LOAD
    check(me.mode() == MultiEditMode::LOAD_SLOT && me.slot() == 2, "LOAD starts on the Multi's own slot");
    cmd(me, { E });
    check(rig.multi.common(COMMON_P_FX_TYPE) == FX_PHASER && me.report() == MultiEditReport::LOADED &&
          me.mode() == MultiEditMode::SELECT, "a short press loads it and reports so");

    cmd(me, { E, P, E });  // LOAD slot 3: empty
    check(me.report() == MultiEditReport::EMPTY && rig.multi.common(COMMON_P_FX_TYPE) == FX_PHASER,
          "an empty slot is reported and loads nothing");
}

void test_load_without_an_own_slot_starts_on_the_first_stored_multi() {
    Rig rig;
    rig.multi.store(6);
    rig.multi.init();  // no slot of its own now
    MultiEdit me(rig.multi);
    cmd(me, { E, E });
    check(me.slot() == 6, "LOAD starts on the first slot holding a Multi");
}

void test_store_and_rename_from_the_multi_page() {
    Rig rig;
    rig.multi.store(0);
    rig.multi.init();
    rig.multi.set_part(2, PART_P_VOLUME, 77);
    MultiEdit me(rig.multi);
    cmd(me, { E, P, E });  // STORE
    check(me.mode() == MultiEditMode::STORE_SLOT && me.slot() == 1, "STORE starts on the first empty Multi slot");
    cmd(me, { E });
    check(me.mode() == MultiEditMode::NAME_CURSOR && strcmp(me.draft(), "INIT            ") == 0,
          "then the name, starting from the Multi's");
    cmd(me, { E, M, E });  // 'I' -> 'H'
    for (int i = 0; i < 20; i++) cmd(me, { P });
    cmd(me, { E });
    char name[kPatchNameLen + 1];
    check(rig.multi.user_slot_name(1, name) && strcmp(name, "HNIT") == 0 && rig.multi.origin_slot() == 1,
          "OK stores it into slot 1 under the new name");
    check(me.report() == MultiEditReport::STORED && me.mode() == MultiEditMode::SELECT, "and reports so");

    cmd(me, { E, E, E, P, X });  // STORE again, rename, cancel
    check(me.mode() == MultiEditMode::SELECT && strcmp(rig.multi.name(), "HNIT") == 0, "a long press cancels the Store");
}

void test_nrpn_addresses_part_and_multi_parameters() {
    Rig rig;
    rig.multi.set_part(1, PART_P_CHANNEL, 0);
    // NRPN MSB 126: a Part parameter (LSB = its ID) on the channel's Parts.
    rig.multi.control_change(0, 99, 126);
    rig.multi.control_change(0, 98, PART_P_SEND);
    rig.multi.control_change(0, 6, 0);
    // NRPN MSB 127: one of the Multi's own settings, from any channel.
    rig.multi.control_change(5, 99, 127);
    rig.multi.control_change(5, 98, COMMON_P_MASTER);
    rig.multi.control_change(5, 6, 64);
    MultiCooked got{};
    rig.take(got);
    check(got.part[0].send_q15 == 0 && got.part[1].send_q15 == 0 && got.part[2].send_q15 == 32768,
          "NRPN 126/ID sets a Part parameter on every Part on the channel");
    check(rig.multi.common(COMMON_P_MASTER) == 64, "NRPN 127/ID sets one of the Multi's own settings");
    rig.multi.control_change(0, 99, 0);
    rig.multi.control_change(0, 98, 0);
    rig.multi.control_change(0, 6, 127);
    check(rig.level(0) == 100 && rig.level(1) == 100, "a Patch parameter's NRPN still edits the Patches");
}

}  // namespace

int main() {
    test_the_default_multi_puts_part_n_on_channel_n();
    test_program_change_loads_every_part_on_the_channel();
    test_a_part_switched_off_hears_no_channel();
    test_program_change_never_changes_the_multi_fx();
    test_control_changes_reach_the_parts_on_the_channel();
    test_a_multi_stores_and_loads_back();
    test_a_part_whose_patch_is_gone_falls_back_to_the_first_factory_patch();
    test_an_older_multi_loads_with_defaults();
    test_another_modules_multi_is_rejected();
    test_the_stored_format_round_trips_through_encode();
    test_nrpn_addresses_part_and_multi_parameters();
    test_the_multi_page_leaves_navigation_alone_until_entered();
    test_load_from_the_multi_page();
    test_load_without_an_own_slot_starts_on_the_first_stored_multi();
    test_store_and_rename_from_the_multi_page();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
