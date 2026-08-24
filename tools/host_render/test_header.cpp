// Host-buildable unit test for src/wslcd/header.h (issue #134): the
// two-row Header Widget's row-1 centered module name, row-2 Page name +
// Page indicator, single-Page row-2 omission, and both rows' typed-latch
// redraw-only-if-changed behavior. Compiles the real, unmodified gfx.cpp
// against host_stub_lcd/lcd_st7789.h's in-memory framebuffer, same
// convention as test_value_row.cpp/test_activity_grid.cpp (test_*()
// functions, an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").

#include "../../src/wslcd/font8x8.h"
#include "../../src/wslcd/header.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstring>

namespace {

// True if pixel (px, py) is inside the glyph block header.h draws for
// `text`'s i-th character (n chars, `scale`) rooted at (x, y), and sets
// *out to the fg/bg pixel it should be. False (leaving *out untouched) for
// any pixel outside that block.
bool text_pixel(int px, int py, int x, int y, const char *text, int n, int scale, uint16_t fg,
                 uint16_t bg, uint16_t *out) {
    int gw = 8 * scale, gh = 8 * scale;
    if (px < x || py < y || py >= y + gh) return false;
    int i = (px - x) / gw;
    if (i >= n) return false;
    char c = text[i];
    if (c < FONT8X8_FIRST || c > FONT8X8_LAST) c = '?';
    const uint8_t *glyph = font8x8_basic[c - FONT8X8_FIRST];
    int gx = ((px - x) % gw) / scale;
    int gy = (py - y) / scale;
    *out = (glyph[gy] & (1u << gx)) ? fg : bg;
    return true;
}

// True if every pixel in row 1's own band ([0, LCD_W) x [kHeaderRow1Y,
// kHeaderRow1Y + kHeaderRow1H)) matches what header_draw_module_name(hdr,
// name, fg, bg) should have drawn: the whole band filled `bg`, with `name`
// (up to kHeaderNameMaxChars) centered on top in `fg`.
bool row1_band_matches(const char *name, uint16_t fg, uint16_t bg) {
    int n = (int)strlen(name);
    if (n > kHeaderNameMaxChars) n = kHeaderNameMaxChars;
    int text_w = n * 8;
    int x = (LCD_W - text_w) / 2;
    int y = kHeaderRow1Y + (kHeaderRow1H - 8) / 2;

    for (int py = kHeaderRow1Y; py < kHeaderRow1Y + kHeaderRow1H; py++) {
        for (int px = 0; px < LCD_W; px++) {
            uint16_t want;
            if (!text_pixel(px, py, x, y, name, n, 1, fg, bg, &want)) want = bg;
            if (lcd_stub_fb[py * LCD_W + px] != want) {
                printf("  FAIL: row1 pixel (%d,%d) = 0x%04x, want 0x%04x\n", px, py,
                       lcd_stub_fb[py * LCD_W + px], want);
                return false;
            }
        }
    }
    return true;
}

// True if every panel pixel outside row 1's own band equals `color` --
// i.e. header_draw_module_name() touched nothing beyond its own row.
bool outside_row1_is_solid(uint16_t color) {
    for (int py = 0; py < LCD_H; py++) {
        if (py >= kHeaderRow1Y && py < kHeaderRow1Y + kHeaderRow1H) continue;
        for (int px = 0; px < LCD_W; px++) {
            if (lcd_stub_fb[py * LCD_W + px] != color) return false;
        }
    }
    return true;
}

// True if every pixel in row 2's own band ([0, LCD_W) x [kHeaderRow2Y,
// kHeaderRow2Y + 16)) matches what header_draw_page_row(hdr, page_index,
// page_name, LCD_W, fg, bg, on, off) should have drawn: the Page name
// blank-padded to whatever character budget fits before the indicator
// (mirroring header.h's own indicator_x/gw computation) at scale 2 (x=0),
// a right-aligned N-cell indicator with only `page_index`'s cell `on`,
// everywhere else `outside`.
template <int N>
bool row2_matches(uint8_t page_index, const char *page_name, uint16_t fg, uint16_t bg,
                   uint16_t on, uint16_t off, uint16_t outside) {
    int indicator_x = LCD_W - N * kActivityGridCellPitch;
    int name_chars = indicator_x / (8 * 2);
    if (name_chars < 0) name_chars = 0;
    if (name_chars > kHeaderNameMaxChars) name_chars = kHeaderNameMaxChars;

    char padded[kHeaderNameMaxChars + 1];
    snprintf(padded, sizeof(padded), "%-*.*s", name_chars, name_chars, page_name);

    for (int py = kHeaderRow2Y; py < kHeaderRow2Y + 16; py++) {
        for (int px = 0; px < LCD_W; px++) {
            uint16_t want;
            if (text_pixel(px, py, 0, kHeaderRow2Y, padded, name_chars, 2, fg, bg, &want)) {
                // matched label text/gap pixel
            } else {
                want = outside;
                for (int i = 0; i < N; i++) {
                    int cx = indicator_x + i * kActivityGridCellPitch;
                    int cy = kHeaderRow2Y;
                    if (px >= cx && px < cx + kActivityGridCellW && py >= cy &&
                        py < cy + kActivityGridCellH) {
                        want = (i == page_index) ? on : off;
                    }
                }
            }
            if (lcd_stub_fb[py * LCD_W + px] != want) {
                printf("  FAIL: row2 pixel (%d,%d) = 0x%04x, want 0x%04x\n", px, py,
                       lcd_stub_fb[py * LCD_W + px], want);
                return false;
            }
        }
    }
    return true;
}

bool panel_is_solid(uint16_t color) {
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H; i++) {
        if (lcd_stub_fb[i] != color) return false;
    }
    return true;
}

