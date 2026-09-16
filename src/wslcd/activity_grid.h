#pragma once

#include "gfx.h"

#include <cstdint>

// ActivityGrid Widget (see CONTEXT.md's ActivityGrid entry): one binary
// on/off cell per voice or channel, arranged in fixed-pitch rows that wrap
// at a hard cell-per-row count. Cell count is this Widget's only shape
// parameter -- cell size, pitch, and the row-wrap point are fixed
// (kActivityGridCellsPerRow is exactly LCD_W / kActivityGridCellPitch), not
// call parameters. A grid wider than one row wraps onto additional rows,
// `row_pitch` px apart -- the one vertical-layout choice left to the
// caller.
//
// Per-cell state lives in one array of ActivityGridCell per Widget
// instance (ActivityGrid<N>::cells), so a cell's fields can't desync from
// each other the way independent parallel arrays can. Same
// redraw-only-if-changed ownership as every other Widget: `initialized`
// marks the not-yet-drawn state explicitly, like ValueRow<T>/ValueBar<T>.

inline constexpr int kActivityGridCellPitch = 15;
inline constexpr int kActivityGridCellW = 13;
inline constexpr int kActivityGridCellH = 14;
inline constexpr int kActivityGridCellsPerRow = 16;

struct ActivityGridCell {
    bool active = false;
};

template <int N>
struct ActivityGrid {
    ActivityGridCell cells[N]{};
    bool initialized = false;
};

// Draws every cell 0..N-1 of `grid`, rooted at (x, y): cell i sits at
// column i % kActivityGridCellsPerRow, row i / kActivityGridCellsPerRow
// (each row `row_pitch` px below the previous). Redraws only the cells
// whose `active[i]` differs from `grid`'s latch (or, on the first call,
// every cell). Updates the latch immediately, on this same call.
template <int N>
void activity_grid_draw(ActivityGrid<N> &grid, const bool *active, int x, int y, int row_pitch,
                         uint16_t on_color, uint16_t off_color) {
    for (int i = 0; i < N; i++) {
        bool a = active[i];
        if (!grid.initialized || grid.cells[i].active != a) {
            int cx = x + (i % kActivityGridCellsPerRow) * kActivityGridCellPitch;
            int cy = y + (i / kActivityGridCellsPerRow) * row_pitch;
            gfx_fill_rect(cx, cy, kActivityGridCellW, kActivityGridCellH, a ? on_color : off_color);
            grid.cells[i].active = a;
        }
    }
    grid.initialized = true;
}
