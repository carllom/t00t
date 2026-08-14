#pragma once

#include "chip/sid_osc.h"   // SID_CLOCK_PAL
#include <cmath>
#include <cstdint>

// MIDI note <-> SID frequency register conversion. Shared by Core 0
// (midi_controller.cpp, note-on/pitch-bend) and Core 1 (audio_engine.cpp's
// frame VM doesn't use this directly -- it applies arpeggio/vibrato as
// ratios on an already-converted register -- but keeping one definition
// avoids two copies drifting).

inline float chip_note_to_hz(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

inline uint16_t chip_hz_to_freq_reg(float hz) {
    // inc = freq_reg * (clock/rate*256); solving sid_freq_to_inc's contract
    // for freq_reg directly (sid.md §4.1's "16-bit frequency register"):
    // freq_reg = hz * 2^24 / clock_hz.
    float reg = hz * 16777216.0f / (float)SID_CLOCK_PAL;
    if (reg < 0.0f) reg = 0.0f;
    if (reg > 65535.0f) reg = 65535.0f;
    return (uint16_t)(reg + 0.5f);
}
