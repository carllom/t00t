// Host test for the Quick-edit Page's interaction (src/patch/quick_edit.h)
// at its seam: UI commands in, the platform's Edit buffer, Compare and user
// bank -- and the Page's own view state -- out. A stub module with four
// quick-edit parameters stands in for a real engine; the user bank is on
// RAM-backed flash (ram_flash.h).

#include "patch/quick_edit.h"
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
    int32_t v[5];
};

inline constexpr const char *kWaveLabels[] = { "SINE", "SQR", "SAW", "NOISE" };

struct StubModule {
    static constexpr ModuleId kModuleId = ModuleId::SUBTRACTIVE;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = 2;
    using Cooked = StubCooked;

    static constexpr ParamDescriptor kDescriptors[] = {
        // id name      group  min  max    default unit             curve                    class               quick  cc
        { 0, "LEVEL",  "AMP",  0,   100,   50,     ParamUnit::NONE, ParamCurve::LINEAR,      ParamClass::LIVE,    true,  kNoDefaultCc },
        { 1, "WAVE",   "OSC",  0,   3,     0,      ParamUnit::ENUM, ParamCurve::LINEAR,      ParamClass::NOTE_ON, true,  kNoDefaultCc,
          kWaveLabels, 4 },
        { 2, "CUTOFF", "FILT", 20,  18000, 1000,   ParamUnit::HZ,   ParamCurve::EXPONENTIAL, ParamClass::LIVE,    true,  kNoDefaultCc },
        { 3, "DETUNE", "OSC", -100, 100,   0,      ParamUnit::NONE, ParamCurve::LINEAR,      ParamClass::NOTE_ON, false, kNoDefaultCc },
        { 4, "RESO",   "FILT", 0,   32767, 0,      ParamUnit::NONE, ParamCurve::LINEAR,      ParamClass::LIVE,    true,  kNoDefaultCc },
    };

    static void cook(const int32_t *values, Cooked &out) {
        for (int i = 0; i < 5; i++) out.v[i] = values[i];
    }
};

using Platform = PatchPlatformT<StubModule>;
using QuickEdit = QuickEditT<StubModule>;

// "DARK": level 30, wave SQR, cutoff 1000, detune 0, reso 0.
const uint8_t kDark[] = {
    'T', '0', 'P', 'T', 0x01, 0x01,
    'D', 'A', 'R', 'K', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x07, 0x00,
    30, 1, 0xD4, 0x03, 100, 0x00, 0x00,
};
const StoredPatchRef kBank[] = { { kDark, sizeof kDark } };
const FactoryBank kFactory[] = { { kBank, 1 } };

struct Rig {
    PartSlotT<StubCooked> slots[StubModule::kPartCount];
    RamFlash flash;
    UserBank user{ StubModule::kModuleId, user_flash(flash) };
    Platform platform{ slots, kFactory, 1, &user };
    QuickEdit qe{ platform };

    Rig() {
        for (uint8_t p = 0; p < StubModule::kPartCount; p++) platform.load_factory(p, 0, 0);
    }

    // Feed commands; returns how many were claimed.
    int cmd(std::initializer_list<UiCommand> commands) {
        int claimed = 0;
        for (UiCommand c : commands) claimed += qe.command(c) ? 1 : 0;
        return claimed;
    }
};

constexpr UiCommand P = UiCommand::PLUS, M = UiCommand::MINUS, E = UiCommand::ENTER, X = UiCommand::EXIT;

// --- Tests ---

void test_rows_are_the_quick_edit_parameters_then_compare_and_store() {
    static_assert(QuickEdit::kQuickCount == 4, "four quick-edit parameters");
    check(QuickEdit::kRowCount == 6, "rows: the quick-edit parameters, COMPARE, STORE");
    check(QuickEdit::param_id(0) == 0 && QuickEdit::param_id(1) == 1 && QuickEdit::param_id(2) == 2 &&
          QuickEdit::param_id(3) == 4, "parameter rows in ID order, skipping the unflagged DETUNE");
    check(QuickEdit::kCompareRow == 4 && QuickEdit::kStoreRow == 5, "COMPARE then STORE last");
}

void test_browsing_leaves_page_navigation_alone() {
    Rig rig;
    check(rig.cmd({ P, M, X }) == 0, "+, - and long press are left to Page navigation while browsing");
    check(rig.cmd({ E }) == 1 && rig.qe.mode() == QuickEditMode::SELECT && rig.qe.row() == 0,
          "a short press starts editing on the first row");
    check(rig.cmd({ M }) == 1 && rig.qe.row() == QuickEdit::kStoreRow, "rows wrap backwards");
    check(rig.cmd({ P, P }) == 2 && rig.qe.row() == 1, "and forwards");
    check(rig.cmd({ X }) == 1 && rig.qe.mode() == QuickEditMode::BROWSE, "a long press backs out to browsing");
}

