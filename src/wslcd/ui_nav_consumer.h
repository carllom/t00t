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

// An interaction context that gives UI commands a more specific meaning
// than Page navigation (e.g. a Page's value editing): returns true if it
// took the command, false to leave it to Page navigation.
using UiCommandClaim = bool (*)(UiCommand command);

// Apply one UI command: to `claim` first if there is one, else to
// `cursor` as Page navigation.
inline void ui_nav_consumer_apply(PageCursor &cursor, UiCommand command, uint8_t page_count,
                                  uint8_t performance_index, UiCommandClaim claim = nullptr) {
    if (claim && claim(command)) return;
    page_cursor_apply(cursor, command, page_count, performance_index);
}

// Feed one already-debounced Sensor event through Shaping and, if it maps
// to a UI command, apply it (ui_nav_consumer_apply()). Returns whether a
// command fired, so a caller (e.g. a display's redraw-only-if-changed
// check) doesn't need to re-derive Shaping's own press-edge predicate.
inline bool ui_nav_consumer_feed(PageCursor &cursor, const SensorEvent &ev, const UiNavConfig &cfg,
                                 UiCommandClaim claim = nullptr) {
    UiCommand command;
    if (!shape_ui_command_event(ev, cfg.shaping, &command)) return false;
    ui_nav_consumer_apply(cursor, command, cfg.page_count, cfg.performance_index, claim);
    return true;
}
