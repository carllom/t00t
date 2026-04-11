#pragma once

#include "common.h"
#include <cstdint>

// Square oscillator: first half of phase cycle = +32767, second = -32767.
// Returns sample in [-32767..32767].
inline int32_t osc_square(uint32_t phase) {
    uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    return (idx < WAVETABLE_SIZE / 2) ? 32767 : -32767;
}
