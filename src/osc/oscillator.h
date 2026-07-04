#pragma once

#include "../engine_base.h"
#include "sine.h"
#include "square.h"
#include "triangle.h"
#include "saw.h"
#include "noise.h"
#include "square_blep.h"
#include "saw_blep.h"

// Dispatch to the correct oscillator based on waveform type.
// Inline so the compiler sees through the switch in the hot loop.
// noise_lfsr: per-voice LFSR state (only used for WAVE_NOISE).
// phase_inc: needed by BLEP variants for correction width.
inline int32_t osc_sample(Waveform waveform, uint32_t phase, uint16_t duty_cycle,
                          uint16_t &noise_lfsr, uint32_t phase_inc) {
    switch (waveform) {
        case WAVE_SQUARE:      return osc_square(phase, duty_cycle);
        case WAVE_SQUARE_BLEP: return osc_square_blep(phase, duty_cycle, phase_inc);
        case WAVE_TRIANGLE:    return osc_triangle(phase);
        case WAVE_SAW:         return osc_saw(phase);
        case WAVE_SAW_BLEP:    return osc_saw_blep(phase, phase_inc);
        case WAVE_NOISE:       return osc_noise(noise_lfsr);
        default:               return osc_sine(phase);
    }
}
