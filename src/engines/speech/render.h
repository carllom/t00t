#pragma once

#include "osc/sine.h"
#include "pan.h"
#include <cstdint>

// Shared core of the speech engine skeleton (#27): renders `native_frames`
// samples of a fixed test tone at the engine's native rate and zero-order-
// holds each one x2 into the (output-rate-sized) dry_l/dry_r buffers, i.e.
// dry_{l,r}[2*i] == dry_{l,r}[2*i+1] for every native sample i. Both the
// device path (engines/speech/audio_engine.cpp, called from the Core 1
// render loop) and the host path (tools/host_render/render_speech.cpp) call
// this exact function, so the ZOH seam and the frame-count arithmetic around
// it are proven identical on both before any formant DSP exists.
//
// Deliberately has no pico-sdk dependency (osc/sine.h and pan.h are both
// header-only, common-layer DSP) -- pulling in engine.h/engine_base.h here
// instead would drag in hardware/sync.h, which the standalone host build
// has no access to (see render_tracker_mixer.cpp's equivalent comment).
inline void speech_render_test_tone(uint32_t &phase, uint32_t phase_inc, int16_t pan,
                                     int32_t *dry_l, int32_t *dry_r, uint32_t native_frames) {
    constexpr int16_t TEST_TONE_AMPLITUDE = 16384;  // fixed headroom, no envelope yet
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    for (uint32_t i = 0; i < native_frames; i++) {
        int32_t sample = (osc_sine(phase) * TEST_TONE_AMPLITUDE) >> 15;
        int32_t l = (sample * gain_l) >> 15;
        int32_t r = (sample * gain_r) >> 15;
        uint32_t o = i * 2;
        dry_l[o] = l; dry_l[o + 1] = l;
        dry_r[o] = r; dry_r[o + 1] = r;
        phase += phase_inc;
    }
}
