#pragma once

#include "gfx.h"
#include "value_bar.h"

// PercentageBar Widget (issue #133, part of #124's shared Widget/Header/Page
// library -- see CONTEXT.md's PercentageBar entry): a Value bar (#129)
// specialized to 0-100% values (e.g. CPU load), whose fill color is chosen
// automatically from a fixed 3-tier severity threshold instead of being
// caller-supplied, so every load-style indicator across modules reads the
// same severity colors. Built directly on ValueBar<float>'s overlay
// primitive and latch, not a reimplementation -- the only things this
// Widget adds are the color selection and its own jitter-suppressing
// hysteresis band.
//
// percentage_bar_severity_color() is exposed on its own (not folded
// silently into percentage_bar_draw_value()) because Resource bar's fill
// color reuses this exact threshold, per CONTEXT.md's Resource bar entry.

inline constexpr float kPercentageBarThresholdLow = 50.0f;   // below: green
inline constexpr float kPercentageBarThresholdMid = 80.0f;   // below: amber; at/above: red

// Matches the COL_LOAD_LO/MID/HI triples already hand-rolled in
// src/engines/{chip,groovebox}/display.cpp, so this Widget reads identically
// to the load bars it's meant to replace.
inline const uint16_t kPercentageBarColorLow = gfx_rgb(60, 200, 90);
inline const uint16_t kPercentageBarColorMid = gfx_rgb(240, 180, 0);
inline const uint16_t kPercentageBarColorHigh = gfx_rgb(230, 60, 50);

// The fixed severity threshold every PercentageBar (and Resource bar) fill
// color derives from: <50% green, <80% amber, else red.
inline uint16_t percentage_bar_severity_color(float percent) {
    if (percent < kPercentageBarThresholdLow) return kPercentageBarColorLow;
    if (percent < kPercentageBarThresholdMid) return kPercentageBarColorMid;
    return kPercentageBarColorHigh;
}

// A ±2%-style hysteresis band on top of ValueBar<float>'s own latch: a load
// reading's natural EMA jitter shouldn't cause a redraw on every tick. Kept
// local to this Widget rather than generalized onto ValueBar/ValueRow's
// shared latch, since no other Widget has this need.
inline constexpr float kPercentageBarHysteresis = 2.0f;

// PercentageBar's own state is nothing but a ValueBar<float> -- the shared
// latch (bar.value/bar.initialized) is also what the hysteresis check reads
// from, so no separate "last drawn" field is needed.
struct PercentageBar {
    ValueBar<float> bar;
};

// Draws `label` overlaid on a bar spanning [x, x+w) x [y, y+h), filled to
// `percent` (clamped to [0,100]) and colored by percentage_bar_severity_color().
// Skips the redraw (and leaves the latch untouched) when `percent` is within
// kPercentageBarHysteresis of the latch's current value, even though that's
// not an exact match -- ValueBar<float>::value_bar_draw_value()'s own
// exact-equality latch still runs underneath for the values that do clear
// the band, so a jitter-free repeated value is still a no-op either way.
inline void percentage_bar_draw_value(PercentageBar &pb, float percent, const char *label, int x,
                                       int y, int w, int h, int scale, uint16_t fg,
                                       uint16_t off_color) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;

    if (pb.bar.initialized) {
        float delta = percent - pb.bar.value;
        if (delta < 0.0f) delta = -delta;
        if (delta < kPercentageBarHysteresis) return;
    }

    uint16_t fill_color = percentage_bar_severity_color(percent);
    value_bar_draw_value(pb.bar, percent, label, percent / 100.0f, x, y, w, h, scale, fg,
                          fill_color, off_color);
}
