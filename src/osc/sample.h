#pragma once

#include "sample_def.h"
#include "common.h"
#include "../audio_common.h"
#include <cstdint>

// Compute phase_inc for sample playback at a target frequency.
// Scales native rate by (target_freq / base_freq) ratio.
// phase_inc = (sample_rate * target_freq / base_freq) << PHASE_FRAC_BITS / SAMPLE_RATE
inline uint32_t osc_sample_phase_inc(const SampleDef *s, float target_freq) {
    float rate = (float)s->sample_rate * (target_freq / s->base_freq);
    return (uint32_t)((rate * (float)(1 << PHASE_FRAC_BITS)) / (float)SAMPLE_RATE);
}

// Read one sample from a SampleDef with linear interpolation.
// Phase is in the same 22.10 fixed-point format as other oscillators.
// Returns 0 if phase is past the end of a non-looped sample.
// Data is signed int8_t, shifted left 8 to Q15 for pipeline.
inline int32_t osc_sample_play(const SampleDef *s, uint32_t phase) {
    uint32_t idx = phase >> PHASE_FRAC_BITS;
    if (idx >= s->num_samples) return 0;

    uint32_t frac = phase & ((1u << PHASE_FRAC_BITS) - 1);
    int32_t s0 = (int32_t)s->data[idx] << 8;
    int32_t s1 = (idx + 1 < s->num_samples) ? ((int32_t)s->data[idx + 1] << 8) : s0;
    return s0 + (((s1 - s0) * (int32_t)frac) >> PHASE_FRAC_BITS);
}

// Handle phase advance for sample playback.
// Call after phase += phase_inc each sample.
// Wraps phase at loop boundaries or clamps past end.
// Returns false if a non-looped sample has finished.
inline bool osc_sample_advance_phase(const SampleDef *s, uint32_t &phase) {
    if (s->looped) {
        uint32_t loop_end_ph = s->loop_end << PHASE_FRAC_BITS;
        if (phase >= loop_end_ph) {
            uint32_t loop_len_ph = loop_end_ph - (s->loop_start << PHASE_FRAC_BITS);
            phase -= loop_len_ph;
            // Safety for extreme phase_inc (multiple wraps)
            while (phase >= loop_end_ph) phase -= loop_len_ph;
        }
        return true;
    } else {
        return phase < (s->num_samples << PHASE_FRAC_BITS);
    }
}
