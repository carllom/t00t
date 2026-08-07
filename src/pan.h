#pragma once

#include "osc/common.h"   // PHASE_CYCLE
#include "osc/sine.h"     // osc_sine
#include <cstdint>

// Equal-power stereo pan law, reusing the existing sine wavetable as a
// quarter-cycle quadrature source (no extra table). `pan` is Q15, signed:
// -32768 = full left, 0 = center, 32767 = full right. At center both gains
// are ~23170 (0.707 full scale) so L^2+R^2 stays ~constant across the sweep,
// unlike a linear law that dips in the middle.
inline void pan_gains_q15(int16_t pan, int32_t &gain_l, int32_t &gain_r) {
    uint32_t upan = (uint32_t)pan + 32768u;       // 0..65535
    uint32_t phase = upan << 2;                   // 0..PHASE_CYCLE/4 (quarter turn)
    gain_r = osc_sine(phase);
    gain_l = osc_sine((PHASE_CYCLE / 4) - phase);
}
