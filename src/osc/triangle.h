#pragma once

#include "common.h"
#include <cstdint>

// Triangle oscillator: linear ramp up in first half, linear ramp down in second.
// Returns sample in [-32767..32767].
inline int32_t osc_triangle(uint32_t phase) {
    uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    // Map index 0..1023 to triangle:
    // 0..255 → 0..32767, 256..511 → 32767..0, 512..767 → 0..-32767, 768..1023 → -32767..0
    uint32_t quarter = WAVETABLE_SIZE / 4;  // 256
    if (idx < quarter) {
        return (int32_t)(idx * 32767 / (quarter - 1));
    } else if (idx < 2 * quarter) {
        return (int32_t)((2 * quarter - 1 - idx) * 32767 / (quarter - 1));
    } else if (idx < 3 * quarter) {
        return -(int32_t)((idx - 2 * quarter) * 32767 / (quarter - 1));
    } else {
        return -(int32_t)((4 * quarter - 1 - idx) * 32767 / (quarter - 1));
    }
}
