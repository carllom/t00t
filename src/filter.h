#pragma once

#include "engine.h"
#include "audio_common.h"
#include <cstdint>

// State-Variable Filter (SVF), SID-style multimode — fixed-point.
// Two state variables (lowpass, bandpass) produce LP/BP/HP/notch outputs.
// Uses 2-pass integration for stability at high cutoff + low Q.
// RP2350 (Cortex-M33): SMULL gives 64-bit intermediates, no clamping needed.

struct SVFilter {
    int32_t lp;   // lowpass state
    int32_t bp;   // bandpass state

    void init() {
        lp = 0;
        bp = 0;
    }

    // Process one sample (2-pass integration).
    // F_half: Q15 half-frequency coefficient (from svf_compute_f_half)
    // Q_q15: Q15 damping coefficient (from svf_compute_q)
    inline int32_t tick(int32_t input, int16_t F_half, int32_t Q_q15, FilterMode mode) {
        int32_t hp;
        for (int pass = 0; pass < 2; pass++) {
            hp = input - lp - (int32_t)(((int64_t)Q_q15 * bp) >> 15);
            bp += (int32_t)(((int64_t)F_half * hp) >> 15);
            lp += (int32_t)(((int64_t)F_half * bp) >> 15);
        }

        switch (mode) {
            case FILTER_LP:    return lp;
            case FILTER_BP:    return bp;
            case FILTER_HP:    return hp;
            case FILTER_NOTCH: return lp + hp;
            default:           return input;
        }
    }
};

// Convert cutoff Hz (20–18000) to half-frequency coefficient in Q15.
// F/2 = pi * cutoff / sample_rate. Integer: cutoff * 76539 >> 15.
// 76539/32768 ≈ 2.336 ≈ pi * 32768 / 44100 (0.08% error).
inline int16_t svf_compute_f_half(int32_t cutoff_hz) {
    int32_t f = (cutoff_hz * 76539) >> 15;
    if (f < 33) f = 33;       // ~20 Hz minimum
    if (f > 15564) f = 15564; // F=0.95 maximum (0.475 * 32768)
    return (int16_t)f;
}

// Convert resonance (0–32767) to Q15 damping coefficient.
// 0 → Q=2.0 (65534), 32767 → Q≈0 (2, near self-oscillation).
inline int32_t svf_compute_q(uint16_t resonance) {
    int32_t q = 65534 - (int32_t)resonance * 2;
    if (q < 2) q = 2;
    return q;
}
