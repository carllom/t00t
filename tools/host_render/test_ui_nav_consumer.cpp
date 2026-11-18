// Host-buildable unit test for src/wslcd/ui_command_shaping.h and
// src/wslcd/ui_nav_consumer.h (issue #132): the SensorEvent -> Shaping ->
// UI-navigation consumer path that drives #126's PageCursor state machine
// directly from a raw Sensor event, diverging before input_dispatch()/the
// Router so a UI command never reaches a module Handler. Pure logic, no LCD
// stub dependency -- same convention as test_page_cursor.cpp /
// test_button_shaping.cpp (test_*() functions, an aggregated `bool ok`,
// "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/ui_nav_consumer.h"

#include "../../src/button_shaping.h"

#include <cstdio>

namespace {

constexpr UiNavConfig kPlusButton = { { UiCommand::PLUS }, /*page_count=*/4, /*performance_index=*/0 };
constexpr UiNavConfig kExitButton = { { UiCommand::EXIT }, /*page_count=*/4, /*performance_index=*/2 };
constexpr UiNavConfig kEnterButton = { { UiCommand::ENTER }, /*page_count=*/4, /*performance_index=*/0 };
constexpr UiNavConfig kSinglePageModule = { { UiCommand::PLUS }, /*page_count=*/1, /*performance_index=*/0 };

bool test_press_advances_cursor_via_shaping() {
    PageCursor cursor;
    cursor.index = 1;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_button(0, true), kPlusButton);

    bool ok = fired && cursor.index == 2;
    printf(ok ? "  OK: a raw Sensor event press Shapes into PLUS and advances the cursor\n"
              : "  FAIL: press did not advance the cursor as expected\n");
    return ok;
}

bool test_release_produces_no_command() {
    PageCursor cursor;
    cursor.index = 1;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_button(0, false), kPlusButton);

    bool ok = !fired && cursor.index == 1;
    printf(ok ? "  OK: a release edge fires no UI command and leaves the cursor untouched\n"
              : "  FAIL: release edge unexpectedly fired a command or moved the cursor\n");
    return ok;
}

bool test_pot_event_produces_no_command() {
    PageCursor cursor;
    cursor.index = 1;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_pot(0, 2048), kPlusButton);

    bool ok = !fired && cursor.index == 1;
    printf(ok ? "  OK: a non-button Sensor event fires no UI command\n"
              : "  FAIL: a pot reading unexpectedly fired a UI command\n");
    return ok;
}

bool test_exit_jumps_to_performance_page() {
    PageCursor cursor;
    cursor.index = 3;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_button(1, true), kExitButton);

    bool ok = fired && cursor.index == 2;
    printf(ok ? "  OK: EXIT drives the cursor straight to the Performance page\n"
              : "  FAIL: EXIT did not land on the Performance page\n");
    return ok;
}

bool test_enter_is_recognized_but_noop() {
    PageCursor cursor;
    cursor.index = 1;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_button(2, true), kEnterButton);

    bool ok = fired && cursor.index == 1;
    printf(ok ? "  OK: ENTER fires as a recognized command but leaves the cursor untouched\n"
              : "  FAIL: ENTER either failed to fire or moved the cursor\n");
    return ok;
}

bool test_single_page_module_never_advances() {
    PageCursor cursor;
    bool fired = ui_nav_consumer_feed(cursor, sensor_event_button(3, true), kSinglePageModule);

    bool ok = fired && cursor.index == 0;
    printf(ok ? "  OK: a Performance-only module's PLUS fires but never leaves its single Page\n"
              : "  FAIL: a Performance-only module moved off its single Page\n");
    return ok;
}

struct TestContext {
    int calls = 0;
};

void set_note(TestContext &ctx, const InputValue &) {
    ctx.calls++;
}

constexpr InputMapEntryT<TestContext> kTable[] = {
    { InputCategory::NOTE, 0, 0, 0xFF, 0, set_note },
};

