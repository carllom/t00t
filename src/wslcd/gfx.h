// Tiny tile-based graphics over the ST7789 driver. No full framebuffer: each
// primitive renders into a small scratch tile and DMA-blits region by region.
// All colours are "wire format" (byte-swapped RGB565) produced by gfx_rgb().
#pragma once

#include <cstdint>

// Build a wire-format (byte-swapped) RGB565 colour from 8-bit components.
static inline uint16_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}

// Filled rectangle (visible-panel coords, clipped to the panel).
void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);

// 8x8 text, integer-scaled (1..3). Renders one line; long lines are clipped
// at the right edge. Returns the x just past the drawn string.
int gfx_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);

// Like gfx_text(), but for a label overlaid on a proportional fill bar
// (issue #129): each glyph's background is chosen per pixel column rather
// than once for the whole glyph, so `fill_x` (a panel x-coordinate, not a
// glyph index) can land inside a single glyph's cell -- a column is drawn
// `fill` if its x is < fill_x, `off` otherwise. Ordinary sequential draws,
// same as gfx_text: no read-modify-write, no shadow framebuffer. Returns
// the x just past the drawn string, so a caller can fill the remainder of
// the bar's own width (if any) with a plain gfx_fill_rect.
int gfx_text_bar(int x, int y, const char *s, uint16_t fg, uint16_t fill, uint16_t off,
                  int fill_x, int scale);

// Full-screen horizontal R->B gradient (bring-up eye candy).
void gfx_gradient();