void test_the_part_follows_the_last_channel_until_editing_starts() {
    Rig rig;
    rig.qe.follow_part(1);
    check(rig.qe.part() == 1, "while browsing the Page follows the last-played Part");
    rig.cmd({ E });
    rig.qe.follow_part(0);
    check(rig.qe.part() == 1, "while editing it stays on its Part");
    rig.cmd({ X });
    rig.qe.follow_part(0);
    check(rig.qe.part() == 0, "and follows again once backed out");
    rig.qe.follow_part(9);
    check(rig.qe.part() == 0, "an unknown Part is ignored");
}

void test_value_edits_step_by_curve_and_reach_the_edit_buffer() {
    Rig rig;  // DARK: level 30, wave SQR, cutoff 1000, reso 0
    rig.cmd({ E, E });  // SELECT row 0, VALUE
    check(rig.qe.mode() == QuickEditMode::VALUE, "a short press on a parameter row edits its value");
    rig.cmd({ P, P, P });
    check(rig.platform.param(0, 0) == 33, "LEVEL (range 100) steps by 1");
    StubCooked got{};
    bool published = false;
    while (rig.slots[0].consume(got)) { published = true; rig.platform.task(); }
    check(published && got.v[0] == 33 && rig.platform.edited(0), "through the Edit buffer, cooked and published");

    rig.cmd({ E, P, E, P, P, P, P });  // back to SELECT, row 1 WAVE, edit, +4
    check(rig.platform.param(0, 1) == 3, "an ENUM steps by 1 and stops at its last value");

    rig.cmd({ E, P, E, P });  // row 2 CUTOFF, +1
    check(rig.platform.param(0, 2) == 1059, "an EXPONENTIAL parameter steps by about 6% (980 above min -> +58)");
    rig.cmd({ M, M });
    check(rig.platform.param(0, 2) == 945, "and down by the same ratio");

    rig.cmd({ E, P, E, P, P });  // row 3 RESO, +2
    check(rig.platform.param(0, 4) == 512, "a wide LINEAR parameter steps by 1/128 of its range");
    rig.cmd({ X });
    check(rig.qe.mode() == QuickEditMode::SELECT, "a long press ends value editing");
}

void test_the_compare_row_toggles_compare() {
    Rig rig;
    rig.cmd({ E, E, P, E });  // LEVEL +1, back to SELECT
    rig.cmd({ M, M });        // row 4: COMPARE (0 -> 5 -> 4)
    check(rig.qe.row() == QuickEdit::kCompareRow, "on the COMPARE row");
    rig.cmd({ E });
    check(rig.platform.comparing(0), "a short press turns Compare on");
    rig.cmd({ E });
    check(!rig.platform.comparing(0), "and off again");
}

void test_store_with_rename_writes_the_chosen_user_slot() {
    Rig rig;
    rig.user.store_patch(0, kDark, sizeof kDark);  // slot 0 taken
    rig.cmd({ E, E, P, E });  // LEVEL 31
    rig.cmd({ M, E });        // STORE row
    check(rig.qe.mode() == QuickEditMode::STORE_SLOT && rig.qe.store_slot() == 1,
          "Store starts on the first empty user slot for a factory Patch");
    rig.cmd({ M, M });
    check(rig.qe.store_slot() == kUserPatchSlots - 1, "the slot wraps through the whole bank");
    rig.cmd({ P, P, P });
    check(rig.qe.store_slot() == 2, "slot 2");

    rig.cmd({ E });
    check(rig.qe.mode() == QuickEditMode::NAME_CURSOR && rig.qe.name_cursor() == 0 &&
          strcmp(rig.qe.draft(), "DARK            ") == 0, "renaming starts on the first character of the name");
    rig.cmd({ E, P, E });  // character 0: 'D' -> 'E'
    check(strcmp(rig.qe.draft(), "EARK            ") == 0, "a short press picks the character, + steps it");
    rig.cmd({ P, P, P, E, P, E });  // character 3: 'K' -> 'L'
    rig.cmd({ P, E, M });  // character 4: ' ' stepped back
    check(rig.qe.mode() == QuickEditMode::NAME_CHAR &&
          rig.qe.draft()[4] == QuickEdit::kNameChars[sizeof QuickEdit::kNameChars - 2],
          "- from a space wraps to the end of the character set");
    rig.cmd({ P, E });  // back to ' '
    for (int i = 0; i < 20; i++) rig.cmd({ P });
    check(rig.qe.name_cursor() == kPatchNameLen, "the cursor stops on OK after the last character");
    rig.cmd({ E });  // OK
    char name[kPatchNameLen + 1];
    check(rig.platform.user_slot_name(2, name) && strcmp(name, "EARL") == 0,
          "OK stores into slot 2 under the new name, trailing spaces dropped");
    check(rig.platform.param(0, 0) == 31 && !rig.platform.edited(0) && strcmp(rig.platform.name(0), "EARL") == 0,
          "the Edit buffer is now that stored Patch, unedited");
    check(rig.qe.mode() == QuickEditMode::SELECT && rig.qe.store_result() == QuickEditStore::OK,
          "back to row selection, reporting the Store");
    rig.cmd({ P });
    check(rig.qe.store_result() == QuickEditStore::NONE, "the report clears on the next command");
}

