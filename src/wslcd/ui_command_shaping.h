#pragma once

#include "../sensor_event.h"
#include "page.h"

// Config-driven Shaping for a GPIO button that produces a UI command
// (issue #132, CONTEXT.md's "UI command" entry): the UiCommand sibling of
// button_shaping.h's NOTE-event Shaping. Same Sensor event -> Shaping stage
// boundary and the same "config supplies what a bare switch has no natural
// signal for" role button_shaping.h fills for note/channel/velocity -- here
// a button's id has no natural command meaning, so Shaping supplies which
// one of the four generic commands its press means. Diverges from
// button_shaping.h's output type (InputValue) rather than its mechanism,
// since a UI command is consumed by the UI-navigation consumer
// (ui_nav_consumer.h), never by input_dispatch()/the Router.
struct UiCommandShapingConfig {
    UiCommand command;  // which UI command this button's press edge fires
};

// Shape an already-debounced button SensorEvent into a UiCommand. A UI
// command is a discrete, one-shot trigger (unlike NOTE's on/off pair), so
// this fires on the press edge only; a release edge or a non-button
// SensorEvent (e.g. a pot reading) leaves *out untouched and returns false,
// letting a caller tell "no command fired" from "command fired."
inline bool shape_ui_command_event(const SensorEvent &ev, const UiCommandShapingConfig &cfg,
                                    UiCommand *out) {
    if (ev.kind != SENSOR_BUTTON || ev.edge != SENSOR_PRESSED) return false;
    *out = cfg.command;
    return true;
}
