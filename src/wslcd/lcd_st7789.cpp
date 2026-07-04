#include "lcd_st7789.h"

#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

// --- Pins (from the board header) ---
#ifndef LCD_DC_PIN
#error "LCD_*_PIN must be defined by the board header (HAS_LCD board only)"
#endif

// SPI clock. 64 MHz runs cleanly even over the breadboard's dupont jumpers
// (the demo is conservative at 8 MHz, but the panel tolerates far more).
// spi_init returns the actual achievable rate, which is all we depend on.
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ (64000000u)
#endif

// MADCTL orientation byte (0x00 = VERTICAL, like the Rev2 demo). bit3 selects
// RGB(0)/BGR(1) sub-pixel order; flip to 0x08 if red/blue come out swapped.
#ifndef LCD_MADCTL
#define LCD_MADCTL 0x00
#endif

static int s_dma_chan = -1;
static uint s_bl_slice;

// --- init-table encoding -------------------------------------------------
// Byte stream: <cmd> <flags|len> <len data bytes...> [<delay_ms> if DELAY set].
// Terminated by a command byte of 0x00 (NOP, unused in our sequences).
#define LCD_DELAY 0x80

// ST7789P (Rev2) init, matching the verified Waveshare 1.83" demo register set.
static const uint8_t s_init[] = {
    0x36, 1, LCD_MADCTL,                 // MADCTL: orientation / colour order
    0x3A, 1, 0x05,                       // COLMOD: 16-bit RGB565
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,   // PORCTRL
    0xB7, 1, 0x35,                       // GCTRL
    0xBB, 1, 0x19,                       // VCOMS
    0xC0, 1, 0x2C,                       // LCMCTRL
    0xC2, 1, 0x01,                       // VDVVRHEN
    0xC3, 1, 0x12,                       // VRHS
    0xC4, 1, 0x20,                       // VDVS
    0xC6, 1, 0x0F,                       // FRCTRL2: ~60 Hz
    0xD0, 2, 0xA4, 0xA1,                 // PWCTRL1
    0xE0, 14, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
              0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23,  // PVGAMCTRL
    0xE1, 14, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
              0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23,  // NVGAMCTRL
    0x21, 0,                             // INVON (IPS panel)
    0x11, LCD_DELAY | 0, 200,            // SLPOUT
    0x29, LCD_DELAY | 0, 10,             // DISPON
    0x00,                                // end
};

// --- SPI byte-level helpers ---------------------------------------------
static inline void spi_drain() {
    while (spi_is_busy(spi1)) tight_loop_contents();
}

// CS is framed per transaction (CS low → transfer → CS high), matching the
// verified Waveshare demo. spi_write_blocking only returns once the last frame
// has shifted out (it reads back the RX bytes), so CS can be raised right after.
static void lcd_write_cmd(uint8_t cmd) {
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);          // command
    spi_write_blocking(spi1, &cmd, 1);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_write_data(const uint8_t *data, size_t len) {
    if (!len) return;
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);          // data
    spi_write_blocking(spi1, data, len);
    gpio_put(LCD_CS_PIN, 1);
}

static void lcd_reset() {
    gpio_put(LCD_CS_PIN, 1);          // idle high between transactions
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(10);
    gpio_put(LCD_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(120);
}

static void lcd_run_init_table(const uint8_t *t) {
    while (*t != 0x00) {
        uint8_t cmd = *t++;
        uint8_t flags = *t++;
        uint8_t len = flags & 0x7F;
        lcd_write_cmd(cmd);
        if (len) {
            lcd_write_data(t, len);
            t += len;
        }
        if (flags & LCD_DELAY) {
            sleep_ms(*t++);
        }
    }
}

// --- public API ----------------------------------------------------------
void lcd_set_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    pwm_set_chan_level(s_bl_slice, pwm_gpio_to_channel(LCD_BL_PIN), percent);
}

void lcd_init() {
    // SPI1 on CLK/MOSI.
    spi_init(spi1, LCD_SPI_HZ);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);

    // Control pins.
    gpio_init(LCD_DC_PIN);  gpio_set_dir(LCD_DC_PIN, GPIO_OUT);
    gpio_init(LCD_CS_PIN);  gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
    gpio_init(LCD_RST_PIN); gpio_set_dir(LCD_RST_PIN, GPIO_OUT);
    gpio_put(LCD_CS_PIN, 1);
    gpio_put(LCD_DC_PIN, 0);

    // Backlight via PWM (wrap 100 => level == percent), start full-on.
    gpio_set_function(LCD_BL_PIN, GPIO_FUNC_PWM);
    s_bl_slice = pwm_gpio_to_slice_num(LCD_BL_PIN);
    pwm_set_wrap(s_bl_slice, 100);
    pwm_set_clkdiv(s_bl_slice, 50.0f);
    pwm_set_enabled(s_bl_slice, true);
    lcd_set_backlight(100);

    // Dedicated DMA channel for pixel pushes (polled, no IRQ).
    s_dma_chan = dma_claim_unused_channel(true);

    lcd_reset();
    lcd_run_init_table(s_init);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += LCD_COL_OFFSET; x1 += LCD_COL_OFFSET;
    y0 += LCD_ROW_OFFSET; y1 += LCD_ROW_OFFSET;

    uint8_t caset[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t raset[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_write_cmd(0x2A);           // CASET
    lcd_write_data(caset, 4);
    lcd_write_cmd(0x2B);           // RASET
    lcd_write_data(raset, 4);
    lcd_write_cmd(0x2C);           // RAMWR
    gpio_put(LCD_DC_PIN, 1);       // stay in data mode for pixel stream
}

// Stream pixels via DMA. CS is framed for the whole blit (asserted here, released
// in lcd_blit_wait) — the SPI RX FIFO is left to overflow harmlessly since we
// only transmit. The DMA offloads Core 0 for the duration of the transfer.
void lcd_blit_start(const uint16_t *buf, uint32_t npix) {
    spi_drain();
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 1);

    dma_channel_config c = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_SPI1_TX);

    dma_channel_configure(
        s_dma_chan, &c,
        &spi_get_hw(spi1)->dr,       // SPI TX FIFO
        buf,                         // wire-format (byte-swapped) RGB565
        npix * 2,                    // bytes
        true);                       // start now
}

bool lcd_blit_busy() {
    return dma_channel_is_busy(s_dma_chan) || spi_is_busy(spi1);
}

void lcd_blit_wait() {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    spi_drain();
    gpio_put(LCD_CS_PIN, 1);          // release CS after the last bit shifts out
}

void lcd_blit(const uint16_t *buf, uint32_t npix) {
    lcd_blit_start(buf, npix);
    lcd_blit_wait();
}

void lcd_fill(uint16_t color) {
    // Stream a small repeated run so we don't need a full framebuffer.
    static uint16_t row[LCD_W];
    for (uint16_t i = 0; i < LCD_W; i++) row[i] = color;
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    for (uint16_t y = 0; y < LCD_H; y++) lcd_blit(row, LCD_W);
}
