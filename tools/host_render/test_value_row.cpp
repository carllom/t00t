// Host-buildable unit test for src/wslcd/value_row.h (issue #128): the
// Value row Widget's label + latched-value drawing. Compiles the real,
// unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory
// framebuffer, same convention as test_gfx.cpp (test_*() functions, an
// aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/value_row.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstring>

namespace {

// True if every pixel in [x, x + chars*8*scale) x [y, y + 8*scale) at row y
// matches the glyph pixels `text` (blank-padded to `chars`) would produce.
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

bool test_initial_draw_pixels(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t label_fg = gfx_rgb(110, 120, 140);
    const uint16_t value_fg = gfx_rgb(240, 240, 240);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int y = 40, x = 104, scale = 2;

    value_row_draw_label(0, y, "VOICES", 6, label_fg, bg, scale);
    ValueRow<int> row;
    value_row_draw_value(row, 3, "3/8", x, y, 8, scale, value_fg, bg);

    bool ok = region_matches(0, y, "VOICES", 6, scale, label_fg, bg) &&
              region_matches(x, y, "3/8", 8, scale, value_fg, bg);
    printf(ok ? "  OK: initial draw rendered both label and value pixels\n"
              : "  FAIL: initial draw pixels did not match\n");
    return ok;
}

bool test_shorter_value_blank_pads_over_longer(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int y = 40, x = 104, scale = 2, chars = 8;

    ValueRow<int> row;
    value_row_draw_value(row, 1, "12345678", x, y, chars, scale, fg, bg);
    value_row_draw_value(row, 2, "AB", x, y, chars, scale, fg, bg);

    bool ok = region_matches(x, y, "AB", chars, scale, fg, bg);
    printf(ok ? "  OK: a shorter value fully blank-padded over the longer previous one\n"
              : "  FAIL: stale trailing characters remained from the longer previous value\n");
    return ok;
}

bool test_unchanged_value_is_a_noop(const char *) {
    lcd_stub_clear(0x1234);  // a color gfx_text would never draw
    const uint16_t fg = gfx_rgb(240, 240, 240);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int y = 40, x = 104, scale = 2, chars = 8;

    ValueRow<int> row;
    row.value = 7;
    row.initialized = true;

    value_row_draw_value(row, 7, "SEVEN", x, y, chars, scale, fg, bg);

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

    printf("== initial draw renders label and value pixels ==\n");
    ok = test_initial_draw_pixels("n/a") && ok;

    printf("\n== a shorter new value blank-pads over a longer previous one ==\n");
    ok = test_shorter_value_blank_pads_over_longer("n/a") && ok;

    printf("\n== an unchanged value is a no-op ==\n");
    ok = test_unchanged_value_is_a_noop("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
