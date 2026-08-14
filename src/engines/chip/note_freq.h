#pragma once

#include "chip/sid_osc.h"   // SID_CLOCK_PAL
#include "chip/ay_osc.h"    // AY_CLOCK_ZX
#include <cmath>
#include <cstdint>

// MIDI note <-> chip frequency-register conversion, for every chip type
// this module builds (module_chip.md §12: "additional VoiceTypes in this module").
// Shared by Core 0 (midi_controller.cpp, note-on/pitch-bend) and Core 1
// (the SID frame VM doesn't use this directly -- it applies arpeggio/
// vibrato as ratios on an already-converted register -- but keeping one
// definition avoids two copies drifting).

inline float chip_note_to_hz(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

inline uint16_t chip_hz_to_freq_reg(float hz) {
    // inc = freq_reg * (clock/rate*256); solving sid_freq_to_inc's contract
    // for freq_reg directly (module_chip.md §4.1's "16-bit frequency register"):
    // freq_reg = hz * 2^24 / clock_hz.
    float reg = hz * 16777216.0f / (float)SID_CLOCK_PAL;
    if (reg < 0.0f) reg = 0.0f;
    if (reg > 65535.0f) reg = 65535.0f;
    return (uint16_t)(reg + 0.5f);
}

// AY-3-8910/YM2149: the tone generator counts *down* period, not up in
// proportion to frequency (ay_osc.h's header comment) -- freq = clock/16/
// period, so period = clock/16/hz, inverted from SID's freq_reg. Clamped to
// the real 12-bit register range; 1 rather than 0 at the top end (AyTone
// already clamps 0 to 1 itself, but doing it here means the value stored in
// VoiceParams.freq is always the true register value, not a placeholder
// that happens to get fixed up downstream).
inline uint16_t ay_hz_to_period(float hz) {
    if (hz < 1.0f) hz = 1.0f;
    float period = (float)AY_CLOCK_ZX / 16.0f / hz;
    if (period < 1.0f) period = 1.0f;
    if (period > 4095.0f) period = 4095.0f;
    return (uint16_t)(period + 0.5f);
}
