#pragma once

#include "common.h"
#include "polyblep.h"
#include <cstdint>

// Band-limited square wave via PolyBLEP.
// Smooths both the rising edge (phase=0) and falling edge (phase=duty_cycle).
// duty_cycle: 0–1023 threshold. phase_inc: for correction width.
inline int32_t osc_square_blep(uint32_t phase, uint16_t duty_cycle, uint32_t phase_inc) {
    uint32_t phase_mod = phase & PHASE_CYCLE_MASK;
    uint32_t idx = (phase_mod >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
    // Naive square
    int32_t sq = (idx < duty_cycle) ? 32767 : -32767;
    // PolyBLEP at rising edge (phase=0, transition from -1 to +1)
    sq += polyblep(phase_mod, phase_inc);
    // PolyBLEP at falling edge (phase=duty_cycle, transition from +1 to -1)
    uint32_t duty_phase = (uint32_t)duty_cycle << PHASE_FRAC_BITS;
    uint32_t shifted = (phase_mod + PHASE_CYCLE - duty_phase) & PHASE_CYCLE_MASK;
    sq -= polyblep(shifted, phase_inc);
    return sq;
}
