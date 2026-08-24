#pragma once

#include "activity_grid.h"
#include "gfx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

// Header Widget (issue #134, part of #124's shared Widget/Header/Page
// library -- see CONTEXT.md's Page entry): the standard two-row chrome every
// module's Page sits below. Row 1 is the module name alone (centered, scale
// 1, accent-colored, no literal "t00t" wordmark -- `module_name` is always a
// caller-supplied string, never hardcoded here). Row 2 is the current Page's
// name (scale 2) plus a Page indicator built from ActivityGrid (#130)'s cell
// shape, one cell per Page, the current Page's cell the "on" color and every
// other cell "off". `N` (a Header<N> instance's own compile-time Page count,
// same shape as ActivityGrid<N>) fixes the indicator's cell count to a
// module's own declared Page list length; N == 1 (a module with only the
// required Performance page) means row 2 is never drawn at all -- omitted
// chrome, not a "1 of 1" indicator -- rather than a caller-side special case.
//
// Header is an ordinary Widget: each row owns a typed latch ({T value; bool
// initialized}), redrawing only when its own value actually changed. The
// module name's latch is a fixed-size char buffer compared with strncmp,
// since there's no owning string type available here without heap
// allocation. No bulk end-of-tick snapshot, no separate update path -- row
// 1 is drawn once in practice (a module's own name never changes at
// runtime) purely because its latch never sees a different value, not
// because of any special-cased "static chrome" code path.
//
// Row 1 sits at y=3, 14px tall -- `docs/lcd-driver-capabilities.md`'s
// corner-inset formula (`30 - sqrt(30^2 - (30-y)^2)`) puts the panel's
// ~30px corner-rounding safe margin at that y at ~17px on both sides, which
// is why row 1's centered text needs that much clearance. Row 2 starts
// immediately below, at y=17.
inline constexpr int kHeaderRow1Y = 3;
inline constexpr int kHeaderRow1H = 14;
inline constexpr int kHeaderRow2Y = kHeaderRow1Y + kHeaderRow1H;

// Largest module/Page name this Widget will latch/redraw. Comfortably
// covers the longest real module name ("SUBTRACTIVE", "GROOVEBOX") with
// headroom for names not yet chosen.
inline constexpr int kHeaderNameMaxChars = 16;

template <int N>
struct Header {
    static_assert(N >= 1, "a module always declares at least its required Performance page");
    static_assert(N <= kActivityGridCellsPerRow,
                  "the Page indicator's layout assumes a single row of cells; "
                  "ActivityGrid<N> itself wraps past kActivityGridCellsPerRow");

    char module_name[kHeaderNameMaxChars + 1] = {};
    bool module_name_initialized = false;

    uint8_t page_index = 0;
    char page_name[kHeaderNameMaxChars + 1] = {};
    bool page_row_initialized = false;

    // Only ever drawn into when N > 1 -- see header_draw_page_row() below.
    ActivityGrid<N> indicator;
};

// Draws row 1: `module_name` (truncated to kHeaderNameMaxChars), centered
// within [0, panel_w), scale 1, `fg` on `bg`. `panel_w` is a call parameter
// rather than a hardcoded LCD_W: the real driver header and its host-test
// stand-in both define a same-named LCD_W constant, and either one included
// directly from this file would collide with whichever one a caller's own
// translation unit already pulled in -- taking the panel width as a plain
// int sidesteps that entirely. The whole row-1 band is re-filled with `bg`
// before the text is drawn, so a shorter new name fully erases a longer
// previous one regardless of where centering places it. Only redraws when
// `module_name` differs from `hdr`'s latch (or `hdr` hasn't drawn yet);
// updates the latch immediately, on this same call.
template <int N>
void header_draw_module_name(Header<N> &hdr, const char *module_name, int panel_w, uint16_t fg,
                              uint16_t bg) {
    if (hdr.module_name_initialized &&
        strncmp(hdr.module_name, module_name, kHeaderNameMaxChars) == 0) {
        return;
    }

    int n = (int)strlen(module_name);
    if (n > kHeaderNameMaxChars) n = kHeaderNameMaxChars;

    gfx_fill_rect(0, kHeaderRow1Y, panel_w, kHeaderRow1H, bg);

    char buf[kHeaderNameMaxChars + 1];
    snprintf(buf, sizeof(buf), "%.*s", n, module_name);
    int text_w = n * 8;  // 8px glyph cell at scale 1
    int x = (panel_w - text_w) / 2;
    int y = kHeaderRow1Y + (kHeaderRow1H - 8) / 2;
    gfx_text(x, y, buf, fg, bg, 1);

    snprintf(hdr.module_name, sizeof(hdr.module_name), "%.*s", n, module_name);
    hdr.module_name_initialized = true;
}

