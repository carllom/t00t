#pragma once

#include "common.h"
#include <cstdint>

// Sine wavetable — generated at runtime by osc_init_tables()
extern int16_t sine_table[WAVETABLE_SIZE];

void osc_init_sine();

// Sine oscillator: wavetable lookup with linear interpolation.
// Returns sample in [-32767..32767].
inline int32_t osc_sine(uint32_t phase) {
    uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    uint32_t frac = phase & ((1 << PHASE_FRAC_BITS) - 1);

    int16_t s0 = sine_table[idx];
    int16_t s1 = sine_table[(idx + 1) & WAVETABLE_MASK];
    return s0 + (((int32_t)(s1 - s0) * (int32_t)frac) >> PHASE_FRAC_BITS);
}
