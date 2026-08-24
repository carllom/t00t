#pragma once

#include "gfx.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

// VoiceGrid Widget (see CONTEXT.md's VoiceGrid entry): one cell per voice, each
// showing a short text label and one of three caller-assigned colors --
// independent of ActivityGrid, not a variant of it. The three color slots are
// generic (index 0..2 into the caller's own `colors` array): this header has
// no "held"/"ringing" naming or behavior anywhere in it, so a future module
// can assign the slots to whatever 3-state concept it needs.
//
// Per-cell state lives in one array of VoiceGridCell per Widget instance
// (VoiceGrid<N>::cells). Unlike ActivityGrid's fixed dot geometry, a cell
// here carries a text label, so its pixel width depends on caller-chosen
// label length -- column/row pitch, columns-per-row, label width and text
// scale are all draw-call parameters, not fixed constants. `initialized`
// marks the not-yet-drawn state explicitly, same convention as
// ValueRow<T>/ValueBar<T>/ActivityGrid<N>.

// Largest label this Widget will latch/redraw. Comfortably covers a
// "voice:instrument/table-row"-style label (e.g. "3:2/07") with headroom.
inline constexpr int kVoiceGridMaxLabelChars = 15;

struct VoiceGridCell {
    char label[kVoiceGridMaxLabelChars + 1] = {};
    uint8_t color_slot = 0;
};

template <int N>
struct VoiceGrid {
    VoiceGridCell cells[N]{};
    bool initialized = false;
};

// Draws every cell 0..N-1 of `grid`, rooted at (x, y): cell i sits at column
// i % cols, row i / cols (each column `col_pitch` px apart, each row
// `row_pitch` px apart). `labels[i]` is blank-padded to `char_width` chars
// and drawn at `scale` in `colors[color_slots[i]]` on `bg`. Redraws only the
// cells whose label or color_slot differs from `grid`'s latch (or, on the
// first call, every cell). Updates the latch immediately, on this same call.
template <int N>
void voice_grid_draw(VoiceGrid<N> &grid, const char *const *labels, const uint8_t *color_slots,
                      int x, int y, int cols, int col_pitch, int row_pitch, int char_width,
                      int scale, const uint16_t colors[3], uint16_t bg) {
    for (int i = 0; i < N; i++) {
        const char *label = labels[i];
        uint8_t slot = color_slots[i];
        VoiceGridCell &cell = grid.cells[i];
        bool changed = !grid.initialized || slot != cell.color_slot
                       || strncmp(cell.label, label, kVoiceGridMaxLabelChars) != 0;
        if (changed) {
            int cx = x + (i % cols) * col_pitch;
            int cy = y + (i / cols) * row_pitch;

            int n = char_width < 0 ? 0
                    : (char_width < kVoiceGridMaxLabelChars ? char_width : kVoiceGridMaxLabelChars);
            char buf[kVoiceGridMaxLabelChars + 1];
            snprintf(buf, sizeof(buf), "%-*.*s", n, n, label);
            gfx_text(cx, cy, buf, colors[slot], bg, scale);

            snprintf(cell.label, sizeof(cell.label), "%.*s", kVoiceGridMaxLabelChars, label);
            cell.color_slot = slot;
        }
    }
    grid.initialized = true;
}
