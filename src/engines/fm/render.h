#pragma once

#include "pan.h"
#include "sine_tab.h"
#include <cstdint>

// Shared core of the FM engine skeleton (#41): renders `frames` samples of a
// fixed test tone through the FM operator sine table (sine_tab.h) --
// phase >> 20, no interpolation -- and pans it into dry_l/dry_r. Both the
// device path (engines/fm/audio_engine.cpp, called from the Core 1 render
// loop) and the host path (tools/host_render/render_fm.cpp) call this exact
// function, so the FM table lookup and phase arithmetic are proven identical
// on both before any operator kernel, patch struct, envelope, or algorithm
// table exists -- fm.md's P0 measurement gate is the very next slice.
//
// No pico-sdk dependency (sine_tab.h and pan.h are both header-only,
// common-layer DSP) -- pulling in engine.h/engine_base.h here instead would
// drag in hardware/sync.h, which the standalone host build has no access to
// (see render_speech.cpp's equivalent comment).
inline void fm_render_test_tone(uint32_t &phase, uint32_t phase_inc, int16_t pan,
                                 int32_t *dry_l, int32_t *dry_r, uint32_t frames) {
    constexpr int16_t TEST_TONE_AMPLITUDE = 16384;  // fixed headroom, no envelope yet
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    for (uint32_t i = 0; i < frames; i++) {
        int32_t sample = (fm_sine(phase) * TEST_TONE_AMPLITUDE) >> 15;
        dry_l[i] = (sample * gain_l) >> 15;
        dry_r[i] = (sample * gain_r) >> 15;
        phase += phase_inc;
    }
}
