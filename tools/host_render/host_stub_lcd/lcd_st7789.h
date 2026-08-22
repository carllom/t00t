#pragma once

// Minimal stand-in for src/wslcd/lcd_st7789.h's implementation: an in-memory
// RGB565 framebuffer behind the same seven functions gfx.cpp calls, so gfx.cpp
// can be host-built and its pixel output inspected directly instead of driven
// over SPI to real hardware. lcd_st7789.h itself has no hardware dependency
// and is safe to include as-is; only lcd_st7789.cpp (SPI/DMA/PWM) isn't
// host-buildable, so this replaces that .cpp, not the header. Compiled from a
// copy of gfx.cpp placed in a directory with no real lcd_st7789.h alongside
// it (see CMakeLists.txt) so this header, listed first on the include path,
// is what gfx.cpp's own #include "lcd_st7789.h" resolves to.

#include <cstdint>

static constexpr uint16_t LCD_W = 240;
static constexpr uint16_t LCD_H = 284;

// The panel's GRAM, exposed for test inspection.
inline uint16_t lcd_stub_fb[LCD_W * LCD_H];

namespace lcd_stub {

inline uint16_t win_x0, win_y0, win_x1, win_y1;
inline uint32_t win_cursor;  // pixels already written into the current window

// Writes npix pixels starting at the window cursor, wrapping row-major
// within [win_x0,win_x1]x[win_y0,win_y1] the way the real controller's
// address counter does. Pixels past the window's bottom-right corner are
// dropped rather than spilling into the next window.
inline void write(const uint16_t *buf, uint16_t solid, bool from_buf, uint32_t npix) {
    uint32_t width = (uint32_t)(win_x1 - win_x0) + 1;
    uint32_t height = (uint32_t)(win_y1 - win_y0) + 1;
    for (uint32_t i = 0; i < npix; i++) {
        uint32_t pos = win_cursor + i;
        uint32_t row = pos / width;
        if (row >= height) break;
        uint32_t col = pos % width;
        lcd_stub_fb[(win_y0 + row) * LCD_W + (win_x0 + col)] = from_buf ? buf[i] : solid;
    }
    win_cursor += npix;
}

}  // namespace lcd_stub

// Resets the in-memory panel to a known color, for use between tests.
inline void lcd_stub_clear(uint16_t color = 0) {
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H; i++) lcd_stub_fb[i] = color;
}

inline void lcd_init() {}
inline void lcd_set_backlight(uint8_t) {}

inline void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_stub::win_x0 = x0;
    lcd_stub::win_y0 = y0;
    lcd_stub::win_x1 = x1;
    lcd_stub::win_y1 = y1;
    lcd_stub::win_cursor = 0;
}

inline void lcd_blit(const uint16_t *buf, uint32_t npix) {
    lcd_stub::write(buf, 0, true, npix);
}

inline void lcd_blit_start(const uint16_t *buf, uint32_t npix) {
    lcd_blit(buf, npix);
}

inline bool lcd_blit_busy() {
    return false;
}

inline void lcd_blit_wait() {}

inline void lcd_fill_window(uint16_t color, uint32_t npix) {
    lcd_stub::write(nullptr, color, false, npix);
}

inline void lcd_fill(uint16_t color) {
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    lcd_fill_window(color, (uint32_t)LCD_W * LCD_H);
}
