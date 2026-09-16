#pragma once

#include "gfx.h"

#include <cstdint>
#include <cstdio>

// Value row Widget (issue #128, part of #124's shared Widget/Header/Page
// library -- see CONTEXT.md's Value row entry): a fixed-width label plus a
// value, both drawn as plain text. x-offset, character width and text scale
// are all call parameters rather than hardcoded constants, so a wide-value
// row (e.g. chip's long instrument names) is the same Widget at a wider
// character budget, not a separate Widget type. Unifies today's per-module
// hand-rolled draw_val()/draw_label() pattern into one shared header.
//
// A caller must pick x/char_width/scale that keep the whole padded field on
// the panel: gfx_text() clips a too-wide line by abandoning the rest of the
// draw outright (see gfx.h), not per character, so a field that runs past
// LCD_W would render partially instead of shrinking.

// Largest value field this Widget will render. LCD_W (240) / 8px @ scale 1
// is 30 chars, the widest any real caller can fit; this leaves headroom.
inline constexpr int kValueRowMaxChars = 32;

// A ValueRow instance's own typed latch for the value it last drew --
// `initialized` marks the not-yet-drawn state explicitly, replacing the
// "impossible value" sentinels (e.g. 0xFE/0xFF "no value yet" markers)
// hand-rolled per module today. `text` passed to value_row_draw_value()
// should always be the same rendering of `value`, since a redraw is skipped
// whenever `value` matches the latch regardless of `text`.
template <typename T>
struct ValueRow {
    T value{};
    bool initialized = false;
};

namespace value_row_detail {

// Blank-pads `text` to `chars` (clamped to [0, kValueRowMaxChars]) and draws
// it at (x, y). Shared by the label and value draw calls below so the pad
// width's clamp lives in exactly one place.
inline void draw_padded(int x, int y, const char *text, int chars, uint16_t fg, uint16_t bg,
                         int scale) {
    char buf[kValueRowMaxChars + 1];
    int n = chars < 0 ? 0 : (chars < kValueRowMaxChars ? chars : kValueRowMaxChars);
    snprintf(buf, sizeof(buf), "%-*.*s", n, n, text);
    gfx_text(x, y, buf, fg, bg, scale);
}

}  // namespace value_row_detail

// Draws `label` at (x, y), blank-padded to `label_chars`. Unconditional (the
// label is fixed for the row's lifetime, so it carries no latch of its own)
// -- call once, e.g. from a Page's init.
inline void value_row_draw_label(int x, int y, const char *label, int label_chars, uint16_t fg,
                                  uint16_t bg, int scale) {
    value_row_detail::draw_padded(x, y, label, label_chars, fg, bg, scale);
}

// Draws `text` into the value field at (x, y), blank-padded to `char_width`
// so a shorter new value fully overwrites a longer previous one -- but only
// when `value` differs from `row`'s latch (or `row` hasn't drawn yet).
// Updates the latch immediately, on this same call: no bulk end-of-tick
// snapshot/compare.
template <typename T>
void value_row_draw_value(ValueRow<T> &row, const T &value, const char *text, int x, int y,
                           int char_width, int scale, uint16_t fg, uint16_t bg) {
    if (row.initialized && row.value == value) return;

    value_row_detail::draw_padded(x, y, text, char_width, fg, bg, scale);

    row.value = value;
    row.initialized = true;
}