const uint16_t kFg1 = gfx_rgb(255, 255, 255);
const uint16_t kBg1 = gfx_rgb(160, 60, 30);
const uint16_t kFg2 = gfx_rgb(240, 240, 240);
const uint16_t kBg2 = gfx_rgb(0, 0, 0);
const uint16_t kOn = gfx_rgb(60, 220, 90);
const uint16_t kOff = gfx_rgb(28, 28, 34);

bool test_row1_centers_module_name() {
    lcd_stub_clear(kBg1);
    Header<2> hdr;
    header_draw_module_name(hdr, "FM", LCD_W, kFg1, kBg1);

    bool ok = row1_band_matches("FM", kFg1, kBg1) && outside_row1_is_solid(kBg1);
    printf(ok ? "  OK: row 1 renders the module name centered, accent-colored, on its own band\n"
              : "  FAIL: row 1 did not render as expected\n");
    return ok;
}

bool test_two_page_module_layout() {
    lcd_stub_clear(kBg2);
    Header<2> hdr;
    header_draw_module_name(hdr, "FM", LCD_W, kFg1, kBg1);
    header_draw_page_row(hdr, /*page_index=*/0, "MAIN", LCD_W, kFg2, kBg2, kOn, kOff);

    // Row 1 and row 2 occupy disjoint y ranges, so checking each in
    // isolation covers the whole panel between them.
    bool ok = row1_band_matches("FM", kFg1, kBg1) && row2_matches<2>(0, "MAIN", kFg2, kBg2, kOn, kOff, kBg2);
    printf(ok ? "  OK: a two-Page module renders row 1 (centered name) and row 2 (Page name + 2-cell indicator)\n"
              : "  FAIL: two-Page module layout did not match\n");
    return ok;
}

bool test_single_page_module_omits_row2() {
    lcd_stub_clear(kBg2);
    Header<1> hdr;
    header_draw_page_row(hdr, /*page_index=*/0, "PERFORMANCE", LCD_W, kFg2, kBg2, kOn, kOff);

    bool ok = panel_is_solid(kBg2) && !hdr.page_row_initialized;
    printf(ok ? "  OK: a single-Page module's row 2 is fully omitted, never drawn\n"
              : "  FAIL: a single-Page module's row 2 touched the framebuffer\n");
    return ok;
}

bool test_indicator_cell_count_matches_declared_pages() {
    lcd_stub_clear(kBg2);
    Header<5> hdr;
    header_draw_page_row(hdr, /*page_index=*/2, "FX", LCD_W, kFg2, kBg2, kOn, kOff);

    bool ok = row2_matches<5>(2, "FX", kFg2, kBg2, kOn, kOff, kBg2);
    printf(ok ? "  OK: a 5-Page module's indicator draws exactly 5 cells, cell 2 (current) distinct from the rest\n"
              : "  FAIL: indicator cell count or current-cell distinction did not match the declared Page list\n");
    return ok;
}

