#include "gfx.h"
#include "lcd_st7789.h"
#include "font8x8.h"

// Scratch tile: full panel width, tall enough for one scaled text line
// (8 * MAX_SCALE) or a fill band. 240 * 24 * 2 = ~11.5 KB.
static constexpr int TILE_H = 24;
static uint16_t s_tile[LCD_W * TILE_H];

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

    // Fill one tile band with the colour, blit it repeatedly down the rect.
    int band = TILE_H;
    for (int i = 0; i < pw * band; i++) s_tile[i] = color;

    lcd_set_window(x, y, x1 - 1, y1 - 1);
    for (int row = y; row < y1; row += band) {
        int rows = (row + band <= y1) ? band : (y1 - row);
        lcd_blit(s_tile, pw * rows);
    }
}

int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    scale = clampi(scale, 1, TILE_H / 8);
    int gh = 8 * scale;
    int gw = 8 * scale;

    // Count drawable chars until the right edge.
    int maxchars = (LCD_W - x) / gw;
    if (maxchars <= 0) return x;

    int n = 0;
    while (s[n] && n < maxchars) n++;
    int pw = n * gw;

    // Render the whole line into the tile band, then one blit.
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                uint16_t col = (bits & (1u << gx)) ? fg : bg;
                // expand by scale into the tile
                for (int sy = 0; sy < scale; sy++) {
                    uint16_t *dst = &s_tile[(gy * scale + sy) * pw + i * gw + gx * scale];
                    for (int sx = 0; sx < scale; sx++) dst[sx] = col;
                }
            }
        }
    }

    lcd_set_window(x, y, x + pw - 1, y + gh - 1);
    lcd_blit(s_tile, pw * gh);
    return x + pw;
}

void gfx_gradient() {
    for (int band = 0; band < LCD_H; band += TILE_H) {
        int rows = (band + TILE_H <= LCD_H) ? TILE_H : (LCD_H - band);
        for (int col = 0; col < LCD_W; col++) {
            uint8_t r = (uint8_t)(255 - col * 255 / (LCD_W - 1));
            uint8_t b = (uint8_t)(col * 255 / (LCD_W - 1));
            uint8_t g = (uint8_t)(band * 255 / (LCD_H - 1));
            uint16_t c = gfx_rgb(r, g, b);
            for (int row = 0; row < rows; row++) s_tile[row * LCD_W + col] = c;
        }
        lcd_set_window(0, band, LCD_W - 1, band + rows - 1);
        lcd_blit(s_tile, LCD_W * rows);
    }
}
