// Host-buildable unit test for src/wslcd/voice_grid.h (issue #131): the
// VoiceGrid Widget's per-cell label+color drawing and latch. Compiles the
// real, unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory
// framebuffer, same convention as test_activity_grid.cpp/test_value_row.cpp
// (test_*() functions, an aggregated `bool ok`,
// "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/voice_grid.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstring>

namespace {

// True if every pixel in [x, x + chars*8*scale) x [y, y + 8*scale) matches
// the glyph pixels `text` (blank-padded to `chars`) would produce.
bool region_matches(int x, int y, const char *text, int chars, int scale, uint16_t fg,
                     uint16_t bg) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%-*.*s", chars, chars, text);

    for (int i = 0; i < chars; i++) {
        char c = buf[i];
        if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                uint16_t want = (glyph[gy] & (1u << gx)) ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + i * 8 * scale + gx * scale + sx;
                        int py = y + gy * scale + sy;
                        if (lcd_stub_fb[py * LCD_W + px] != want) return false;
                    }
                }
            }
        }
    }
    return true;
}

const uint16_t kColors[3] = {
    gfx_rgb(60, 220, 90),   // slot 0
    gfx_rgb(240, 180, 0),   // slot 1
    gfx_rgb(230, 60, 50),   // slot 2
};
const uint16_t kBg = gfx_rgb(0, 0, 0);

constexpr int kN = 8;
// char_width*8px*scale (48) fits inside col_pitch (60) with room to spare, so
// adjacent cells never spill into each other -- kept deliberately
// non-overlapping (unlike display.cpp's tighter, space-constrained grids) so
// each cell's region can be checked in isolation.
constexpr int kCols = 4, kColPitch = 60, kRowPitch = 12, kCharWidth = 6, kScale = 1;

bool test_initial_draw(const char *) {
    lcd_stub_clear(0x0000);
    const char *labels[kN] = { "0:1/07", "1:2/03", "", "", "", "", "", "" };
    uint8_t slots[kN] = { 0, 2, 1, 1, 1, 1, 1, 1 };

    VoiceGrid<kN> grid;
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);

    bool ok = true;
    for (int i = 0; i < kN; i++) {
        int cx = (i % kCols) * kColPitch;
        int cy = (i / kCols) * kRowPitch;
        if (!region_matches(cx, cy, labels[i], kCharWidth, kScale, kColors[slots[i]], kBg)) {
            printf("  FAIL: cell %d did not render its label/color\n", i);
            ok = false;
        }
    }
    printf(ok ? "  OK: initial draw rendered every cell's label and color\n"
              : "  FAIL: initial draw did not match the expected cells\n");
    return ok;
}

bool test_label_change_redraws(const char *) {
    lcd_stub_clear(0x0000);
    const char *labels[kN] = {};
    for (int i = 0; i < kN; i++) labels[i] = "";
    uint8_t slots[kN] = {};

    VoiceGrid<kN> grid;
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);

    labels[3] = "3:0/12";
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);

    int cx = (3 % kCols) * kColPitch, cy = (3 / kCols) * kRowPitch;
    bool ok = region_matches(cx, cy, "3:0/12", kCharWidth, kScale, kColors[0], kBg);
    printf(ok ? "  OK: a label-only change redrew the cell with the new label\n"
              : "  FAIL: a label-only change did not redraw as expected\n");
    return ok;
}

bool test_color_only_change_redraws(const char *) {
    lcd_stub_clear(0x0000);
    const char *labels[kN] = {};
    for (int i = 0; i < kN; i++) labels[i] = "5:1/00";
    uint8_t slots[kN] = {};

    VoiceGrid<kN> grid;
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);

    slots[5] = 2;  // same label, different caller-assigned color slot
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);

    int cx = (5 % kCols) * kColPitch, cy = (5 / kCols) * kRowPitch;
    bool ok = region_matches(cx, cy, "5:1/00", kCharWidth, kScale, kColors[2], kBg);
    printf(ok ? "  OK: a color-only change redrew the cell in the new color\n"
              : "  FAIL: a color-only change did not redraw as expected\n");
    return ok;
}

bool test_unchanged_grid_is_a_noop(const char *) {
    const char *labels[kN] = {};
    for (int i = 0; i < kN; i++) labels[i] = "4:2/01";
    uint8_t slots[kN] = {};
    for (int i = 0; i < kN; i++) slots[i] = 1;

    VoiceGrid<kN> grid;
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);  // establishes the latch

    lcd_stub_clear(0x1234);  // a color the Widget would never draw
    voice_grid_draw(grid, labels, slots, 0, 0, kCols, kColPitch, kRowPitch, kCharWidth, kScale,
                     kColors, kBg);  // identical labels/slots -- should be a no-op

    bool ok = true;
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H && ok; i++) {
        if (lcd_stub_fb[i] != 0x1234) ok = false;
    }
    printf(ok ? "  OK: an unchanged grid produced no LCD/gfx calls at all\n"
              : "  FAIL: an unchanged grid still touched the framebuffer\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== initial draw renders every cell's label and color ==\n");
    ok = test_initial_draw("n/a") && ok;

    printf("\n== a label-only change redraws the cell ==\n");
    ok = test_label_change_redraws("n/a") && ok;

    printf("\n== a color-only change redraws the cell ==\n");
    ok = test_color_only_change_redraws("n/a") && ok;

    printf("\n== an unchanged grid is a no-op ==\n");
    ok = test_unchanged_grid_is_a_noop("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
