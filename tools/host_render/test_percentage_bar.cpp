// Host-buildable unit test for src/wslcd/percentage_bar.h (issue #133): the
// PercentageBar Widget's auto-selected severity color and its own hysteresis
// band on top of Value bar's shared latch. Compiles the real, unmodified
// gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory framebuffer, same
// convention as test_value_bar.cpp (test_*() functions, an aggregated
// `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/percentage_bar.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstring>

namespace {

// True if every pixel in the bar [x, x+w) x [y, y+h) matches what
// value_bar_draw_value() should have drawn for `label` filled to
// `percent`/100 with `fill_color` -- reimplements the same per-column/
// remainder-rect math test_value_bar.cpp's own bar_region_matches() does;
// PercentageBar adds no new pixel-level drawing of its own to verify.
bool bar_region_matches(int x, int y, int w, int h, const char *label, float fill_frac, int scale,
                         uint16_t fg, uint16_t fill, uint16_t off) {
    int gw = 8 * scale, gh = 8 * scale;
    int max_chars = w / gw;
    char buf[64];
    snprintf(buf, sizeof(buf), "%-*.*s", max_chars, max_chars, label);

    if (fill_frac < 0.0f) fill_frac = 0.0f;
    if (fill_frac > 1.0f) fill_frac = 1.0f;
    int fill_x = x + (int)(w * fill_frac + 0.5f);

    int text_end = x + max_chars * gw;
    for (int i = 0; i < max_chars; i++) {
        char c = buf[i];
        if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
        for (int gy = 0; gy < gh; gy++) {
            uint8_t bits = glyph[gy / scale];
            for (int gx = 0; gx < 8; gx++) {
                int col_x = x + i * gw + gx * scale;
                uint16_t bg = (col_x < fill_x) ? fill : off;
                uint16_t want = (bits & (1u << gx)) ? fg : bg;
                for (int sx = 0; sx < scale; sx++) {
                    int px = col_x + sx;
                    int py = y + gy;
                    if (lcd_stub_fb[py * LCD_W + px] != want) return false;
                }
            }
        }
    }

    if (text_end < x + w) {
        int local_fill = fill_x - text_end;
        if (local_fill < 0) local_fill = 0;
        if (local_fill > (x + w) - text_end) local_fill = (x + w) - text_end;
        for (int py = y; py < y + h; py++) {
            for (int px = text_end; px < x + w; px++) {
                uint16_t want = (px < text_end + local_fill) ? fill : off;
                if (lcd_stub_fb[py * LCD_W + px] != want) return false;
            }
        }
    }

    if (h > gh && text_end > x) {
        int local_fill = fill_x - x;
        if (local_fill < 0) local_fill = 0;
        if (local_fill > text_end - x) local_fill = text_end - x;
        for (int py = y + gh; py < y + h; py++) {
            for (int px = x; px < text_end; px++) {
                uint16_t want = (px < x + local_fill) ? fill : off;
                if (lcd_stub_fb[py * LCD_W + px] != want) return false;
            }
        }
    }
    return true;
}

const uint16_t kFg = gfx_rgb(240, 240, 240);
const uint16_t kOff = gfx_rgb(28, 28, 34);
constexpr int kX = 4, kY = 40, kW = 112, kH = 16, kScale = 2;

bool test_color_below_low_threshold() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 49.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.49f, kScale, kFg,
                                  kPercentageBarColorLow, kOff);
    printf(ok ? "  OK: 49%% (below the low threshold) fills green\n"
              : "  FAIL: 49%% did not render with the green severity color\n");
    return ok;
}

bool test_color_at_low_threshold() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 50.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.50f, kScale, kFg,
                                  kPercentageBarColorMid, kOff);
    printf(ok ? "  OK: 50%% (at the low threshold) fills amber\n"
              : "  FAIL: 50%% did not render with the amber severity color\n");
    return ok;
}

bool test_color_below_mid_threshold() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 79.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.79f, kScale, kFg,
                                  kPercentageBarColorMid, kOff);
    printf(ok ? "  OK: 79%% (below the mid threshold) fills amber\n"
              : "  FAIL: 79%% did not render with the amber severity color\n");
    return ok;
}

bool test_color_at_mid_threshold() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 80.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.80f, kScale, kFg,
                                  kPercentageBarColorHigh, kOff);
    printf(ok ? "  OK: 80%% (at the mid threshold) fills red\n"
              : "  FAIL: 80%% did not render with the red severity color\n");
    return ok;
}

bool test_color_well_above_mid_threshold() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 99.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.99f, kScale, kFg,
                                  kPercentageBarColorHigh, kOff);
    printf(ok ? "  OK: 99%% fills red\n"
              : "  FAIL: 99%% did not render with the red severity color\n");
    return ok;
}

bool test_jitter_within_hysteresis_band_is_a_noop() {
    PercentageBar pb;
    percentage_bar_draw_value(pb, 42.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);

    lcd_stub_clear(0x1234);  // a color this Widget would never draw
    percentage_bar_draw_value(pb, 43.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);  // +1, within the 2%-band

    bool ok = true;
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H && ok; i++) {
        if (lcd_stub_fb[i] != 0x1234) ok = false;
    }
    ok = ok && pb.bar.value == 42.0f;  // the latch itself must also stay at the last drawn value
    printf(ok ? "  OK: a jitter-sized change (+1%%, inside the hysteresis band) produced no redraw\n"
              : "  FAIL: a jitter-sized change redrew, or moved the latch, when it should not have\n");
    return ok;
}

bool test_change_past_hysteresis_band_redraws() {
    lcd_stub_clear(0x0000);
    PercentageBar pb;
    percentage_bar_draw_value(pb, 42.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);
    percentage_bar_draw_value(pb, 46.0f, "CPU", kX, kY, kW, kH, kScale, kFg, kOff);  // +4, past the band

    bool ok = bar_region_matches(kX, kY, kW, kH, "CPU", 0.46f, kScale, kFg,
                                  kPercentageBarColorLow, kOff) &&
              pb.bar.value == 46.0f;
    printf(ok ? "  OK: a change past the hysteresis band redraws and moves the latch\n"
              : "  FAIL: a change past the hysteresis band did not redraw as expected\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== fill color auto-selects by the fixed severity threshold ==\n");
    ok = test_color_below_low_threshold() && ok;
    ok = test_color_at_low_threshold() && ok;
    ok = test_color_below_mid_threshold() && ok;
    ok = test_color_at_mid_threshold() && ok;
    ok = test_color_well_above_mid_threshold() && ok;

    printf("\n== hysteresis suppresses a jitter-sized redraw ==\n");
    ok = test_jitter_within_hysteresis_band_is_a_noop() && ok;
    ok = test_change_past_hysteresis_band_redraws() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
