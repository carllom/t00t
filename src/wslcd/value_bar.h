#pragma once

#include "gfx.h"

#include <cstdint>
#include <cstdio>

// Value bar Widget (issue #129, part of #124's shared Widget/Header/Page
// library -- see CONTEXT.md's Value bar entry): a label overlaid on a
// proportional fill bar, for CC-style continuous values (pan, FX mix,
// filter cutoff) -- more screen-estate-compact than Value row (#128) for
// this case, since the fill level itself carries most of the value's
// meaning. Built on gfx_text_bar() (gfx.h), which lets the fill/off
// boundary land inside a single glyph's cell instead of only on a glyph
// edge -- see prototype variant B on branch `prototype/value-bar`, the
// winning variant this Widget implements.
//
// Same x/char-budget/scale-are-call-parameters shape as Value row, same
// typed latch ({T value; bool initialized;}), same redraw-only-if-changed
// behavior. A caller must pick x/w/scale that keep the whole bar on the
// panel -- gfx_text_bar() clips a too-wide line at the right edge outright
// (see gfx.h), not by shrinking it.

// Largest label this Widget will render. Matches value_row.h's
// kValueRowMaxChars headroom rationale.
inline constexpr int kValueBarMaxChars = 32;

// A ValueBar instance's own typed latch for the value it last drew --
// mirrors ValueRow<T> (value_row.h). `label` and `fill_frac` passed to
// value_bar_draw_value() should always be the same rendering of `value`,
// since a redraw is skipped whenever `value` matches the latch regardless
// of what `label`/`fill_frac` are passed.
template <typename T>
struct ValueBar {
    T value{};
    bool initialized = false;
};

// Draws `label` overlaid on a bar spanning [x, x+w) x [y, y+h), filled to
// `fill_frac` (clamped to [0,1]) -- but only when `value` differs from
// `bar`'s latch (or `bar` hasn't drawn yet). `label` is truncated (or
// blank-padded) to the bar's own character budget (w / (8*scale)), the same
// truncate/blank-pad idiom value_row.h's draw_padded() uses, so both
// Widgets behave predictably when a label doesn't fit. Updates the latch
// immediately, on this same call.
template <typename T>
void value_bar_draw_value(ValueBar<T> &bar, const T &value, const char *label, float fill_frac,
                           int x, int y, int w, int h, int scale, uint16_t fg, uint16_t fill_color,
                           uint16_t off_color) {
    if (bar.initialized && bar.value == value) return;

    int gw = 8 * scale;
    int max_chars = gw > 0 ? w / gw : 0;
    int n = max_chars < 0 ? 0 : (max_chars < kValueBarMaxChars ? max_chars : kValueBarMaxChars);
    char buf[kValueBarMaxChars + 1];
    snprintf(buf, sizeof(buf), "%-*.*s", n, n, label);

    if (fill_frac < 0.0f) fill_frac = 0.0f;
    if (fill_frac > 1.0f) fill_frac = 1.0f;
    int fill_x = x + (int)(w * fill_frac + 0.5f);

    // gfx_text_bar() only paints the one glyph-tall strip [y, y+gh); finish
    // the fill/off split with plain two-rect fills for whatever's left of
    // the bar's own [x, x+w) x [y, y+h) footprint -- the width remainder
    // past the text (the bar's width isn't always a whole number of glyph
    // cells) and, when h is taller than a glyph line, the height remainder
    // below it, both split at fill_x same as the text itself was.
    int gh = 8 * scale;
    int end_x = gfx_text_bar(x, y, buf, fg, fill_color, off_color, fill_x, scale);

    auto split_fill = [&](int rx, int ry, int rw, int rh) {
        int local_fill = fill_x - rx;
        if (local_fill < 0) local_fill = 0;
        if (local_fill > rw) local_fill = rw;
        gfx_fill_rect(rx, ry, local_fill, rh, fill_color);
        gfx_fill_rect(rx + local_fill, ry, rw - local_fill, rh, off_color);
    };

    if (end_x < x + w) split_fill(end_x, y, (x + w) - end_x, h);
    if (h > gh && end_x > x) split_fill(x, y + gh, end_x - x, h - gh);

    bar.value = value;
    bar.initialized = true;
}