void test_store_defaults_to_the_slot_a_user_patch_came_from() {
    Rig rig;
    rig.user.store_patch(7, kDark, sizeof kDark);
    rig.platform.load_user(0, 7);
    rig.cmd({ E, M, E });
    check(rig.qe.store_slot() == 7, "a user Patch is stored back to its own slot by default");
}

void test_backing_out_of_store_stores_nothing() {
    Rig rig;
    rig.cmd({ E, M, E, E, E, P });  // STORE, slot, name: changing 'D'
    rig.cmd({ X });
    check(rig.qe.mode() == QuickEditMode::SELECT && rig.flash.writes.empty(),
          "a long press while naming cancels the Store in one step");
    check(strcmp(rig.platform.name(0), "DARK") == 0 && !rig.platform.edited(0), "and the name is unchanged");
    rig.cmd({ E, X });  // STORE again, long press while choosing the slot
    check(rig.qe.mode() == QuickEditMode::SELECT && rig.flash.writes.empty(), "so does one while choosing the slot");
}

void test_a_load_during_the_store_dialog_closes_it() {
    Rig rig;
    rig.cmd({ E, M, E, E });  // STORE, slot, naming
    rig.platform.load_factory(0, 0, 0);  // a Program Change arrives
    rig.qe.follow_part(0);
    check(rig.qe.mode() == QuickEditMode::SELECT, "a load into the Part closes the Store dialog");
    rig.cmd({ E, E });
    rig.platform.load_factory(0, 0, 0);
    rig.cmd({ P });
    check(rig.qe.mode() == QuickEditMode::SELECT && rig.flash.writes.empty(),
          "even before the display notices: the next command finds it closed and stores nothing");
}

void test_while_comparing_the_rows_show_what_is_heard() {
    Rig rig;
    rig.cmd({ E, E, P, P, E });  // LEVEL 32
    rig.platform.set_compare(0, true);
    check(QuickEdit::shown_param(rig.platform, 0, 0) == 30, "while comparing, a row shows the stored value");
    rig.platform.set_compare(0, false);
    check(QuickEdit::shown_param(rig.platform, 0, 0) == 32, "otherwise the edited one");
}

void test_a_failed_store_is_reported_and_keeps_the_name() {
    Rig rig;
    rig.flash.fail = true;
    rig.cmd({ E, M, E, E, E, P, E });  // STORE, slot 0, rename 'D' -> 'E'
    for (int i = 0; i < 20; i++) rig.cmd({ P });
    rig.cmd({ E });
    check(rig.qe.store_result() == QuickEditStore::FAILED, "a failed Store is reported");
    check(strcmp(rig.platform.name(0), "DARK") == 0, "and the Edit buffer keeps its old name");
}

void test_values_read_in_their_units() {
    char out[24];
    const ParamDescriptor hz = { 0, "F", "G", -18000, 18000, 0, ParamUnit::HZ, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc };
    const ParamDescriptor chz = { 0, "R", "G", 0, 2000, 0, ParamUnit::CENTI_HZ, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc };
    const ParamDescriptor pm = { 0, "D", "G", 0, 1000, 0, ParamUnit::PERMILLE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc };
    param_format(hz, 1234, out, sizeof out);
    bool a = strcmp(out, "1234Hz") == 0;
    param_format(hz, -12500, out, sizeof out);
    bool b = strcmp(out, "-12.5k") == 0;
    param_format(chz, 250, out, sizeof out);
    bool c = strcmp(out, "2.50Hz") == 0;
    param_format(pm, 355, out, sizeof out);
    bool d = strcmp(out, "35.5%") == 0;
    param_format(StubModule::kDescriptors[1], 2, out, sizeof out);
    bool e = strcmp(out, "SAW") == 0;
    param_format(StubModule::kDescriptors[4], 512, out, sizeof out);
    bool f = strcmp(out, "512") == 0;
    check(a && b && c && d && e && f, "values read as 1234Hz, -12.5k, 2.50Hz, 35.5%, an ENUM label, a plain number");
}

}  // namespace

int main() {
    test_rows_are_the_quick_edit_parameters_then_compare_and_store();
    test_browsing_leaves_page_navigation_alone();
    test_the_part_follows_the_last_channel_until_editing_starts();
    test_value_edits_step_by_curve_and_reach_the_edit_buffer();
    test_the_compare_row_toggles_compare();
    test_store_with_rename_writes_the_chosen_user_slot();
    test_store_defaults_to_the_slot_a_user_patch_came_from();
    test_backing_out_of_store_stores_nothing();
    test_a_failed_store_is_reported_and_keeps_the_name();
    test_a_load_during_the_store_dialog_closes_it();
    test_while_comparing_the_rows_show_what_is_heard();
    test_values_read_in_their_units();
    printf(g_ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return g_ok ? 0 : 1;
}
