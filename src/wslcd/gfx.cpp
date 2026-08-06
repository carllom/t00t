#include "gfx.h"
#include "lcd_st7789.h"
#include "font8x8.h"

// Largest integer text scale the title bar needs (decided in #5).
static constexpr int MAX_SCALE = 3;

// Scratch cell: one glyph at max scale. 24 * 24 * 2 = 1,152 B. gfx_gradient()
// reuses the same storage for its bring-up bands (diagnostic-only).
static uint16_t s_glyph[8 * MAX_SCALE * 8 * MAX_SCALE];

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color) {
    int x1 = clampi(x + w, 0, LCD_W);
    int y1 = clampi(y + h, 0, LCD_H);
    x = clampi(x, 0, LCD_W);
    y = clampi(y, 0, LCD_H);
    if (x1 <= x || y1 <= y) return;
    int pw = x1 - x;

    lcd_set_window(x, y, x1 - 1, y1 - 1);
    lcd_fill_window(color, (uint32_t)pw * (y1 - y));
}

int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    scale = clampi(scale, 1, MAX_SCALE);
    int gh = 8 * scale;
    int gw = 8 * scale;

    // Render one glyph at a time into s_glyph, then blit it immediately.
    while (*s) {
        if (x + gw > LCD_W) break;   // clip at the right edge

        char c = *s++;
        if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                uint16_t col = (bits & (1u << gx)) ? fg : bg;
                // expand by scale into the cell
                for (int sy = 0; sy < scale; sy++) {
                    uint16_t *dst = &s_glyph[(gy * scale + sy) * gw + gx * scale];
                    for (int sx = 0; sx < scale; sx++) dst[sx] = col;
                }
            }
        }

        lcd_set_window(x, y, x + gw - 1, y + gh - 1);
        lcd_blit(s_glyph, gw * gh);
        x += gw;
    }
    return x;
}

void gfx_gradient() {
    // Diagnostic-only bring-up eye candy: reuse s_glyph's backing storage as
    // a full-width band buffer, sized to whatever height it affords.
    constexpr int GRAD_BAND = (int)(sizeof(s_glyph) / sizeof(s_glyph[0])) / LCD_W;
    static_assert(GRAD_BAND > 0, "s_glyph too small for a gradient band");

    for (int band = 0; band < LCD_H; band += GRAD_BAND) {
        int rows = (band + GRAD_BAND <= LCD_H) ? GRAD_BAND : (LCD_H - band);
        for (int col = 0; col < LCD_W; col++) {
            uint8_t r = (uint8_t)(255 - col * 255 / (LCD_W - 1));
            uint8_t b = (uint8_t)(col * 255 / (LCD_W - 1));
            uint8_t g = (uint8_t)(band * 255 / (LCD_H - 1));
            uint16_t c = gfx_rgb(r, g, b);
            for (int row = 0; row < rows; row++) s_glyph[row * LCD_W + col] = c;
        }
        lcd_set_window(0, band, LCD_W - 1, band + rows - 1);
        lcd_blit(s_glyph, LCD_W * rows);
    }
}
