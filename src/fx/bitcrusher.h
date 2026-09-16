#pragma once

#include "engine_base.h"   // EffectParams
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono bitcrusher: bit-depth quantization plus sample-and-hold decimation.
// No delay buffer, no LFO -- both stages are stateless aside from the one
// held sample, unlike the modulation effects above.
struct FxBitcrusher {
    int32_t  held;     // last quantized sample, held across the decimation period
    uint32_t counter;  // samples left before the next hold update

    void init() {
        held = 0;
        counter = 0;
    }

    // Maps the raw controller values: p1 → bits dropped (0..14, quantizing
    // 16-bit samples down toward 2-bit), p2 → hold length (1..32 samples,
    // the decimated "sample rate"), mix → wet level.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        uint32_t shift = (uint32_t)fx.p1 * 14u / 127u;
        int32_t  mask  = (int32_t)(~0u << shift);   // upper bits (sign incl.) pass through
        uint32_t hold  = 1u + (uint32_t)fx.p2 * 31u / 127u;
        int32_t  mix   = (int32_t)fx.mix * 258;      // Q15, 0..~32766

        for (uint32_t i = 0; i < n; i++) {
            if (counter == 0) {
                held = __ssat(scratch[i], 16) & mask;
                counter = hold;
            }
            counter--;
            scratch[i] = (int32_t)(((int64_t)mix * held) >> 15);
        }
    }
};
