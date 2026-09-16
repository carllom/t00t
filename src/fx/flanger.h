#pragma once

#include "engine_base.h"   // EffectParams
#include "fx/lfo.h"
#include "fx/allpass_interp.h"
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono flanger: short LFO-modulated feedback delay line with first-order
// allpass fractional-delay interpolation. 1024 samples @ 44.1 kHz covers the
// whole 0-15ms sweep range with headroom -- three orders of magnitude
// smaller than fx/delay.h's 65536-sample line, since flanger has no use for
// echo-length delays.
static constexpr uint32_t FLANGER_LEN  = 1024;
static constexpr uint32_t FLANGER_MASK = FLANGER_LEN - 1;
static constexpr uint32_t FLANGER_MAX_SWEEP_Q8 = 700u << 8;  // ~15.9 ms, Q8 samples

struct FxFlanger {
    int16_t  buf[FLANGER_LEN];
    uint32_t w;
    uint32_t cur_delay_q8;  // smoothed delay length, Q8 fixed-point samples
    int32_t  interp_z;      // allpass interpolator state
    FxLfo    lfo;

    void init() {
        for (uint32_t i = 0; i < FLANGER_LEN; i++) buf[i] = 0;
        w = 0;
        cur_delay_q8 = 1 << 8;
        interp_z = 0;
        lfo.init();
    }

    // Maps the raw controller values: p1 → sweep rate, p2 → sweep depth,
    // mix → wet level. Feedback is a fixed modest constant, not a knob --
    // rate/depth is the classic two-knob flanger control set, and there's
    // no CC left in the {p1, p2, mix} budget for a fourth.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        uint32_t lfo_inc = fx_lfo_inc(fx.p1, 0.05f, 2.0f);
        uint32_t sweep   = (uint32_t)fx.p2 * FLANGER_MAX_SWEEP_Q8 / 127u;
        int32_t  fb      = 8192;                   // Q15 ≈ 0.25, fixed
        int32_t  mix     = (int32_t)fx.mix * 258;   // Q15, 0..~32766

        for (uint32_t i = 0; i < n; i++) {
            int32_t  lfo_bi  = lfo.step(lfo_inc);                     // -32767..32767
            uint32_t lfo_uni = (uint32_t)(lfo_bi + 32767) >> 1;       // 0..32767
            uint32_t target_q8 = (1u << 8) +
                                  (uint32_t)(((uint64_t)sweep * lfo_uni) >> 15);
            // One-pole glide toward the target length, same idiom as
            // fx/delay.h -- here it smooths the LFO's own step size too,
            // on top of any CC72/75 change.
            cur_delay_q8 = (uint32_t)((int32_t)cur_delay_q8 +
                           (((int32_t)target_q8 - (int32_t)cur_delay_q8) >> 4));

            uint32_t int_delay = cur_delay_q8 >> 8;
            int32_t  eta       = allpass_eta_q15(cur_delay_q8 & 0xFF);  // Q15

            uint32_t r0 = (w - int_delay) & FLANGER_MASK;
            uint32_t r1 = (r0 - 1) & FLANGER_MASK;
            int32_t  x0 = buf[r0];
            int32_t  x1 = buf[r1];

            // First-order allpass fractional-delay interpolation: no gain
            // error, unlike linear interpolation at the same cost -- matters
            // here because the delay line also carries a feedback tap.
            int32_t y = (int32_t)(((int64_t)eta * x0) >> 15) + x1 -
                        (int32_t)(((int64_t)eta * interp_z) >> 15);
            interp_z = y;

            int32_t send = __ssat(scratch[i], 16);
            buf[w] = (int16_t)__ssat(send + (int32_t)(((int64_t)fb * y) >> 15), 16);
            w = (w + 1) & FLANGER_MASK;

            scratch[i] = (int32_t)(((int64_t)mix * y) >> 15);
        }
    }
};
