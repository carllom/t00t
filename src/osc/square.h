#pragma once

#include "common.h"
#include <cstdint>

// Square oscillator with variable duty cycle.
// duty_cycle: 0–1023 threshold within the wavetable (512 = 50%).
// Returns sample in [-32767..32767].
inline int32_t osc_square(uint32_t phase, uint16_t duty_cycle) {
    uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    return (idx < duty_cycle) ? 32767 : -32767;
}
