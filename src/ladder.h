#pragma once

#include "audio_common.h"

// 4-pole (24 dB/oct) resonant low-pass — Moog-ladder model (Stilson/Smith
// variant, musicdsp.org #26). Float state, single-precision FPU friendly.
//
// Used by the groovebox TB-303 voice for the authentic acid squelch that the
// 2-pole SVF (filter.h) can't produce; self-oscillates near full resonance.
// Operates on normalized samples in ~[-1, 1] — the caller scales Q15 <-> float.
//
// Coefficients are recomputed per sample (cheap multiplies) so the filter
// envelope can sweep the cutoff continuously. The one costly term in the
// original algorithm — a per-sample expf() for resonance compensation — is
// replaced by a cubic Taylor approximation (<6% error over the used range).
struct LadderFilter {
    float y1, y2, y3, y4;
    float oldx, oldy1, oldy2, oldy3;

    void init() {
        y1 = y2 = y3 = y4 = 0.0f;
        oldx = oldy1 = oldy2 = oldy3 = 0.0f;
    }

    // cutoff_hz: 20..~18000 ; resonance: 0..1 (→ self-oscillation near 1).
    // input/return: normalized float, roughly [-1, 1].
    inline float tick(float input, float cutoff_hz, float resonance) {
        if (resonance > 0.95f) resonance = 0.95f;   // headroom against divergence
        float f = 2.0f * cutoff_hz * (1.0f / (float)SAMPLE_RATE);  // [0,1)
        float k = 3.6f * f - 1.6f * f * f - 1.0f;                  // empirical tuning
        float p = (k + 1.0f) * 0.5f;

        // scale = e^((1-p)*1.386249): keeps resonance perceptually constant as
        // the cutoff moves. Cubic Taylor of e^x avoids a per-sample expf().
        float xr = (1.0f - p) * 1.386249f;
        float scale = 1.0f + xr + 0.5f * xr * xr + (1.0f / 6.0f) * xr * xr * xr;
        float r = resonance * scale;

        float x = input - r * y4;   // resonance feedback

        // four cascaded one-pole sections (bilinear-transform poles)
        y1 = x  * p + oldx  * p - k * y1;
        y2 = y1 * p + oldy1 * p - k * y2;
        y3 = y2 * p + oldy2 * p - k * y3;
        y4 = y3 * p + oldy3 * p - k * y4;

        // band-limited cubic soft-clip tames the resonance peak / self-osc
        y4 -= (1.0f / 6.0f) * y4 * y4 * y4;
        // Safety net: the cubic clip folds (not limits) for |y4|>~1.7, so a hard
        // clamp on the fed-back state guarantees the loop can't run away.
        if (y4 > 1.5f) y4 = 1.5f; else if (y4 < -1.5f) y4 = -1.5f;

        oldx = x; oldy1 = y1; oldy2 = y2; oldy3 = y3;
        return y4;
    }
};
