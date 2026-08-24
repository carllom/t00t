#pragma once

#include "../sensor_event.h"
#include "page.h"
#include "ui_command_shaping.h"

// The shared UI-navigation consumer (issue #132, part of #124's Widget/
// Header/Page library). The one place a Shaped UI command reaches #126's
// PageCursor state machine -- taking a raw Sensor event directly and never
// constructing or touching an InputValue, so a UI command structurally
// cannot reach input_dispatch()/a module's audio-facing Handler. One
// implementation reused by every module the same way page_cursor_apply()
// itself already is; a module supplies only its own Page count and
// Performance-page index, exactly what page_cursor_apply() needs.
//
// Fully optional at the board/config level by construction: nothing in
// Core 0's audio/MIDI path (main.cpp, controller.cpp) includes this file or
// calls into it, so a board with no physical UI-navigation control wired --
// or a `HAS_LCD=0` build, which never links src/wslcd/ at all -- carries no
// compiled-in reference that forces this consumer to run.
struct UiNavConfig {
    UiCommandShapingConfig shaping;
    uint8_t page_count;
    uint8_t performance_index;
};

// Feed one already-debounced Sensor event through Shaping and, if it maps
// to a UI command, apply it to `cursor`. Returns whether a command fired,
// so a caller (e.g. a display's redraw-only-if-changed check) doesn't need
// to re-derive Shaping's own press-edge predicate.
inline bool ui_nav_consumer_feed(PageCursor &cursor, const SensorEvent &ev, const UiNavConfig &cfg) {
    UiCommand command;
    if (!shape_ui_command_event(ev, cfg.shaping, &command)) return false;
    page_cursor_apply(cursor, command, cfg.page_count, cfg.performance_index);
    return true;
}
