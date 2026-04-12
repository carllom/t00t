#pragma once

#include "common.h"
#include <cstdint>

// Sawtooth oscillator: linear ramp from -32767 to +32767 across the full cycle.
// Returns sample in [-32767..32767].
inline int32_t osc_saw(uint32_t phase) {
    uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    // Map 0..1023 to -32767..+32767
    return (int32_t)(idx * 2 * 32767 / (WAVETABLE_SIZE - 1)) - 32767;
}
