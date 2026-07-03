#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"

void display_init() {
    lcd_init();
    lcd_fill(gfx_rgb(0, 0, 0));
    lcd_set_backlight(100);
}

void display_bringup_test() {
    // Colour bars across the top third.
    static const uint16_t bars[] = {
        gfx_rgb(255, 0, 0), gfx_rgb(0, 255, 0), gfx_rgb(0, 0, 255),
        gfx_rgb(255, 255, 0), gfx_rgb(0, 255, 255), gfx_rgb(255, 0, 255),
    };
    const int nbars = sizeof(bars) / sizeof(bars[0]);
    int bw = LCD_W / nbars;
    for (int i = 0; i < nbars; i++) {
        gfx_fill_rect(i * bw, 0, bw, 80, bars[i]);
    }

    // Gradient band in the middle.
    // (gfx_gradient paints the whole panel; instead draw a framed area.)
    gfx_fill_rect(0, 80, LCD_W, 120, gfx_rgb(16, 16, 24));

    // Text banner. Strings are sized to fit the 240px width (10ch @3x = 240,
    // 15ch @2x = 240).
    uint16_t white = gfx_rgb(255, 255, 255);
    uint16_t bg    = gfx_rgb(16, 16, 24);
    uint16_t amber = gfx_rgb(255, 180, 0);
    gfx_text(0, 100, "hello t00t", amber, bg, 3);
    gfx_text(0, 140, "ST7789P 240x284", white, bg, 2);
    gfx_text(0, 165, "SPI1+DMA ok", white, bg, 2);

    // Bottom strip.
    gfx_fill_rect(0, 200, LCD_W, LCD_H - 200, gfx_rgb(0, 0, 0));
    gfx_text(0, 220, "core0 low-pri", gfx_rgb(120, 200, 120), gfx_rgb(0, 0, 0), 2);
}
