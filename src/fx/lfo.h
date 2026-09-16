#pragma once

#include "engine_base.h"   // SAMPLE_RATE (via audio_common.h)
#include <cstdint>

// Shared modulation LFO for the phaser/flanger/chorus post-mix effects.
// None of the per-voice LFOs elsewhere in the codebase (e.g. engines/fm/lfo.h)
// are usable here -- this is engine-agnostic global-FX state, not voice
// state. Triangle wave, not sine: a table-free phase-to-triangle mapping
// avoids a sine LUT entirely and is indistinguishable from sine at the slow,
// subtle rates these effects sweep at.
struct FxLfo {
    uint32_t phase;  // Q32 phase, one full cycle = 2^32

    void init() { phase = 0; }

    // Advance by one sample and return a Q15-range triangle sample
    // (-32767..32767). `inc` is the per-sample Q32 phase step for the
    // desired rate -- computed once per buffer by fx_lfo_inc(), not here,
    // since it only needs to track a knob value, not the signal.
    inline int32_t step(uint32_t inc) {
        phase += inc;
        uint32_t quadrant = phase >> 30;
        int32_t  frac15   = (int32_t)((phase & 0x3FFFFFFFu) >> 15);
        switch (quadrant) {
            case 0:  return frac15;
            case 1:  return 32767 - frac15;
            case 2:  return -frac15;
            default: return frac15 - 32767;
        }
    }
};

// Maps a 0..127 controller value to a Q32 phase increment across [min_hz,
// max_hz]. Float math, but only once per buffer (mirrors fx/reverb.h's own
// per-buffer float parameter mapping), not per sample.
inline uint32_t fx_lfo_inc(uint8_t cc, float min_hz, float max_hz) {
    float hz = min_hz + (max_hz - min_hz) * ((float)cc / 127.0f);
    return (uint32_t)(hz * (4294967296.0f / (float)SAMPLE_RATE));
}
