#pragma once

#include <cstdint>

// Source-specific parsed event for a non-MIDI physical input (a GPIO
// button, a potentiometer, ...). Carries only values in the input's own
// native terms -- no unit conversion, no module-specific meaning attached.
// Debouncing (and, for a continuous source, any other signal-shaping)
// happens upstream of this file; construction here only wraps an
// already-settled reading into a fixed shape.

enum SensorKind : uint8_t {
    SENSOR_BUTTON = 0,  // discrete on/off input
    SENSOR_POT    = 1,  // continuous input, sampled as a raw reading
};

enum SensorEdge : uint8_t {
    SENSOR_RELEASED = 0,
    SENSOR_PRESSED  = 1,
};

// Field meaning depends on kind:
//   SENSOR_BUTTON : edge = pressed/released, raw unused
//   SENSOR_POT    : raw  = sampled reading (e.g. ADC counts), edge unused
struct SensorEvent {
    SensorKind kind;
    uint8_t id;   // source index (which button/pot this reading is from)
    SensorEdge edge;
    uint16_t raw;
};

// Build a SensorEvent from an already-debounced button edge.
inline SensorEvent sensor_event_button(uint8_t id, bool pressed) {
    SensorEvent ev{};
    ev.kind = SENSOR_BUTTON;
    ev.id = id;
    ev.edge = pressed ? SENSOR_PRESSED : SENSOR_RELEASED;
    ev.raw = 0;
    return ev;
}

// Build a SensorEvent from an already-sampled continuous reading.
inline SensorEvent sensor_event_pot(uint8_t id, uint16_t raw) {
    SensorEvent ev{};
    ev.kind = SENSOR_POT;
    ev.id = id;
    ev.edge = SENSOR_RELEASED;
    ev.raw = raw;
    return ev;
}
