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

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
