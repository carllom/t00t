#pragma once

#include <cassert>
#include <cmath>

// Two-pole resonator (one complex-conjugate pole pair). Common layer: shared
// by the groovebox (808 toms/congas/cowbell, backport pending) and the
// speech module's formant cascade. See module_speech.md "Resonator and the stability
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

// Pole radius from bandwidth: exp(-pi*bw/fs). LUT + linear interpolation
// over x = pi*bw/fs instead of calling expf() in the audio path (#28): for
// every (bw, fs) pair this codebase actually uses -- speech formants at
// 50-400 Hz @ SPEECH_RATE (22.05 kHz), groovebox resonators at 20-400 Hz @
// 44.1 kHz -- x stays inside [0, RES2P_RADIUS_X_MAX], comfortably below it
// with margin. res2p_init() must be called once at startup before any
// res2p_radius()/res2p_set() call; tools/host_render/render_res2p.cpp
// checks the LUT against expf() across that whole range.
inline constexpr int   RES2P_RADIUS_LUT_SIZE = 33;
inline constexpr float RES2P_RADIUS_X_MAX = 0.08f;
inline float res2p_radius_lut[RES2P_RADIUS_LUT_SIZE];

inline void res2p_init() {
    for (int i = 0; i < RES2P_RADIUS_LUT_SIZE; i++) {
        float x = RES2P_RADIUS_X_MAX * (float)i / (float)(RES2P_RADIUS_LUT_SIZE - 1);
        res2p_radius_lut[i] = expf(-x);
    }
}

// bw and fs in Hz. Out-of-range (x > RES2P_RADIUS_X_MAX) clamps to the LUT's
// last entry rather than extrapolating -- still < 1.0f (stable), just not
// validated against expf() past that point.
inline float res2p_radius(float bw, float fs) {
    float x = (float)M_PI * bw / fs;
    float pos = x * (float)(RES2P_RADIUS_LUT_SIZE - 1) / RES2P_RADIUS_X_MAX;
    if (pos < 0.0f) pos = 0.0f;
    if (pos >= (float)(RES2P_RADIUS_LUT_SIZE - 1)) return res2p_radius_lut[RES2P_RADIUS_LUT_SIZE - 1];
    int i0 = (int)pos;
    float frac = pos - (float)i0;
    return res2p_radius_lut[i0] + (res2p_radius_lut[i0 + 1] - res2p_radius_lut[i0]) * frac;
}

// f = resonant frequency (Hz), bw = -3dB bandwidth (Hz), fs = sample rate (Hz).
// Unity DC gain (b0 = 1 + a1 + a2). Debug builds assert the pole stays
// strictly inside the unit circle (module_speech.md "Testing": "catches the
// interpolation-instability class immediately") -- this only guards against
// bw/theta values that push the pole out, not against interpolating a1/a2/b0
// directly, which no code path in this repo does.
inline void res2p_set(Res2p &r, float f, float bw, float fs) {
    float rr    = res2p_radius(bw, fs);
    float theta = 2.0f * (float)M_PI * f / fs;
    r.a1 = -2.0f * rr * cosf(theta);
    r.a2 = rr * rr;
    r.b0 = 1.0f + r.a1 + r.a2;
    assert(r.a2 < 1.0f);
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
