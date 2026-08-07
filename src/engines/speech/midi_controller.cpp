#include "midi_controller.h"

// Stub (#27): no phoneme keyboard yet (speech.md Phase 1, SPEECH_HOLD), so
// there is nothing for incoming MIDI to drive. Exists only so main.cpp's
// transport polling still links; also keeps this engine off the shared
// src/midi/midi_controller.cpp, which expects a presets.h/VoicePreset shape
// this engine's minimal VoiceParams (engine.h) doesn't have.
static MidiUiState ui_state;

void midi_controller_init() {
    ui_state = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
}

void midi_controller_process(const uint8_t *, uint32_t, ParamExchange *) {
    // Ignored — no phoneme keyboard yet.
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
