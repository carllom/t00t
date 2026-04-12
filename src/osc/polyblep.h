#pragma once

#include "common.h"
#include <cstdint>

// PolyBLEP residual for a discontinuity at phase_mod=0.
// Smooths the step over one sample on each side of the transition.
// Returns correction scaled to [-32767..32767].
// Uses Q10 fixed-point internally (matches PHASE_FRAC_BITS).
// Division uses RP2040 hardware divider (~8 cycles).
inline int32_t polyblep(uint32_t phase_mod, uint32_t phase_inc) {
    if (phase_mod < phase_inc) {
        // Just past the transition: t in [0, 1)
        int32_t t = ((int32_t)phase_mod << PHASE_FRAC_BITS) / (int32_t)phase_inc;
        int32_t poly = 2 * t - ((t * t) >> PHASE_FRAC_BITS) - (1 << PHASE_FRAC_BITS);
        return (poly * 32767) >> PHASE_FRAC_BITS;
    }
    if (phase_mod > PHASE_CYCLE - phase_inc) {
        // Just before the transition: t in (-1, 0]
        int32_t t = (((int32_t)phase_mod - (int32_t)PHASE_CYCLE) << PHASE_FRAC_BITS) / (int32_t)phase_inc;
        int32_t poly = ((t * t) >> PHASE_FRAC_BITS) + 2 * t + (1 << PHASE_FRAC_BITS);
        return (poly * 32767) >> PHASE_FRAC_BITS;
    }
    return 0;
}
