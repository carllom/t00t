// Host-buildable unit test for src/wslcd/gfx.cpp: compiles the real,
// unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory
// framebuffer stand-in for the ST7789 driver, so pixel-level rendering can
// be checked directly instead of on real hardware. Same convention as
// test_voice_alloc.cpp/test_input_layer.cpp (test_*() functions, an
// aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/gfx.h"
#include "lcd_st7789.h"

#include <cstdio>

// Guards against host_stub_lcd/lcd_st7789.h's LCD_W/LCD_H drifting from the
// real panel geometry: pull the real header's declarations into their own
// namespace (its own #pragma once is keyed on this distinct file path, so
// this doesn't collide with the stub's own inclusion above) purely to
// static_assert the constants match.
namespace real_lcd {
#include "../../src/wslcd/lcd_st7789.h"
}
static_assert(real_lcd::LCD_W == LCD_W && real_lcd::LCD_H == LCD_H,
              "host_stub_lcd/lcd_st7789.h's panel geometry no longer matches the real driver");

namespace {

bool test_fill_rect_bounds(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t color = gfx_rgb(255, 0, 0);
    gfx_fill_rect(10, 20, 30, 40, color);

    bool ok = true;
    for (int y = 0; y < LCD_H && ok; y++) {
        for (int x = 0; x < LCD_W; x++) {
            bool inside = (x >= 10 && x < 40 && y >= 20 && y < 60);
            uint16_t got = lcd_stub_fb[y * LCD_W + x];
            uint16_t want = inside ? color : (uint16_t)0x0000;
            if (got != want) {
                printf("  FAIL: pixel (%d,%d) = 0x%04x, want 0x%04x\n", x, y, got, want);
                ok = false;
                break;
            }
        }
    }
    if (ok) printf("  OK: gfx_fill_rect filled exactly [10,40)x[20,60) and nothing else\n");
    return ok;
}

bool test_text_glyph_pixels(const char *) {
    lcd_stub_clear(0x0000);
    const uint16_t fg = gfx_rgb(255, 255, 255);
    const uint16_t bg = gfx_rgb(0, 0, 0);
    const int scale = 2;
    const int x0 = 5, y0 = 7;

    gfx_text(x0, y0, "A", fg, bg, scale);

    const uint8_t *glyph = font8x8_basic['A' - FONT8X8_FIRST];
    bool ok = true;
    for (int gy = 0; gy < 8 && ok; gy++) {
        for (int gx = 0; gx < 8 && ok; gx++) {
            uint16_t want = (glyph[gy] & (1u << gx)) ? fg : bg;
            for (int sy = 0; sy < scale && ok; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x0 + gx * scale + sx;
                    int py = y0 + gy * scale + sy;
                    uint16_t got = lcd_stub_fb[py * LCD_W + px];
                    if (got != want) {
                        printf("  FAIL: pixel (%d,%d) = 0x%04x, want 0x%04x\n", px, py, got, want);
                        ok = false;
                        break;
                    }
                }
            }
        }
    }
    if (ok) printf("  OK: gfx_text rendered 'A' at scale %d with the expected glyph pixels\n", scale);
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== gfx_fill_rect fills exactly its region ==\n");
    ok = test_fill_rect_bounds("n/a") && ok;

    printf("\n== gfx_text renders the expected glyph pixels ==\n");
    ok = test_text_glyph_pixels("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
