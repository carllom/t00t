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

// Full-screen horizontal R->B gradient (bring-up eye candy).
void gfx_gradient();
