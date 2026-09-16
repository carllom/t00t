// Host-buildable unit test for src/wslcd/page.h (issue #126): the shared
// Page-navigation cursor state machine. Pure logic, no LCD stub dependency
// -- same rationale and convention as test_input_layer.cpp's no-arg
// invocation mode (test_*() functions, an aggregated `bool ok`, "ALL
// CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/page.h"

#include <cstdio>

namespace {

bool test_plus_advances_by_one() {
    PageCursor cursor;
    cursor.index = 1;
    page_cursor_apply(cursor, UiCommand::PLUS, /*page_count=*/4, /*performance_index=*/0);

    bool ok = cursor.index == 2;
    printf(ok ? "  OK: PLUS advances the cursor by one\n"
              : "  FAIL: PLUS did not advance to the expected index\n");
    return ok;
}

bool test_minus_retreats_by_one() {
    PageCursor cursor;
    cursor.index = 2;
    page_cursor_apply(cursor, UiCommand::MINUS, /*page_count=*/4, /*performance_index=*/0);

    bool ok = cursor.index == 1;
    printf(ok ? "  OK: MINUS retreats the cursor by one\n"
              : "  FAIL: MINUS did not retreat to the expected index\n");
    return ok;
}

bool test_plus_wraps_past_last() {
    PageCursor cursor;
    cursor.index = 3;  // last index of a 4-Page list
    page_cursor_apply(cursor, UiCommand::PLUS, /*page_count=*/4, /*performance_index=*/0);

    bool ok = cursor.index == 0;
    printf(ok ? "  OK: PLUS past the last Page wraps to the first\n"
              : "  FAIL: PLUS at the last index did not wrap to 0\n");
    return ok;
}

bool test_minus_wraps_past_first() {
    PageCursor cursor;
    cursor.index = 0;
    page_cursor_apply(cursor, UiCommand::MINUS, /*page_count=*/4, /*performance_index=*/0);

    bool ok = cursor.index == 3;
    printf(ok ? "  OK: MINUS past the first Page wraps to the last\n"
              : "  FAIL: MINUS at index 0 did not wrap to the last index\n");
    return ok;
}

bool test_exit_jumps_to_performance_regardless_of_position() {
    PageCursor cursor_near, cursor_far;
    cursor_near.index = 1;
    cursor_far.index = 3;

    page_cursor_apply(cursor_near, UiCommand::EXIT, /*page_count=*/4, /*performance_index=*/2);
    page_cursor_apply(cursor_far, UiCommand::EXIT, /*page_count=*/4, /*performance_index=*/2);

    bool ok = cursor_near.index == 2 && cursor_far.index == 2;
    printf(ok ? "  OK: EXIT jumps straight to the Performance index from any position\n"
              : "  FAIL: EXIT did not land on the Performance index\n");
    return ok;
}

bool test_enter_is_noop() {
    PageCursor cursor;
    cursor.index = 1;
    page_cursor_apply(cursor, UiCommand::ENTER, /*page_count=*/4, /*performance_index=*/0);

    bool ok = cursor.index == 1;
    printf(ok ? "  OK: ENTER produces no cursor state change\n"
              : "  FAIL: ENTER unexpectedly moved the cursor\n");
    return ok;
}

bool test_performance_only_module_never_advances() {
    PageCursor cursor;  // starts at index 0, the module's only (Performance) Page

    page_cursor_apply(cursor, UiCommand::PLUS, /*page_count=*/1, /*performance_index=*/0);
    bool plus_ok = cursor.index == 0;

    page_cursor_apply(cursor, UiCommand::MINUS, /*page_count=*/1, /*performance_index=*/0);
    bool minus_ok = cursor.index == 0;

    bool ok = plus_ok && minus_ok;
    printf(ok ? "  OK: a Performance-only Page list never advances on PLUS/MINUS\n"
              : "  FAIL: a Performance-only Page list moved off its single Page\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== PLUS/MINUS advance/retreat the cursor by one ==\n");
    ok = test_plus_advances_by_one() && ok;
    ok = test_minus_retreats_by_one() && ok;
    printf("\n== wraparound at both ends ==\n");
    ok = test_plus_wraps_past_last() && ok;
    ok = test_minus_wraps_past_first() && ok;
    printf("\n== EXIT jumps to the Performance page ==\n");
    ok = test_exit_jumps_to_performance_regardless_of_position() && ok;
    printf("\n== ENTER is a no-op ==\n");
    ok = test_enter_is_noop() && ok;
    printf("\n== a Performance-only Page list never advances ==\n");
    ok = test_performance_only_module_never_advances() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
