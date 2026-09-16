#pragma once

#include "engine_base.h"   // EffectParams
#include "fx/lfo.h"
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono phaser: NSTAGE cascaded first-order allpass stages, LFO-swept
// coefficient, no delay buffer at all -- a phaser doesn't need one (unlike
// flanger/chorus below), since it sweeps notch position via the allpass
// coefficient rather than a modulated delay time.
static constexpr int PHASER_NSTAGE = 6;

struct FxPhaser {
    int32_t zm1[PHASER_NSTAGE];  // one-multiplier allpass state per stage, Q15
    int32_t cur_g;               // smoothed allpass coefficient, Q15
    FxLfo   lfo;

    void init() {
        for (int s = 0; s < PHASER_NSTAGE; s++) zm1[s] = 0;
        cur_g = 0;
        lfo.init();
    }

    // Maps the raw controller values: p1 → sweep rate, p2 → sweep depth
    // (how far the allpass coefficient swings), mix → wet level. No
    // feedback-around knob -- fixed out of the budget of two free params,
    // same simplification flanger/chorus below make.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        uint32_t lfo_inc = fx_lfo_inc(fx.p1, 0.1f, 2.0f);
        int32_t  depth   = (int32_t)fx.p2 * 236;  // Q15, ≤ ~0.91 -- same safety
                                                    // headroom fx/delay.h's feedback uses
        int32_t  mix     = (int32_t)fx.mix * 258;  // Q15, 0..~32766

        for (uint32_t i = 0; i < n; i++) {
            int32_t lfo_val  = lfo.step(lfo_inc);  // Q15, -32767..32767
            int32_t target_g = (int32_t)(((int64_t)lfo_val * depth) >> 15);
            // One-pole glide toward the target coefficient, same idiom as
            // fx/delay.h's delay-length glide, so CC72/75 turns don't click.
            cur_g += (target_g - cur_g) >> 5;

            int32_t out = __ssat(scratch[i], 16);
            for (int s = 0; s < PHASER_NSTAGE; s++) {
                // One-multiplier allpass (Kingsbury form): single state
                // register per stage, 2 multiplies, exactly AP1(g) =
                // (g + z⁻¹)/(1 + g·z⁻¹).
                int32_t d = zm1[s];
                int32_t y = (int32_t)(((int64_t)cur_g * out) >> 15) + d;
                zm1[s] = __ssat(out - (int32_t)(((int64_t)cur_g * y) >> 15), 16);
                out = y;
            }
            scratch[i] = (int32_t)(((int64_t)mix * out) >> 15);
        }
    }
};
