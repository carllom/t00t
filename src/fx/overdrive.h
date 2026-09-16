#pragma once

#include "engine_base.h"   // EffectParams
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono overdrive: pre-gain into a cubic soft clipper, then a one-pole
// tone lowpass. No delay buffer, no LFO -- the one-pole state is the only
// thing carried between samples.
struct FxOverdrive {
    int32_t tone_z;  // one-pole lowpass state, Q15

    void init() {
        tone_z = 0;
    }

    // Maps the raw controller values: p1 → drive gain (1x..10x, pushing the
    // signal into the clipper), p2 → tone (post-clip lowpass coefficient,
    // tames the added harmonics), mix → wet level. Clip to Q15 full scale
    // before the cubic so the curve (1.5x - 0.5x^3, exact on [-1,1]) stays
    // monotonic -- driven peaks beyond that just sit at the flat top.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        int32_t drive_q8 = 256 + (int32_t)fx.p1 * 2304 / 127;  // Q8, 1x..10x
        int32_t tone      = 8192 + (int32_t)fx.p2 * 180;       // Q15 lowpass coeff
        int32_t mix       = (int32_t)fx.mix * 258;             // Q15, 0..~32766

        for (uint32_t i = 0; i < n; i++) {
            int32_t send = __ssat(scratch[i], 16);
            int32_t x    = __ssat((int32_t)(((int64_t)send * drive_q8) >> 8), 16);

            int32_t x2 = (int32_t)(((int64_t)x * x) >> 15);
            int32_t x3 = (int32_t)(((int64_t)x2 * x) >> 15);
            int32_t y  = __ssat((3 * x - x3) >> 1, 16);

            tone_z += (int32_t)(((int64_t)(y - tone_z) * tone) >> 15);
            scratch[i] = (int32_t)(((int64_t)mix * tone_z) >> 15);
        }
    }
};
