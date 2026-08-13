#pragma once

#include "res2p.h"
#include <cmath>

// Speaker simulation output stage -- sid.md §10's shape (HP cone rolloff ->
// resonant "boxy" peak -> LP -> soft clip), built for the P0 gate's cost
// measurement, NOT the tuned P5 stage. Center frequencies are picked from the
// middle of §10's ranges (200 Hz / 600 Hz / 7 kHz); P5 owns presets
// (Commodore 1702, portable TV, Game Boy, arcade, bypass) and real tuning.
//
// Float, not the chip primitives' fixed-point: this sits downstream of the
// insert (delay/reverb), which are already float (fx/reverb.h's Freeverb),
// so there is no fixed-point contract to match here the way sid_voice.h's
// DAC chain has to match reSID exactly. Divides in here are real hardware
// vdiv.f32 (this target's -mfloat-abi=softfp still has the FPU), not the
// software 64-bit divides sid_filter.h's saturate() had -- no reciprocal
// trick needed.
struct SidSpeakerStage {
    float hp_lp_state = 0.0f;   // HP built as x - lowpass(x), per §10's "one-pole"
    float lp_state = 0.0f;
    float hp_a = 0.0f, lp_a = 0.0f;
    Res2p peak;

    void init(float fs) {
        hp_lp_state = 0.0f;
        lp_state = 0.0f;
        peak = Res2p{};
        hp_a = 1.0f - expf(-2.0f * (float)M_PI * 200.0f / fs);   // HP corner ~200 Hz
        lp_a = 1.0f - expf(-2.0f * (float)M_PI * 7000.0f / fs);  // LP corner ~7 kHz
        res2p_set(peak, 600.0f, 300.0f, fs);                      // boxy peak ~600 Hz
    }

    inline float tick(float x) {
        hp_lp_state += (x - hp_lp_state) * hp_a;
        float hp_out = x - hp_lp_state;

        float peaked = res2p_tick(peak, hp_out);

        lp_state += (peaked - lp_state) * lp_a;
        float y = lp_state;

        // Soft clip / cone breakup: same cubic shape as sid_filter_saturate,
        // scaled to this stage's float working range instead of the chip
        // primitives' int32 one.
        constexpr float lim = 24000.0f;
        if (y >= lim)  return  lim * (2.0f / 3.0f);
        if (y <= -lim) return -lim * (2.0f / 3.0f);
        float yn = y / lim;
        return y - (y * yn * yn) / 3.0f;
    }
};
