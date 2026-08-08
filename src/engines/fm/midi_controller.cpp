#include "midi_controller.h"

// Stub (#41): no patch/note logic yet (fm.md P1, the DAG routing/algorithm
// resolution at note-on), so there is nothing for incoming MIDI to drive.
// Exists only so main.cpp's transport polling still links; also keeps this
// engine off the shared src/midi/midi_controller.cpp, which expects a
// presets.h/VoicePreset shape this engine's minimal VoiceParams (engine.h)
// doesn't have -- same reasoning as the speech engine's stub (#27).
static MidiUiState ui_state;

void midi_controller_init() {
    ui_state = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
}

void midi_controller_process(const uint8_t *, uint32_t, ParamExchange *) {
    // Ignored — no patch/note logic yet.
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
