#pragma once

#include "engine_base.h"   // EffectParams
#include "fx/lfo.h"
#include "fx/allpass_interp.h"
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono chorus: NVOICE independently-LFO-modulated interpolated taps on one
// shared delay line (not N separate lines -- memory cost stays fixed while
// compute scales with voice count), summed to a mono wet signal. Same
// fractional-delay interpolation as fx/flanger.h, retuned longer (10-25ms
// vs flanger's 0-15ms) and with no feedback tap.
static constexpr uint32_t CHORUS_LEN  = 2048;
static constexpr uint32_t CHORUS_MASK = CHORUS_LEN - 1;
static constexpr int      CHORUS_NVOICE = 3;
static constexpr uint32_t CHORUS_BASE_Q8  = 441u << 8;  // ~10 ms floor, Q8 samples
static constexpr uint32_t CHORUS_SWEEP_Q8 = 660u << 8;  // up to ~15 ms more, Q8 samples

struct FxChorus {
    int16_t  buf[CHORUS_LEN];
    uint32_t w;
    uint32_t cur_delay_q8[CHORUS_NVOICE];
    int32_t  interp_z[CHORUS_NVOICE];
    FxLfo    lfo[CHORUS_NVOICE];

    void init() {
        for (uint32_t i = 0; i < CHORUS_LEN; i++) buf[i] = 0;
        w = 0;
        for (int v = 0; v < CHORUS_NVOICE; v++) {
            cur_delay_q8[v] = CHORUS_BASE_Q8;
            interp_z[v] = 0;
            // Stagger each voice's LFO phase by 1/NVOICE of a cycle so the
            // taps decorrelate instead of sweeping in lockstep.
            lfo[v].init();
            lfo[v].phase = (uint32_t)((uint64_t)v * 0x100000000ULL / CHORUS_NVOICE);
        }
    }

    // Maps the raw controller values: p1 → sweep rate, p2 → sweep depth,
    // mix → wet level, same shape as fx/flanger.h. Wet gain is split across
    // voices so full mix isn't NVOICE times louder than the other effects'.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        uint32_t lfo_inc = fx_lfo_inc(fx.p1, 0.2f, 3.0f);
        uint32_t sweep   = (uint32_t)fx.p2 * CHORUS_SWEEP_Q8 / 127u;
        int32_t  mix     = (int32_t)fx.mix * 258 / CHORUS_NVOICE;  // Q15

        for (uint32_t i = 0; i < n; i++) {
            int32_t send = __ssat(scratch[i], 16);
            buf[w] = (int16_t)send;

            int32_t sum = 0;
            for (int v = 0; v < CHORUS_NVOICE; v++) {
                int32_t  lfo_bi  = lfo[v].step(lfo_inc);
                uint32_t lfo_uni = (uint32_t)(lfo_bi + 32767) >> 1;
                uint32_t target_q8 = CHORUS_BASE_Q8 +
                                      (uint32_t)(((uint64_t)sweep * lfo_uni) >> 15);
                cur_delay_q8[v] = (uint32_t)((int32_t)cur_delay_q8[v] +
                                  (((int32_t)target_q8 - (int32_t)cur_delay_q8[v]) >> 4));

                uint32_t int_delay = cur_delay_q8[v] >> 8;
                int32_t  eta       = allpass_eta_q15(cur_delay_q8[v] & 0xFF);

                uint32_t r0 = (w - int_delay) & CHORUS_MASK;
                uint32_t r1 = (r0 - 1) & CHORUS_MASK;
                int32_t  x0 = buf[r0];
                int32_t  x1 = buf[r1];

                int32_t y = (int32_t)(((int64_t)eta * x0) >> 15) + x1 -
                            (int32_t)(((int64_t)eta * interp_z[v]) >> 15);
                interp_z[v] = y;
                sum += y;
            }

            w = (w + 1) & CHORUS_MASK;
            scratch[i] = (int32_t)(((int64_t)mix * sum) >> 15);
        }
    }
};
