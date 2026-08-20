#pragma once

#include "sensor_event.h"
#include "input_layer.h"

// Config-driven Shaping for a GPIO button: supplies the note identity,
// MIDI channel, and fixed-value velocity substitution a bare on/off switch
// has no natural signal for -- entirely in source-native (MIDI note/
// velocity) terms, so the resulting Input event means the same thing to
// any module a MIDI note-on/off would.
struct ButtonShapingConfig {
    uint8_t note;            // note number this button plays
    uint8_t channel;         // channel this button plays on
    uint8_t fixed_velocity;  // velocity substituted on press
};

// Shape an already-debounced button SensorEvent into an Input event, ready
// for input_dispatch() under InputCategory::NOTE -- the same shape a MIDI
// note-on/off produces.
inline InputValue shape_button_event(const SensorEvent &ev, const ButtonShapingConfig &cfg) {
    InputValue value{};
    value.channel = cfg.channel;
    value.note = cfg.note;
    value.note_on = (ev.edge == SENSOR_PRESSED);
    value.velocity = value.note_on ? cfg.fixed_velocity : 0;
    return value;
}
