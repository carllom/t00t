#pragma once

#include "../engine.h"
#include <cstdint>

// Transport-agnostic MIDI controller.
// Parses raw MIDI bytes, maps notes to voices via the allocator.
// Fed by any transport (USB, UART, etc.).

void midi_controller_init();

// Feed raw MIDI bytes. Parses, handles note on/off, commits if changed.
void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params);
