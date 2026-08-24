// Host-buildable unit test for src/wslcd/activity_grid.h (issue #130): the
// ActivityGrid Widget's fixed-geometry cell drawing and per-cell latch.
// Compiles the real, unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's
// in-memory framebuffer, same convention as test_value_row.cpp (test_*()
// functions, an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/activity_grid.h"
#include "lcd_st7789.h"

#include <cstdio>

namespace {

// True if every pixel in the panel matches: cell i (0..n-1) is `on_color`
// if `active[i]`, `off_color` otherwise, laid out at the fixed pitch/wrap
// rooted at (x, y); everywhere else is `bg`.
bool grid_matches(int n, const bool *active, int x, int y, int row_pitch, uint16_t on_color,
                   uint16_t off_color, uint16_t bg) {
    for (int py = 0; py < LCD_H; py++) {
        for (int px = 0; px < LCD_W; px++) {
            // activity_grid_draw() draws cells 0..n-1 in order, so if two
            // cells' rects ever overlap (e.g. row_pitch < kActivityGridCellH),
            // the higher index wins -- keep scanning through n rather than
            // stopping at the first match, so `want` ends on that same cell.
            uint16_t want = bg;
            for (int i = 0; i < n; i++) {
                int cx = x + (i % kActivityGridCellsPerRow) * kActivityGridCellPitch;
                int cy = y + (i / kActivityGridCellsPerRow) * row_pitch;
                if (px >= cx && px < cx + kActivityGridCellW && py >= cy &&
                    py < cy + kActivityGridCellH) {
                    want = active[i] ? on_color : off_color;
                }
            }
            if (lcd_stub_fb[py * LCD_W + px] != want) {
                printf("  FAIL: pixel (%d,%d) = 0x%04x, want 0x%04x\n", px, py,
                       lcd_stub_fb[py * LCD_W + px], want);
                return false;
            }
        }
    }
    return true;
}

bool test_initial_draw(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t on = gfx_rgb(60, 220, 90);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int x = 1, y = 56, row_pitch = 18;
    const int N = 16;

    bool active[N] = {};
    for (int i = 0; i < N; i++) active[i] = (i % 3) == 0;

    ActivityGrid<N> grid;
    activity_grid_draw(grid, active, x, y, row_pitch, on, off);

    bool ok = grid_matches(N, active, x, y, row_pitch, on, off, bg);
    printf(ok ? "  OK: initial draw rendered every cell's on/off state\n"
              : "  FAIL: initial draw did not match the expected cell pattern\n");
    return ok;
}

bool test_wraps_at_16_per_row(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t on = gfx_rgb(60, 220, 90);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int x = 1, y = 182, row_pitch = 18;
    const int N = 32;  // more than one row's worth of cells

    bool active[N] = {};
    active[15] = true;   // last cell of row 0
    active[16] = true;   // first cell of row 1 -- must land on a new row, not overlap row 0

    ActivityGrid<N> grid;
    activity_grid_draw(grid, active, x, y, row_pitch, on, off);

    bool ok = grid_matches(N, active, x, y, row_pitch, on, off, bg);
    printf(ok ? "  OK: cell 16 wrapped onto a second row instead of overflowing row 0\n"
              : "  FAIL: the 16-cell-per-row wrap did not land where expected\n");
    return ok;
}

bool test_unchanged_cells_are_a_noop_changed_cell_redraws_only_itself(const char *) {
    const uint16_t on = gfx_rgb(60, 220, 90);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const int x = 1, y = 56, row_pitch = 18;
    const int N = 16;

    bool active[N] = {};
    ActivityGrid<N> grid;
    activity_grid_draw(grid, active, x, y, row_pitch, on, off);  // all off, establishes the latch

    lcd_stub_clear(0x1234);  // a color the Widget would never draw
    activity_grid_draw(grid, active, x, y, row_pitch, on, off);  // same pattern -- should be a no-op

    bool noop_ok = true;
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H && noop_ok; i++) {
        if (lcd_stub_fb[i] != 0x1234) noop_ok = false;
    }
    printf(noop_ok ? "  OK: an unchanged grid produced no LCD/gfx calls at all\n"
                   : "  FAIL: an unchanged grid still touched the framebuffer\n");

    active[5] = true;  // flip exactly one cell
    activity_grid_draw(grid, active, x, y, row_pitch, on, off);

    bool only_changed_ok = true;
    for (int py = 0; py < LCD_H && only_changed_ok; py++) {
        for (int px = 0; px < LCD_W; px++) {
            int cx = x + 5 * kActivityGridCellPitch;
            bool inside_cell5 = px >= cx && px < cx + kActivityGridCellW && py >= y &&
                                 py < y + kActivityGridCellH;
            uint16_t want = inside_cell5 ? on : (uint16_t)0x1234;
            if (lcd_stub_fb[py * LCD_W + px] != want) {
                only_changed_ok = false;
                break;
            }
        }
    }
    printf(only_changed_ok ? "  OK: a single changed cell redrew only itself\n"
                            : "  FAIL: a single changed cell touched more than its own region\n");

    return noop_ok && only_changed_ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== initial draw renders every cell's on/off state ==\n");
    ok = test_initial_draw("n/a") && ok;

    printf("\n== a grid wider than one row wraps at 16 cells per row ==\n");
    ok = test_wraps_at_16_per_row("n/a") && ok;

    printf("\n== an unchanged grid is a no-op; a changed cell redraws only itself ==\n");
    ok = test_unchanged_cells_are_a_noop_changed_cell_redraws_only_itself("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
