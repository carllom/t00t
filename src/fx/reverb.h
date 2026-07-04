#pragma once

#include "engine.h"        // EffectParams
#include <cstdint>
#include <arm_acle.h>      // __ssat

// Mono Freeverb (Schroeder): 8 parallel comb filters into 4 series allpasses.
// Float maths on the RP2350 M33 FPU (the engine already runs float per-sample).
// Comb/allpass tunings are the canonical Freeverb values for 44.1 kHz.
// ~50 KB of float buffers (.bss on Core 1).
struct FxReverb {
    static constexpr int NCOMB = 8;
    static constexpr int NALL  = 4;
    static constexpr int CSZ[NCOMB] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int ASZ[NALL]  = {556, 441, 341, 225};

    float c0[1116], c1[1188], c2[1277], c3[1356], c4[1422], c5[1491], c6[1557], c7[1617];
    float a0[556], a1[441], a2[341], a3[225];
    float *cbuf[NCOMB];
    float *abuf[NALL];
    int    cidx[NCOMB];
    float  cstore[NCOMB];
    int    aidx[NALL];

    void init() {
        cbuf[0]=c0; cbuf[1]=c1; cbuf[2]=c2; cbuf[3]=c3;
        cbuf[4]=c4; cbuf[5]=c5; cbuf[6]=c6; cbuf[7]=c7;
        abuf[0]=a0; abuf[1]=a1; abuf[2]=a2; abuf[3]=a3;
        for (int c = 0; c < NCOMB; c++) {
            cidx[c] = 0; cstore[c] = 0.0f;
            for (int i = 0; i < CSZ[c]; i++) cbuf[c][i] = 0.0f;
        }
        for (int a = 0; a < NALL; a++) {
            aidx[a] = 0;
            for (int i = 0; i < ASZ[a]; i++) abuf[a][i] = 0.0f;
        }
    }

    // Process one buffer in place. Maps controller values: p1 → room size
    // (comb feedback 0.70..0.98), p2 → damping, mix → wet/dry.
    inline void process(int32_t *scratch, uint32_t n, const EffectParams &fx) {
        float wetknob  = fx.mix / 127.0f;
        float dry_gain = 1.0f - wetknob;
        float wet_gain = wetknob * 3.0f;                    // Freeverb scalewet
        float feedback = 0.70f + (fx.p1 / 127.0f) * 0.28f;  // room size
        float damp1    = (fx.p2 / 127.0f) * 0.4f;           // damping
        float damp2    = 1.0f - damp1;
        const float gain = 0.015f;                          // Freeverb fixedgain

        for (uint32_t i = 0; i < n; i++) {
            float dry = (float)__ssat(scratch[i], 16);
            float in  = dry * gain;

            float out = 0.0f;
            for (int c = 0; c < NCOMB; c++) {
                float o = cbuf[c][cidx[c]];
                cstore[c] = o * damp2 + cstore[c] * damp1;      // lowpass in the loop
                cbuf[c][cidx[c]] = in + cstore[c] * feedback;
                if (++cidx[c] >= CSZ[c]) cidx[c] = 0;
                out += o;
            }
            for (int a = 0; a < NALL; a++) {
                float bo = abuf[a][aidx[a]];
                float o  = -out + bo;
                abuf[a][aidx[a]] = out + bo * 0.5f;             // allpass feedback 0.5
                if (++aidx[a] >= ASZ[a]) aidx[a] = 0;
                out = o;
            }
            scratch[i] = (int32_t)(dry * dry_gain + out * wet_gain);
        }
    }
};
