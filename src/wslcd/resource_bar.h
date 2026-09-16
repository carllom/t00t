#pragma once

#include "gfx.h"
#include "percentage_bar.h"

#include <cstdint>

// Resource bar Widget (issue #135, part of #124's shared Widget/Header/Page
// library -- see CONTEXT.md's Resource bar entry): a Header-chrome indicator
// folding CPU load and active-voice-count into one unlabeled bar -- fill
// length is active voices / MAX_VOICES, fill color is PercentageBar
// (#133)'s severity threshold driven by CPU load, carrying no text of its
// own. Read the way an LED cluster on a hardware synth panel is read:
// meaning lives off-device, not on screen.
//
// Reference implementation: prototype_compact_widgets.html on branch
// prototype/compact-widgets (Variant D, refined post-prototype into this
// single merged bar).

// The bar's total width (backdrop + fill), fixed regardless of a module's
// real MAX_VOICES -- the largest real value (chip/tracker, 32) maps 1:1;
// every other module's fill is proportionally narrower within this same
// width rather than the whole bar shrinking.
inline constexpr int kResourceBarMaxPx = 32;

// round(active_voices * kResourceBarMaxPx / max_voices) via the standard
// integer round-half-up trick (add half the divisor before truncating
// division) -- avoids a float round() for a value that's just going to be
// used as a pixel count. `active_voices` is clamped to [0, max_voices]
// first so a stale/out-of-range reading can't overshoot the fixed width.
inline int resource_bar_fill_px(int active_voices, int max_voices) {
    if (max_voices <= 0) return 0;
    if (active_voices < 0) active_voices = 0;
    if (active_voices > max_voices) active_voices = max_voices;
    return (active_voices * kResourceBarMaxPx + max_voices / 2) / max_voices;
}

// Placement's corner-mask clearance reuses gfx.h's gfx_corner_safe_margin()
// directly -- it's general panel geometry (also load-bearing for header.h's
// row 1), not something specific to this Widget.

// A ResourceBar instance's own latch: the two independent readings it folds
// together, compared as one value (exact equality per field, same as
// ValueBar's own latch) so a change in either one triggers a redraw. No
// hysteresis of its own -- PercentageBar's jitter-suppressing band (#133)
// stays specific to that Widget rather than being generalized here, so a
// cpu_percent that jitters without crossing severity_color()'s threshold
// still redraws even though the drawn color doesn't change.
struct ResourceBarValue {
    int active_voices = 0;
    int max_voices = 0;
    float cpu_percent = 0.0f;

    bool operator==(const ResourceBarValue &o) const {
        return active_voices == o.active_voices && max_voices == o.max_voices &&
               cpu_percent == o.cpu_percent;
    }
    bool operator!=(const ResourceBarValue &o) const { return !(*this == o); }
};

struct ResourceBar {
    ResourceBarValue value{};
    bool initialized = false;
};

// Draws the bar spanning [x, x+kResourceBarMaxPx) x [y, y+h): filled to
// resource_bar_fill_px(active_voices, max_voices) in
// percentage_bar_severity_color(cpu_percent), the rest in `off_color`. Only
// redraws when (active_voices, max_voices, cpu_percent) differ from `rb`'s
// latch (or `rb` hasn't drawn yet); updates the latch immediately, on this
// same call.
inline void resource_bar_draw_value(ResourceBar &rb, int active_voices, int max_voices,
                                     float cpu_percent, int x, int y, int h, uint16_t off_color) {
    ResourceBarValue v{active_voices, max_voices, cpu_percent};
    if (rb.initialized && rb.value == v) return;

    int fill_px = resource_bar_fill_px(active_voices, max_voices);
    uint16_t fill_color = percentage_bar_severity_color(cpu_percent);

    gfx_fill_rect(x, y, fill_px, h, fill_color);
    gfx_fill_rect(x + fill_px, y, kResourceBarMaxPx - fill_px, h, off_color);

    rb.value = v;
    rb.initialized = true;
}
