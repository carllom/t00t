#pragma once

#include "audio_common.h"
#include <cmath>
#include <cstdint>

// FM operator sine table (module_fm.md §5.1, §41): the FM engine's own 4096-entry
// table, distinct from the shared osc/sine.h 1024-entry interpolating one.
// module_fm.md §3.5: interpolation costs ~45% more per operator for no audible
// benefit under FM's own harmonic density, so operators index this table
// directly with the top FM_TABLE_BITS of a 32-bit phase accumulator -- no
// interpolation, no mask (the shift already produces an index of exactly
// the right width, so phase wrap is free).
static constexpr uint32_t FM_TABLE_BITS = 12;
static constexpr uint32_t FM_TABLE_SIZE = 1u << FM_TABLE_BITS;  // 4096
static constexpr uint32_t FM_PHASE_SHIFT = 32 - FM_TABLE_BITS;  // 20

// Header-only inline variable (C++17), same pattern as res2p.h's
// res2p_radius_lut -- no separate .cpp needed, and the CMake engine-source
// list (top-level CMakeLists.txt) doesn't need to know about it.
inline int16_t fm_sine_table[FM_TABLE_SIZE];

// Quarter-wave symmetric generation -- only the first quarter is computed
// with sinf(), the rest mirrored/negated into place -- but the full table is
// stored: the lookup itself never unfolds symmetry at runtime (module_fm.md §5.1:
// "8 KB is cheaper than the instructions").
inline void fm_init_sine_tab() {
    constexpr uint32_t quarter = FM_TABLE_SIZE / 4;

    // First quarter [0, quarter]: computed directly.
    for (uint32_t i = 0; i <= quarter; i++) {
        fm_sine_table[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / (float)FM_TABLE_SIZE));
    }
    // Second quarter (quarter, 2*quarter): mirror of the first.
    for (uint32_t i = 1; i < quarter; i++) {
        fm_sine_table[2 * quarter - i] = fm_sine_table[i];
    }
    // Second half [2*quarter, 4*quarter): negation of the first half.
    for (uint32_t i = 0; i < 2 * quarter; i++) {
        fm_sine_table[2 * quarter + i] = (int16_t)(-fm_sine_table[i]);
    }
}

// No interpolation: phase >> FM_PHASE_SHIFT (== phase >> 20) indexes
// directly. module_fm.md §3.2's kernel comment applies: "phase wrap is free."
inline int32_t fm_sine(uint32_t phase) {
    return fm_sine_table[phase >> FM_PHASE_SHIFT];
}

// Q32 phase increment for a given frequency, matching fm_sine()'s bare
// 32-bit accumulator (module_fm.md §5.2: "Q32 phase increment"). Distinct from
// osc/common.h's osc_phase_inc() -- that one targets the shared table's
// 22.10 fixed-point format, which this table doesn't use.
inline uint32_t fm_phase_inc(float freq_hz) {
    return (uint32_t)((freq_hz / (float)SAMPLE_RATE) * 4294967296.0);
}
