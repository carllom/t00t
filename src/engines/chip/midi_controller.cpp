#include "midi/midi_controller.h"

// Chip module F0 measurement build (sid.md §1 P0: "No engine, no VM").
//
// The rig is self-cycling and drives its voices directly (rig.h,
// audio_engine.cpp), so MIDI input has nothing to reach. These stubs exist so
// the shared main.cpp links unchanged rather than growing another per-engine
// #if. P1 replaces them with the real static MIDI-channel -> voice map
// (sid.md §8: "MIDI channel -> voice, no allocator. Follows the groovebox.").
void midi_controller_init() {}

void midi_controller_process(const uint8_t *, uint32_t, ParamExchange *) {}

void midi_controller_ui_state(MidiUiState *out) {
    *out = MidiUiState{0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}
