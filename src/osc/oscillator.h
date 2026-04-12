#pragma once

#include "../engine.h"
#include "sine.h"
#include "square.h"
#include "triangle.h"
#include "saw.h"
#include "noise.h"

// Dispatch to the correct oscillator based on waveform type.
// Inline so the compiler sees through the switch in the hot loop.
// noise_lfsr is a per-voice LFSR state reference (only used for WAVE_NOISE).
inline int32_t osc_sample(Waveform waveform, uint32_t phase, uint16_t duty_cycle, uint16_t &noise_lfsr) {
    switch (waveform) {
        case WAVE_SQUARE:   return osc_square(phase, duty_cycle);
        case WAVE_TRIANGLE: return osc_triangle(phase);
        case WAVE_SAW:      return osc_saw(phase);
        case WAVE_NOISE:    return osc_noise(noise_lfsr);
        default:            return osc_sine(phase);
    }
}