// Draws row 2 for a multi-Page module (N > 1): `page_name` (blank-padded,
// left-aligned at x=0, scale 2, to whatever character budget fits before
// the indicator -- computed from the space actually available (indicator_x)
// rather than a budget fixed independent of N, so the label field shrinks
// itself clear of the indicator instead of colliding with it at a large N)
// plus a right-aligned, N-cell Page indicator with only `page_index`'s cell
// in `indicator_on`, every other cell `indicator_off`. The indicator's
// cells are kActivityGridCellH (14px) tall against row 2's own 16px band,
// and its `N <= kActivityGridCellsPerRow` bound (Header<N>'s own
// static_assert) keeps it to one row -- the x/y math below places every
// cell on a single row and has no wrap logic of its own. For a single-Page
// module (N == 1) this is a no-op -- row 2 stays fully omitted chrome,
// never drawn, rather than rendered blank or as "1 of 1". Otherwise
// redraws only when `page_index` or `page_name` differs from `hdr`'s latch
// (or `hdr` hasn't drawn yet) -- so a Page whose displayed name changes
// without its index changing still redraws; updates the latch immediately,
// on this same call.
//
// The whole row-2 band is re-filled with `bg` before anything else is
// drawn: the label field's own pixel width rarely lands exactly on
// indicator_x, and the indicator's cells leave gaps of their own (a 2px
// inter-cell gap, since kActivityGridCellPitch (15) exceeds kActivityGridCellW
// (13), plus the strip below their 14px height within row 2's 16px band) --
// a blanket fill is simpler and more robust than separately computing every
// one of those remainder rectangles by hand. Since that fill also wipes
// whatever the indicator last painted, `hdr.indicator`'s own per-cell latch
// is reset (`initialized = false`) immediately before activity_grid_draw()
// so every cell -- not just the ones whose active state actually changed --
// repaints on top of the fresh fill; this only runs at all when
// header_draw_page_row already decided a redraw is warranted (page_index/
// page_name actually changed), so a full N-cell repaint at that point costs
// nothing an unconditional row-2 fill wasn't already paying for.
template <int N>
void header_draw_page_row(Header<N> &hdr, uint8_t page_index, const char *page_name, int panel_w,
                           uint16_t fg, uint16_t bg, uint16_t indicator_on,
                           uint16_t indicator_off) {
    if (N <= 1) return;

    if (hdr.page_row_initialized && hdr.page_index == page_index &&
        strncmp(hdr.page_name, page_name, kHeaderNameMaxChars) == 0) {
        return;
    }

    int glyph_cell = 8 * 2;  // square glyph cell, scale 2 -- row 2's own band height
    int indicator_x = panel_w - N * kActivityGridCellPitch;

    gfx_fill_rect(0, kHeaderRow2Y, panel_w, glyph_cell, bg);

    int name_chars = indicator_x / glyph_cell;
    if (name_chars < 0) name_chars = 0;
    if (name_chars > kHeaderNameMaxChars) name_chars = kHeaderNameMaxChars;

    char buf[kHeaderNameMaxChars + 1];
    snprintf(buf, sizeof(buf), "%-*.*s", name_chars, name_chars, page_name);
    gfx_text(0, kHeaderRow2Y, buf, fg, bg, 2);

    bool active[N];
    for (int i = 0; i < N; i++) active[i] = (i == page_index);
    hdr.indicator.initialized = false;
    activity_grid_draw(hdr.indicator, active, indicator_x, kHeaderRow2Y, kActivityGridCellPitch,
                        indicator_on, indicator_off);

    hdr.page_index = page_index;
    snprintf(hdr.page_name, sizeof(hdr.page_name), "%.*s", kHeaderNameMaxChars, page_name);
    hdr.page_row_initialized = true;
}
