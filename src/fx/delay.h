#pragma once

#include "engine_base.h"   // EffectParams
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono feedback delay (echo). Power-of-2 buffer so the wrap is a mask.
// 65536 samples @ 44.1 kHz ≈ 1.49 s, 128 KB of int16 (Core 1 .bss).
static constexpr uint32_t DELAY_LEN  = 65536;
static constexpr uint32_t DELAY_MASK = DELAY_LEN - 1;

struct FxDelay {
    int16_t  buf[DELAY_LEN];
    uint32_t w;          // write index
    uint32_t cur_delay;  // smoothed delay length (samples)

    void init() {
        for (uint32_t i = 0; i < DELAY_LEN; i++) buf[i] = 0;
        w = 0;
        cur_delay = 1;
    }

    // Mono send / stereo return: `scratch` enters as the mono downmix of the
    // stereo dry mix and is fed into the (still-mono) delay line for its
    // feedback dynamics; on return it holds the wet contribution only
    // (scaled by mix, no dry component), which the caller adds identically
    // to both output channels. Fixed cost per sample, independent of voice
    // count.
    //
    // Maps the raw controller values: p2 → 20..1000 ms delay time,
    // p1 → feedback (max ≈ 0.91), mix → wet level.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        uint32_t ms = 20 + (uint32_t)fx.p2 * 980u / 127u;
        uint32_t target = ms * SAMPLE_RATE / 1000u;
        if (target < 1) target = 1;
        if (target > DELAY_LEN - 1) target = DELAY_LEN - 1;
        int32_t fb  = (int32_t)fx.p1 * 236;   // Q15, ≤ ~0.91
        int32_t mix = (int32_t)fx.mix * 258;  // Q15, 0..~32766

        for (uint32_t i = 0; i < n; i++) {
            // One-pole glide toward the target length so CC turns don't click.
            cur_delay = (uint32_t)((int32_t)cur_delay +
                        (((int32_t)target - (int32_t)cur_delay) >> 6));
            if (cur_delay < 1) cur_delay = 1;

            // Clip the (unclipped) mono send to int16 before the effect so the
            // wet-mix multiply below can't overflow.
            int32_t send = __ssat(scratch[i], 16);
            uint32_t r   = (w - cur_delay) & DELAY_MASK;
            int32_t d    = buf[r];                                              // delayed sample
            buf[w] = (int16_t)__ssat(send + (int32_t)(((int64_t)fb * d) >> 15), 16);  // + feedback
            w = (w + 1) & DELAY_MASK;
            scratch[i] = (int32_t)(((int64_t)mix * d) >> 15);  // wet only, dry stays in the L/R path
        }
    }
};
