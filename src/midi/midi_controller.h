#pragma once

#include "engine.h"
#include <cstdint>

// Transport-agnostic MIDI controller.
// Parses raw MIDI bytes, maps notes to voices via the allocator.
// Fed by any transport (USB, UART, etc.).

void midi_controller_init();

// Feed raw MIDI bytes. Parses, handles note on/off, commits if changed.
void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params);

// --- Snapshot of recent MIDI activity for a display/UI (Core 0) ---
struct MidiUiState {
    uint8_t last_note;      // 0..127, 0xFF = none yet
    uint8_t last_velocity;  // 0..127
    uint8_t last_channel;   // 0..15
    uint8_t program;        // preset index in use on last_channel
    int16_t bend;           // signed offset from centre (-8192..+8191)
    uint8_t mod;            // mod wheel (CC1), 0..127
    uint8_t fx_type;        // effect type (CC74): 0=off, 1=delay, 2=reverb
    uint8_t fx_mix;         // wet/dry (CC73), 0..127
    uint8_t fx_p1;          // CC72: delay feedback / reverb room size, 0..127
    uint8_t fx_p2;          // CC75: delay time / reverb damping, 0..127
};
void midi_controller_ui_state(MidiUiState *out);
