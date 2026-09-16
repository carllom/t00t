#pragma once

#include "gfx.h"

#include <cstdint>
#include <cstdio>

// Label Widget: a single blank-padded string at a caller-chosen scale
// (1/2/3), for values whose meaning doesn't reduce to a proportional fill
// (contrast with Value bar/PercentageBar) and that don't pair a fixed label
// with a changing value (contrast with Value row) -- e.g. an effect type
// (off/delay/reverb) shown as plain text. Same typed latch /
// redraw-only-if-changed shape as every other Widget here.

inline constexpr int kLabelMaxChars = 32;

template <typename T>
struct Label {
    T value{};
    bool initialized = false;
};

// Draws `text` at (x, y), blank-padded to `chars` so a shorter new string
// fully overwrites a longer previous one -- but only when `value` differs
// from `lbl`'s latch (or `lbl` hasn't drawn yet). Updates the latch
// immediately, on this same call.
template <typename T>
void label_draw_value(Label<T> &lbl, const T &value, const char *text, int x, int y, int chars,
                       int scale, uint16_t fg, uint16_t bg) {
    if (lbl.initialized && lbl.value == value) return;

    int n = chars < 0 ? 0 : (chars < kLabelMaxChars ? chars : kLabelMaxChars);
    char buf[kLabelMaxChars + 1];
    snprintf(buf, sizeof(buf), "%-*.*s", n, n, text);
    gfx_text(x, y, buf, fg, bg, scale);

    lbl.value = value;
    lbl.initialized = true;
}
