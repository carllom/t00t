// Host-buildable unit test for src/wslcd/resource_bar.h (issue #135): the
// Resource bar Widget's proportional-rounded fill-px computation, its reuse
// of PercentageBar's severity color, its fixed 32px max width, its
// corner-inset safe-margin helper, and its own latch. Compiles the real,
// unmodified gfx.cpp against host_stub_lcd/lcd_st7789.h's in-memory
// framebuffer, same convention as test_percentage_bar.cpp (test_*()
// functions, an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/resource_bar.h"
#include "lcd_st7789.h"

#include <cstdio>

namespace {

// True if the bar drawn at [x, x+kResourceBarMaxPx) x [y, y+h) matches
// `fill_px` columns of `fill_color` followed by off_color, and every other
// panel pixel is `bg`.
bool bar_matches(int x, int y, int h, int fill_px, uint16_t fill_color, uint16_t off_color,
                  uint16_t bg) {
    for (int py = 0; py < LCD_H; py++) {
        for (int px = 0; px < LCD_W; px++) {
            uint16_t want = bg;
            if (px >= x && px < x + kResourceBarMaxPx && py >= y && py < y + h) {
                want = (px < x + fill_px) ? fill_color : off_color;
            }
            if (lcd_stub_fb[py * LCD_W + px] != want) {
                printf("  FAIL: pixel (%d,%d) = 0x%04x, want 0x%04x\n", px, py,
                       lcd_stub_fb[py * LCD_W + px], want);
                return false;
            }
        }
    }
    return true;
}

bool test_fill_px_rounds_correctly_at_each_real_max_voices() {
    struct Case {
        int max_voices, active_voices, want_px;
    };
    const Case cases[] = {
        // MAX_VOICES=32 (chip/tracker): 1px/voice, exact.
        {32, 0, 0}, {32, 1, 1}, {32, 16, 16}, {32, 31, 31}, {32, 32, 32},
        // MAX_VOICES=16 (subtractive/fm/groovebox): 2px/voice, exact.
        {16, 0, 0}, {16, 1, 2}, {16, 8, 16}, {16, 15, 30}, {16, 16, 32},
        // MAX_VOICES=9 (opl): doesn't divide 32 evenly -- proportional round.
        {9, 0, 0}, {9, 1, 4}, {9, 2, 7}, {9, 3, 11}, {9, 4, 14}, {9, 8, 28}, {9, 9, 32},
        // MAX_VOICES=8 (speech): 4px/voice, exact.
        {8, 0, 0}, {8, 1, 4}, {8, 2, 8}, {8, 7, 28}, {8, 8, 32},
    };

    bool ok = true;
    for (const auto &c : cases) {
        int got = resource_bar_fill_px(c.active_voices, c.max_voices);
        if (got != c.want_px) {
            printf("  FAIL: fill_px(active=%d, max=%d) = %d, want %d\n", c.active_voices,
                   c.max_voices, got, c.want_px);
            ok = false;
        }
    }
    printf(ok ? "  OK: fill-px rounding matches round(active*32/max) at every real MAX_VOICES\n"
              : "  FAIL: fill-px rounding diverged from expected at one or more cases\n");
    return ok;
}

bool test_fill_never_exceeds_32px_even_past_max_voices() {
    // A stale/out-of-range active_voices reading (> max_voices) must still
    // clamp to the bar's own fixed width, not overshoot it.
    bool ok = resource_bar_fill_px(37, 32) == kResourceBarMaxPx &&
              resource_bar_fill_px(21, 16) == kResourceBarMaxPx &&
              resource_bar_fill_px(14, 9) == kResourceBarMaxPx &&
              resource_bar_fill_px(13, 8) == kResourceBarMaxPx;
    printf(ok ? "  OK: fill_px clamps to kResourceBarMaxPx when active_voices > max_voices\n"
              : "  FAIL: fill_px exceeded kResourceBarMaxPx for an out-of-range active_voices\n");
    return ok;
}

const uint16_t kOff = gfx_rgb(28, 28, 34);
const uint16_t kBg = gfx_rgb(0, 0, 0);
constexpr int kX = 100, kY = 3, kH = 12;

bool test_pixels_match_at_each_real_max_voices() {
    struct Case {
        int max_voices, active_voices, fill_px;
    };
    const Case cases[] = {
        {32, 19, resource_bar_fill_px(19, 32)},
        {16, 6, resource_bar_fill_px(6, 16)},
        {9, 3, resource_bar_fill_px(3, 9)},
        {8, 5, resource_bar_fill_px(5, 8)},
    };

    bool ok = true;
    for (const auto &c : cases) {
        lcd_stub_clear(0x0000);
        ResourceBar rb;
        resource_bar_draw_value(rb, c.active_voices, c.max_voices, 42.0f, kX, kY, kH, kOff);

        bool region_ok =
            bar_matches(kX, kY, kH, c.fill_px, kPercentageBarColorLow, kOff, kBg);
        if (!region_ok) {
            printf("  FAIL: MAX_VOICES=%d pixel region did not match fill_px=%d\n",
                   c.max_voices, c.fill_px);
        }
        ok = ok && region_ok;
    }
    printf(ok ? "  OK: drawn pixels match the computed fill_px at every real MAX_VOICES\n"
              : "  FAIL: drawn pixels diverged from the computed fill_px at one or more cases\n");
    return ok;
}

bool test_fill_color_reuses_percentage_bar_severity_threshold() {
    struct Case {
        float cpu_percent;
        uint16_t want_color;
        const char *label;
    };
    const Case cases[] = {
        {49.0f, kPercentageBarColorLow, "below the low threshold fills green"},
        {50.0f, kPercentageBarColorMid, "at the low threshold fills amber"},
        {79.0f, kPercentageBarColorMid, "below the mid threshold fills amber"},
        {80.0f, kPercentageBarColorHigh, "at the mid threshold fills red"},
    };

    bool ok = true;
    for (const auto &c : cases) {
        lcd_stub_clear(0x0000);
        ResourceBar rb;
        resource_bar_draw_value(rb, 16, 32, c.cpu_percent, kX, kY, kH, kOff);

        bool region_ok = bar_matches(kX, kY, kH, kResourceBarMaxPx / 2, c.want_color, kOff, kBg);
        printf(region_ok ? "  OK: %s\n" : "  FAIL: %s\n", c.label);
        ok = ok && region_ok;
    }
    return ok;
}

bool test_corner_inset_safe_margin() {
    // Header row 1 (kHeaderRow1Y=3, kHeaderRow1H=14, header.h) documents a
    // ~17px safe margin on both sides -- verify gfx_corner_safe_margin(),
    // the shared primitive this Widget's placement relies on, reproduces
    // the same number.
    bool ok = gfx_corner_safe_margin(3, 17) == 17 &&
              gfx_corner_safe_margin(30, 44) == 0 &&  // fully clear of the corner mask
              gfx_corner_safe_margin(0, 30) == 30;    // the corner's own top edge
    printf(ok ? "  OK: safe margin matches the corner-inset formula over the given y-range\n"
              : "  FAIL: safe margin did not match the expected corner-inset value\n");
    return ok;
}

bool test_unchanged_value_is_a_noop_changed_value_redraws() {
    ResourceBar rb;
    resource_bar_draw_value(rb, 10, 32, 42.0f, kX, kY, kH, kOff);  // establishes the latch

    lcd_stub_clear(0x1234);  // a color this Widget would never draw
    resource_bar_draw_value(rb, 10, 32, 42.0f, kX, kY, kH, kOff);  // identical -- should be a no-op

    bool noop_ok = true;
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H && noop_ok; i++) {
        if (lcd_stub_fb[i] != 0x1234) noop_ok = false;
    }
    printf(noop_ok ? "  OK: an unchanged (voices, max_voices, cpu) produced no redraw\n"
                   : "  FAIL: an unchanged value still touched the framebuffer\n");