bool test_ui_command_never_reaches_a_module_handler() {
    TestContext ctx;
    SensorEvent ev = sensor_event_button(0, true);

    // The same raw Sensor event, run through the ordinary NOTE-Shaping ->
    // Router path (button_shaping.h + input_dispatch()), does reach the
    // Handler -- confirming kTable/set_note is a live, callable stand-in,
    // not simply unreferenced code.
    ButtonShapingConfig note_shaping = { /*note*/ 0, /*channel*/ 0, /*fixed_velocity*/ 100 };
    InputValue value = shape_button_event(ev, note_shaping);
    input_dispatch(ctx, kTable, InputCategory::NOTE, note_shaping.note, value);
    bool note_path_reached_handler = ctx.calls == 1;

    // The same event, run through the UI-navigation consumer instead, must
    // add no further Handler call.
    PageCursor cursor;
    ui_nav_consumer_feed(cursor, ev, kPlusButton);

    bool ok = note_path_reached_handler && ctx.calls == 1;
    printf(ok ? "  OK: the same Sensor event reaches a Handler via the NOTE path but never via the UI-nav path\n"
              : "  FAIL: UI-nav consumer path reached the Handler (or the NOTE-path control case itself failed)\n");
    return ok;
}

// --- Press-duration Shaping: one button, two commands ---

// Feeds `ms` 1 ms ticks of `pressed`; returns the commands fired, in order.
int run_press(PressShaper &s, bool pressed, int ms, UiCommand *fired, int cap) {
    int n = 0;
    for (int i = 0; i < ms; i++) {
        UiCommand c;
        if (shape_press_tick(s, pressed, &c) && n < cap) fired[n++] = c;
    }
    return n;
}

bool test_short_press_fires_enter_on_release() {
    PressShaper s;
    UiCommand fired[4];
    int during = run_press(s, true, 100, fired, 4);
    int after = run_press(s, false, 5, fired, 4);
    bool ok = during == 0 && after == 1 && fired[0] == UiCommand::ENTER;
    printf(ok ? "  OK: a short press fires ENTER on release, nothing while held\n"
              : "  FAIL: short press did not fire exactly one ENTER on release\n");
    return ok;
}

bool test_long_press_fires_exit_once_while_held() {
    PressShaper s;
    UiCommand fired[4];
    int early = run_press(s, true, kLongPressMs - 1, fired, 4);
    int at = run_press(s, true, 1, fired, 4);
    bool exit_fired = at == 1 && fired[0] == UiCommand::EXIT;
    int later = run_press(s, true, 2000, fired, 4) + run_press(s, false, 5, fired, 4);
    bool ok = early == 0 && exit_fired && later == 0;
    printf(ok ? "  OK: holding fires EXIT once at the long-press threshold, and nothing on release\n"
              : "  FAIL: long press did not fire exactly one EXIT at the threshold\n");
    return ok;
}

// --- A claiming interaction context takes precedence over Page navigation ---

int g_claimed = 0;
bool claim_all(UiCommand) { g_claimed++; return true; }
bool claim_none(UiCommand) { return false; }

bool test_claimed_command_leaves_the_page_cursor() {
    PageCursor cursor;
    cursor.index = 1;
    g_claimed = 0;
    ui_nav_consumer_feed(cursor, sensor_event_button(0, true), kPlusButton, claim_all);
    bool claimed_ok = cursor.index == 1 && g_claimed == 1;
    ui_nav_consumer_feed(cursor, sensor_event_button(0, true), kPlusButton, claim_none);
    bool fallback_ok = cursor.index == 2;
    ui_nav_consumer_apply(cursor, UiCommand::EXIT, 4, 0, claim_none);
    bool apply_ok = cursor.index == 0;
    bool ok = claimed_ok && fallback_ok && apply_ok;
    printf(ok ? "  OK: a claimed command never moves the cursor; an unclaimed one falls back to Page navigation\n"
              : "  FAIL: claim did not take precedence over Page navigation\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== a raw Sensor event drives the cursor end to end ==\n");
    ok = test_press_advances_cursor_via_shaping() && ok;
    printf("\n== only a press edge fires a command ==\n");
    ok = test_release_produces_no_command() && ok;
    ok = test_pot_event_produces_no_command() && ok;
    printf("\n== EXIT/ENTER semantics ==\n");
    ok = test_exit_jumps_to_performance_page() && ok;
    ok = test_enter_is_recognized_but_noop() && ok;
    printf("\n== a Performance-only module never advances ==\n");
    ok = test_single_page_module_never_advances() && ok;
    printf("\n== divergence before the Router ==\n");
    ok = test_ui_command_never_reaches_a_module_handler() && ok;

    printf("\n== press duration: short = ENTER, long = EXIT ==\n");
    ok = test_short_press_fires_enter_on_release() && ok;
    ok = test_long_press_fires_exit_once_while_held() && ok;
    printf("\n== an interaction context claims commands first ==\n");
    ok = test_claimed_command_leaves_the_page_cursor() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
