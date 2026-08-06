#include "midi_controller.h"

// Stub (#13): no pattern player yet, so there is nothing for incoming MIDI
// to drive — the tracker plays from its own module data, not live notes.
// Exists only so main.cpp's transport polling still links; also keeps this
// engine off the shared src/midi/midi_controller.cpp, which allocates voices
// via voice_alloc — not linked into this build (see CMakeLists.txt, #13).
static MidiUiState ui_state;

void midi_controller_init() {
    ui_state = { 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
}

void midi_controller_process(const uint8_t *, uint32_t, ParamExchange *) {
    // Ignored — no tracker logic yet.
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