    resource_bar_draw_value(rb, 20, 32, 42.0f, kX, kY, kH, kOff);  // active_voices changed
    bool redraw_ok = bar_matches(kX, kY, kH, resource_bar_fill_px(20, 32), kPercentageBarColorLow,
                                  kOff, 0x1234);  // untouched panel still carries the prior fill
    printf(redraw_ok ? "  OK: a changed active_voices redraws with the new fill_px\n"
                      : "  FAIL: a changed active_voices did not redraw as expected\n");

    return noop_ok && redraw_ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== fill length: round(active_voices * 32 / MAX_VOICES) ==\n");
    ok = test_fill_px_rounds_correctly_at_each_real_max_voices() && ok;
    ok = test_fill_never_exceeds_32px_even_past_max_voices() && ok;
    ok = test_pixels_match_at_each_real_max_voices() && ok;

    printf("\n== fill color reuses PercentageBar's severity threshold ==\n");
    ok = test_fill_color_reuses_percentage_bar_severity_threshold() && ok;

    printf("\n== placement's safe margin comes from the corner-inset formula ==\n");
    ok = test_corner_inset_safe_margin() && ok;

    printf("\n== latch: unchanged is a no-op, a real change redraws ==\n");
    ok = test_unchanged_value_is_a_noop_changed_value_redraws() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
