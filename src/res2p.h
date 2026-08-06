#pragma once

#include <cmath>

// Two-pole resonator (one complex-conjugate pole pair). Common layer: shared
// by the groovebox (808 toms/congas/cowbell, backport pending) and the
// speech module's formant cascade. See speech.md "Resonator and the stability
// rule".
//
// Stability rule for callers: ramp f/bw and call res2p_set() to recompute
// coefficients per sub-block — never interpolate a1/a2/b0 directly. Walking
// between two coefficient sets can push a pole outside the unit circle and
// produce a burst of noise. Verified on host in tools/host_render
// (render_res2p.cpp) before this gets wired into any real-time engine.
struct Res2p {
    float a1 = 0.0f, a2 = 0.0f, b0 = 1.0f;
    float s1 = 0.0f, s2 = 0.0f;
};

// Pole radius from bandwidth: exp(-pi*bw/fs). bw and fs in Hz.
inline float res2p_radius(float bw, float fs) {
    return expf(-(float)M_PI * bw / fs);
}

// f = resonant frequency (Hz), bw = -3dB bandwidth (Hz), fs = sample rate (Hz).
// Unity DC gain (b0 = 1 + a1 + a2).
inline void res2p_set(Res2p &r, float f, float bw, float fs) {
    float rr    = res2p_radius(bw, fs);
    float theta = 2.0f * (float)M_PI * f / fs;
    r.a1 = -2.0f * rr * cosf(theta);
    r.a2 = rr * rr;
    r.b0 = 1.0f + r.a1 + r.a2;
}

inline float res2p_tick(Res2p &r, float x) {
    float y = r.b0 * x - r.a1 * r.s1 - r.a2 * r.s2;
    r.s2 = r.s1;
    r.s1 = y;
    return y;
}

inline void res2p_reset(Res2p &r) {
    r.s1 = 0.0f;
    r.s2 = 0.0f;
}