bool test_unchanged_module_name_is_a_noop() {
    Header<2> hdr;
    header_draw_module_name(hdr, "OPL", LCD_W, kFg1, kBg1);

    lcd_stub_clear(0x1234);  // a color this Widget would never draw
    header_draw_module_name(hdr, "OPL", LCD_W, kFg1, kBg1);

    bool ok = panel_is_solid(0x1234);
    printf(ok ? "  OK: an unchanged module name produces no redraw\n"
              : "  FAIL: an unchanged module name still touched the framebuffer\n");
    return ok;
}

bool test_unchanged_page_index_is_a_noop() {
    Header<3> hdr;
    header_draw_page_row(hdr, /*page_index=*/1, "MIX", LCD_W, kFg2, kBg2, kOn, kOff);

    lcd_stub_clear(0x1234);  // a color this Widget would never draw
    header_draw_page_row(hdr, /*page_index=*/1, "MIX", LCD_W, kFg2, kBg2, kOn, kOff);

    bool ok = panel_is_solid(0x1234);
    printf(ok ? "  OK: an unchanged Page index produces no redraw\n"
              : "  FAIL: an unchanged Page index still touched the framebuffer\n");
    return ok;
}

bool test_page_name_change_at_same_index_redraws() {
    lcd_stub_clear(kBg2);
    Header<3> hdr;
    header_draw_page_row(hdr, /*page_index=*/1, "OLD", LCD_W, kFg2, kBg2, kOn, kOff);
    header_draw_page_row(hdr, /*page_index=*/1, "NEW", LCD_W, kFg2, kBg2, kOn, kOff);

    bool ok = row2_matches<3>(1, "NEW", kFg2, kBg2, kOn, kOff, kBg2);
    printf(ok ? "  OK: a Page-name change at the same Page index still redraws row 2\n"
              : "  FAIL: a Page-name change at the same Page index was incorrectly skipped\n");
    return ok;
}

bool test_row2_fully_repaints_over_a_stale_framebuffer() {
    // Row 2 has several slivers no single fill/draw call would touch on its
    // own: the rounding remainder between the label field's actual pixel
    // width and indicator_x, the 2px inter-cell gaps ActivityGrid itself
    // never paints (kActivityGridCellPitch (15) > kActivityGridCellW (13)),
    // and the strip below the indicator's 14px-tall cells within row 2's
    // 16px band. Starting from a sentinel-filled (not pre-bg-cleared)
    // framebuffer and checking the whole band via row2_matches proves
    // header_draw_page_row() actually erases all of them, not just
    // coincidentally matches an already-correct background.
    lcd_stub_clear(0x1234);
    Header<3> hdr;
    header_draw_page_row(hdr, /*page_index=*/1, "FX", LCD_W, kFg2, kBg2, kOn, kOff);

    bool ok = row2_matches<3>(1, "FX", kFg2, kBg2, kOn, kOff, kBg2);
    printf(ok ? "  OK: row 2 fully repaints its own band, leaving no stale pixels from before\n"
              : "  FAIL: some part of row 2's band kept a stale pixel from before the redraw\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== row 1 centers the module name on its own band ==\n");
    ok = test_row1_centers_module_name() && ok;

    printf("\n== a two-Page module renders both rows correctly ==\n");
    ok = test_two_page_module_layout() && ok;

    printf("\n== a single-Page module omits row 2 entirely ==\n");
    ok = test_single_page_module_omits_row2() && ok;

    printf("\n== the Page indicator's cell count matches the declared Page list ==\n");
    ok = test_indicator_cell_count_matches_declared_pages() && ok;

    printf("\n== both rows redraw only when their own latched value changes ==\n");
    ok = test_unchanged_module_name_is_a_noop() && ok;
    ok = test_unchanged_page_index_is_a_noop() && ok;

    printf("\n== a Page-name change at the same index still redraws ==\n");
    ok = test_page_name_change_at_same_index_redraws() && ok;

    printf("\n== row 2 fully repaints over a stale framebuffer ==\n");
    ok = test_row2_fully_repaints_over_a_stale_framebuffer() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
