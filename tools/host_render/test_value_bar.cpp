// Host-buildable unit test for src/wslcd/value_bar.h (issue #129): the
// Value bar Widget's label+fill drawing and latch. Compiles the real,
// unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory
// framebuffer, same convention as test_value_row.cpp (test_*() functions,
// an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/value_bar.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstring>

namespace {

// True if every pixel in the bar [x, x+w) x [y, y+h) matches what
// value_bar_draw_value() should have drawn for `label` (blank-padded/
// truncated to the bar's own character budget) filled to `fill_frac`.
// Reimplements the same per-column/remainder-rect math value_bar.h uses --
// gfx_text_bar()'s own per-glyph-column correctness is covered directly by
// test_gfx.cpp, so this checks the Widget's integration of it (latch,
// padding, fill_x/remainder arithmetic), not the primitive itself.
bool bar_region_matches(int x, int y, int w, int h, const char *label, float fill_frac, int scale,
                         uint16_t fg, uint16_t fill, uint16_t off) {
    int gw = 8 * scale, gh = 8 * scale;
    int max_chars = w / gw;
    char buf[64];
    snprintf(buf, sizeof(buf), "%-*.*s", max_chars, max_chars, label);

    if (fill_frac < 0.0f) fill_frac = 0.0f;
    if (fill_frac > 1.0f) fill_frac = 1.0f;
    int fill_x = x + (int)(w * fill_frac + 0.5f);

    int text_end = x + max_chars * gw;
    for (int i = 0; i < max_chars; i++) {
        char c = buf[i];
        if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
        for (int gy = 0; gy < gh; gy++) {
            uint8_t bits = glyph[gy / scale];
            for (int gx = 0; gx < 8; gx++) {
                int col_x = x + i * gw + gx * scale;
                uint16_t bg = (col_x < fill_x) ? fill : off;
                uint16_t want = (bits & (1u << gx)) ? fg : bg;
                for (int sx = 0; sx < scale; sx++) {
                    int px = col_x + sx;
                    int py = y + gy;
                    if (lcd_stub_fb[py * LCD_W + px] != want) return false;
                }
            }
        }
    }

    if (text_end < x + w) {
        int local_fill = fill_x - text_end;
        if (local_fill < 0) local_fill = 0;
        if (local_fill > (x + w) - text_end) local_fill = (x + w) - text_end;
        for (int py = y; py < y + h; py++) {
            for (int px = text_end; px < x + w; px++) {
                uint16_t want = (px < text_end + local_fill) ? fill : off;
                if (lcd_stub_fb[py * LCD_W + px] != want) return false;
            }
        }
    }

    // Below the glyph line, when the bar is taller than one glyph (h > gh):
    // still fill/off, split at fill_x, all the way down to y + h.
    if (h > gh && text_end > x) {
        int local_fill = fill_x - x;
        if (local_fill < 0) local_fill = 0;
        if (local_fill > text_end - x) local_fill = text_end - x;
        for (int py = y + gh; py < y + h; py++) {
            for (int px = x; px < text_end; px++) {
                uint16_t want = (px < x + local_fill) ? fill : off;
                if (lcd_stub_fb[py * LCD_W + px] != want) return false;
            }
        }
    }
    return true;
}

bool test_fill_boundaries(const char *) {
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t fill = gfx_rgb(70, 130, 180);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const int x = 4, y = 40, w = 112, h = 16, scale = 2;

    // Several fractional values, including one (65%) that lands mid-glyph
    // for this label/width/scale -- the case a whole-glyph-quantized
    // overlay (prototype variant A) gets visibly wrong.
    const float fracs[] = {0.0f, 0.27f, 0.5f, 0.65f, 1.0f};
    bool ok = true;
    for (float frac : fracs) {
        lcd_stub_clear(0x0000);
        ValueBar<int> bar;
        int value = (int)(frac * 100.0f + 0.5f);
        value_bar_draw_value(bar, value, "MIX", frac, x, y, w, h, scale, fg, fill, off);

        bool region_ok = bar_region_matches(x, y, w, h, "MIX", frac, scale, fg, fill, off);
        printf(region_ok ? "  OK: fill_frac=%.2f rendered the expected fill/off split\n"
                          : "  FAIL: fill_frac=%.2f did not match the expected fill/off split\n",
               frac);
        ok = region_ok && ok;
    }
    return ok;
}

bool test_taller_than_glyph_fills_full_height(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t fill = gfx_rgb(70, 130, 180);
    const uint16_t off = gfx_rgb(28, 28, 34);
    // h (20) exceeds 8*scale (16): the strip below the glyph line must
    // still be fill/off colored all the way to y+h, not left untouched.
    const int x = 4, y = 40, w = 112, h = 20, scale = 2;

    ValueBar<int> bar;
    value_bar_draw_value(bar, 65, "MIX", 0.65f, x, y, w, h, scale, fg, fill, off);

    bool ok = bar_region_matches(x, y, w, h, "MIX", 0.65f, scale, fg, fill, off);
    printf(ok ? "  OK: a bar taller than one glyph line filled its full height\n"
              : "  FAIL: a bar taller than one glyph line left rows below the label unfilled\n");
    return ok;
}

bool test_label_truncates_to_char_budget(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t fill = gfx_rgb(70, 130, 180);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const int x = 4, y = 40, w = 112, h = 16, scale = 2;  // budget: 112/16 = 7 chars

    ValueBar<int> bar;
    value_bar_draw_value(bar, 40, "CUTOFF LONG", 0.4f, x, y, w, h, scale, fg, fill, off);

    bool ok = bar_region_matches(x, y, w, h, "CUTOFF LONG", 0.4f, scale, fg, fill, off);
    printf(ok ? "  OK: an over-long label truncated to the bar's own character budget\n"
              : "  FAIL: an over-long label did not truncate as expected\n");
    return ok;
}

bool test_unchanged_value_is_a_noop(const char *) {
    lcd_stub_clear(0x1234);  // a color the Widget would never draw
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t fill = gfx_rgb(70, 130, 180);
    const uint16_t off = gfx_rgb(28, 28, 34);
    const int x = 4, y = 40, w = 112, h = 16, scale = 2;

    ValueBar<int> bar;
    bar.value = 50;
    bar.initialized = true;

    value_bar_draw_value(bar, 50, "MIX", 0.5f, x, y, w, h, scale, fg, fill, off);

    bool ok = true;
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H && ok; i++) {
        if (lcd_stub_fb[i] != 0x1234) ok = false;
    }
    printf(ok ? "  OK: an unchanged value produced no LCD/gfx calls at all\n"
              : "  FAIL: an unchanged value still touched the framebuffer\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== fill boundary lands precisely at several fractional values, including mid-glyph ==\n");
    ok = test_fill_boundaries("n/a") && ok;

    printf("\n== a bar taller than one glyph line fills its full height ==\n");
    ok = test_taller_than_glyph_fills_full_height("n/a") && ok;

    printf("\n== an over-long label truncates to the bar's own character budget ==\n");
    ok = test_label_truncates_to_char_budget("n/a") && ok;

    printf("\n== an unchanged value is a no-op ==\n");
    ok = test_unchanged_value_is_a_noop("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
