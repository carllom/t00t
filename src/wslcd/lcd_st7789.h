// Low-level driver for the Waveshare 1.83" 240x284 IPS LCD (Rev2 = ST7789P)
// on the breadboard rig. SPI1 + a dedicated (polled) DMA channel, so pixel
// pushes never contend with the audio DMA IRQ on Core 1.
//
// Wiring (breadboard_rp2350):
//   DIN GP11 (SPI1 TX)   CLK GP10 (SPI1 SCK)   CS GP9   DC GP8
//   RST GP12             BL  GP13 (PWM)
//
// The panel is driven in "vertical" orientation: 240 wide x 284 tall. The Rev2
// module maps the visible window to GRAM origin (0,0) — no offset, unlike Rev1.

#pragma once

#include <cstdint>

// Visible panel geometry (portrait / VERTICAL scan).
static constexpr uint16_t LCD_W = 240;
static constexpr uint16_t LCD_H = 284;

// GRAM offset of the visible window. Rev2 (ST7789P) uses none; tunable if the
// image ends up shifted on real hardware.
#ifndef LCD_COL_OFFSET
#define LCD_COL_OFFSET 0
#endif
#ifndef LCD_ROW_OFFSET
#define LCD_ROW_OFFSET 0
#endif

// Bring the panel up: SPI1, GPIO, backlight PWM, DMA channel, reset + init.
void lcd_init();

// Backlight brightness, 0..100 percent (PWM on GP13).
void lcd_set_backlight(uint8_t percent);

// Set the drawing window (inclusive coords in visible-panel space) and leave
// the controller ready to receive pixels (RAMWR issued, DC=data).
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Stream `npix` RGB565 pixels from `buf` to the current window via DMA.
// RGB565 is byte-swapped on the way out (controller wants big-endian). Blocks
// until the transfer completes.
void lcd_blit(const uint16_t *buf, uint32_t npix);

// Non-blocking variants for a future low-priority Core-0 update loop.
void lcd_blit_start(const uint16_t *buf, uint32_t npix);
bool lcd_blit_busy();
void lcd_blit_wait();

// Fill the whole visible panel with one RGB565 colour.
void lcd_fill(uint16_t color);
