#pragma once

#include "common.h"
#include "polyblep.h"
#include <cstdint>

// Band-limited sawtooth via PolyBLEP.
// Smooths the discontinuity at the phase wrap point.
// phase_inc needed to scale the correction width.
inline int32_t osc_saw_blep(uint32_t phase, uint32_t phase_inc) {
    uint32_t phase_mod = phase & PHASE_CYCLE_MASK;
    // Naive saw using full 20-bit phase for smooth base waveform
    // (phase_mod >> 5) maps [0, PHASE_CYCLE) to [0, 32767]
    int32_t saw = (int32_t)(phase_mod >> 5) * 2 - 32767;
    // Subtract PolyBLEP at the falling discontinuity (wrap from +32767 to -32767)
    saw -= polyblep(phase_mod, phase_inc);
    return saw;
}
