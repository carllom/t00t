#pragma once

#include "../audio_common.h"
#include <cstdint>

// Wavetable size must be power of 2 for fast masking
static constexpr uint32_t WAVETABLE_SIZE = 1024;
static constexpr uint32_t WAVETABLE_MASK = WAVETABLE_SIZE - 1;

// Fixed-point phase: 22.10 format (22 integer bits, 10 fractional bits)
// Integer part indexes into wavetable, fractional part for interpolation
static constexpr uint32_t PHASE_FRAC_BITS = 10;

// Compute phase_inc for a given frequency (used by Core 0 to fill VoiceParams)
inline uint32_t osc_phase_inc(float freq_hz) {
    return (uint32_t)((freq_hz / (float)SAMPLE_RATE) * (float)WAVETABLE_SIZE * (float)(1 << PHASE_FRAC_BITS));
}
