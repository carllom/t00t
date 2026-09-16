#pragma once

#include <cstdint>

// Page-navigation cursor state machine (issue #126, part of #124's shared
// Widget/Header/Page library, see CONTEXT.md's Page/UI command entries).
// A module supplies only its own ordered Page list -- expressed here as a
// page count plus the index within it of the required Performance page --
// and this owns +/-/enter/exit's cursor advance/wrap/jump behavior: one
// implementation reused by all 7 engine modules instead of seven forks.
// No LCD/rendering dependency; wiring a real Sensor event -> Shaping
// stream into UiCommand is issue #132, out of scope here.

// The parsed form of one of the four generic UI commands. Named generically
// (not Next/Previous) since the vocabulary is meant to be reused by future
// interaction contexts beyond Page navigation (value editing, confirm/
// cancel) -- see CONTEXT.md's UI command entry.
enum class UiCommand : uint8_t {
    PLUS,   // aka Increase, Next
    MINUS,  // aka Decrease, Previous
    ENTER,  // aka select, yes -- no Page-navigation meaning; see below
    EXIT,   // aka no
};

// The current-Page index for one module instance. A module never
// implements its own advance/wrap/jump logic -- it only tells
// page_cursor_apply() how many Pages it has and which one is its required
// Performance page.
struct PageCursor {
    uint8_t index = 0;
};

// Apply one UiCommand to `cursor`, given the module's Page count and the
// index of its required Performance page within that ordered list.
// PLUS/MINUS advance/retreat by one, wrapping at both ends; EXIT jumps
// straight to `performance_index` regardless of the current position;
// ENTER is recognized but leaves `cursor` untouched, since this spec
// defines no Page-navigation meaning for it. `page_count` is always >= 1
// (Performance is required) -- a single-Page module's PLUS/MINUS is a
// no-op because wrapping a 1-entry list always lands back on itself.
inline void page_cursor_apply(PageCursor &cursor, UiCommand command, uint8_t page_count,
                               uint8_t performance_index) {
    if (page_count == 0) return;  // violates the "Performance is required" precondition; no-op rather than UB
    switch (command) {
        case UiCommand::PLUS:
            cursor.index = (cursor.index + 1) % page_count;
            break;
        case UiCommand::MINUS:
            cursor.index = (cursor.index == 0) ? page_count - 1 : cursor.index - 1;
            break;
        case UiCommand::EXIT:
            cursor.index = performance_index;
            break;
        case UiCommand::ENTER:
            break;
    }
}
