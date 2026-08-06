#pragma once

#include <cstdint>

// Stripped 32-voice sample mixer (#15, tracker.md "Rendering Pipeline" /
// "Voice mixer"). Pure integer math, no pico-sdk, no ARM intrinsics — this
// header is included by both the on-device engine (audio_engine.cpp) and
// tools/host_render/render_tracker_mixer.cpp, which renders it to WAV for
// objective verification off-device (tracker.md "Testing": "against a host
// build of the same mixer"). Device-only concerns (profiling pin, DMA
// buffer handoff, __not_in_flash_func placement) stay out of this file.
//
// No voice_alloc, no envelopes, no LFO, no per-sample dispatch: a tracker
// voice is interpolate + scale + accumulate, nothing else (tracker.md
// "Performance Budget"). Pattern data, effects, and the ordered TickBlock
// ring land with the real player (build order steps 3-5); the tick cut here
// is a stub so sub-block-vs-tick-boundary slicing can be proven now.

// Sub-block size: a unit of parameter constancy, not of output. See
// tracker.md "Sub-block size" for the cost/resolution trade that fixed this
// at 64.
static constexpr uint32_t TRACKER_SUBBLOCK = 64;

// Position fixed-point format: Q18.14. Distinct from, and NOT compatible
// with, the existing engines' Q22.10 wavetable phase format (osc/common.h)
// -- that format is ~8.9 cents out of tune for sample playback at a typical
// tracker increment. See tracker.md "Fixed-Point Formats".
static constexpr uint32_t TRACKER_POS_FRAC_BITS = 14;

// Increment arrives as Q8.24 (tracker.md: "stored Q8.24 in the TickBlock").
static constexpr uint32_t TRACKER_INC_FRAC_BITS = 24;

// One resident (SRAM) 8-bit sample. `data` must carry one guard sample
// appended past index [num_samples - 1] -- loop-start value if looped, last
// sample value if one-shot -- so idx+1 is always safe to read with no bounds
// check (tracker.md: "s[idx + 1] reads one past the end at the boundary").
// Never points into XIP: 32 voices at scattered, non-integer strides would
// thrash the 8 KB XIP cache and evict Core 0's code with it.
struct TrackerSample {
    const int8_t *data;
    uint32_t num_samples;
    uint32_t loop_start;  // sample index
    uint32_t loop_end;    // sample index, exclusive; == num_samples for a loop reaching the physical end
    bool looped;
};

struct TrackerVoice {
    bool active;
    const TrackerSample *sample;
    uint32_t pos;                 // Q18.14
    uint32_t inc;                 // Q18.14 -- pre-shifted from Q8.24 once at latch time
    int32_t cur_volL, cur_volR;   // Q15, ramped per-sample toward target
    int32_t tgt_volL, tgt_volR;   // Q15
};

// Pre-shifts a Q8.24 increment to Q18.14 once, at (re)trigger time, so the
// per-sample path never touches it. "the shift is free" (tracker.md).
inline uint32_t tracker_latch_inc(uint32_t inc_q8_24) {
    return inc_q8_24 >> (TRACKER_INC_FRAC_BITS - TRACKER_POS_FRAC_BITS);
}

// Hoists the loop-wrap / end-of-sample test out of the per-sample path: the
// largest run playable from `pos` (Q18.14) before it reaches `end_pos`
// (Q18.14), so the inner loop needs no compare+branch. Precondition: inc > 0
// (a silent voice is `active = false`, never inc == 0).
inline uint32_t samples_to_loop_end(uint32_t pos, uint32_t end_pos, uint32_t inc) {
    if (pos >= end_pos) return 0;
    uint32_t remaining = end_pos - pos;
    return (remaining + inc - 1) / inc;
}

// Forward loops only (ping-pong is tracker.md open question 5, deferred).
// One-shot voices go inactive at their end instead of wrapping.
inline void wrap_loop(TrackerVoice *v) {
    if (v->sample->looped) {
        uint32_t loop_start_pos = v->sample->loop_start << TRACKER_POS_FRAC_BITS;
        uint32_t loop_end_pos   = v->sample->loop_end   << TRACKER_POS_FRAC_BITS;
        uint32_t loop_len = loop_end_pos - loop_start_pos;
        while (v->pos >= loop_end_pos) v->pos -= loop_len;
    } else {
        v->active = false;
    }
}

// Renders up to `n` frames (n <= TRACKER_SUBBLOCK) of one voice into the
// stereo int32_t accumulator (interleaved L/R, added to whatever is already
// there). Volume ramps linearly from current to target across the call;
// increment is held constant -- both per tracker.md's ramping rules (a
// stepped amplitude is a click, a stepped frequency is not). Caller is
// responsible for cutting at sub-block and tick boundaries.
inline void mix_voice(TrackerVoice *v, int32_t *acc, uint32_t n) {
    if (!v->active || n == 0) return;

    int32_t volL = v->cur_volL, volR = v->cur_volR;
    int32_t dL = (v->tgt_volL - volL) / (int32_t)n;
    int32_t dR = (v->tgt_volR - volR) / (int32_t)n;

    uint32_t pos = v->pos;
    const uint32_t inc = v->inc;
    const int8_t *s = v->sample->data;
    const uint32_t end_pos = (v->sample->looped ? v->sample->loop_end : v->sample->num_samples)
                              << TRACKER_POS_FRAC_BITS;

    while (n > 0) {
        uint32_t run = samples_to_loop_end(pos, end_pos, inc);
        if (run > n) run = n;

        for (uint32_t i = 0; i < run; i++) {
            uint32_t idx = pos >> TRACKER_POS_FRAC_BITS;
            int32_t frac = (int32_t)(pos & ((1u << TRACKER_POS_FRAC_BITS) - 1));
            int32_t a = (int32_t)s[idx] << 8;
            int32_t b = (int32_t)s[idx + 1] << 8;
            int32_t smp = a + (((b - a) * frac) >> TRACKER_POS_FRAC_BITS);

            acc[0] += (smp * volL) >> 15;
            acc[1] += (smp * volR) >> 15;
            acc += 2;

            pos += inc;
            volL += dL;
            volR += dR;
        }

        n -= run;
        if (n > 0) {
            // wrap_loop() reads/writes v->pos, not the local `pos` this loop
            // has been advancing -- write it back first, or a wrap partway
            // through a multi-sample call (n > 1 requested past the loop
            // end, e.g. any call from tracker_render_buffer() with more than
            // one sub-block's worth of frames) discards every step taken in
            // this call and wraps from a stale position instead (#17: caught
            // by the openmpt123 reference-diff harness, not by any test that
            // only ever calls with n == 1 or checks aggregate stats).
            v->pos = pos;
            wrap_loop(v);
            pos = v->pos;
            if (!v->active) break;
        }
    }

    v->pos = pos;
    v->cur_volL = volL;
    v->cur_volR = volR;
}

// Nearest-neighbour sibling of mix_voice() (#16, tracker.md open question 2):
// same loop-wrap/end-of-sample structure, but the inner sample fetch skips
// the interpolation lerp entirely. Exists to measure whether the saving is
// worth a build flag -- see engine.md's tracker interpolation measurement.
inline void mix_voice_nearest(TrackerVoice *v, int32_t *acc, uint32_t n) {
    if (!v->active || n == 0) return;

    int32_t volL = v->cur_volL, volR = v->cur_volR;
    int32_t dL = (v->tgt_volL - volL) / (int32_t)n;
    int32_t dR = (v->tgt_volR - volR) / (int32_t)n;

    uint32_t pos = v->pos;
    const uint32_t inc = v->inc;
    const int8_t *s = v->sample->data;
    const uint32_t end_pos = (v->sample->looped ? v->sample->loop_end : v->sample->num_samples)
                              << TRACKER_POS_FRAC_BITS;

    while (n > 0) {
        uint32_t run = samples_to_loop_end(pos, end_pos, inc);
        if (run > n) run = n;

        for (uint32_t i = 0; i < run; i++) {
            uint32_t idx = pos >> TRACKER_POS_FRAC_BITS;
            int32_t smp = (int32_t)s[idx] << 8;

            acc[0] += (smp * volL) >> 15;
            acc[1] += (smp * volR) >> 15;
            acc += 2;

            pos += inc;
            volL += dL;
            volR += dR;
        }

        n -= run;
        if (n > 0) {
            // wrap_loop() reads/writes v->pos, not the local `pos` this loop
            // has been advancing -- write it back first, or a wrap partway
            // through a multi-sample call (n > 1 requested past the loop
            // end, e.g. any call from tracker_render_buffer() with more than
            // one sub-block's worth of frames) discards every step taken in
            // this call and wraps from a stale position instead (#17: caught
            // by the openmpt123 reference-diff harness, not by any test that
            // only ever calls with n == 1 or checks aggregate stats).
            v->pos = pos;
            wrap_loop(v);
            pos = v->pos;
            if (!v->active) break;
        }
    }

    v->pos = pos;
    v->cur_volL = volL;
    v->cur_volR = volR;
}

// Stub tick timer: no player/TickBlock ring yet (those land in build order
// steps 3-5), but sub-blocks must still be cut short at tick boundaries so
// that mechanism is proven now rather than retrofitted later.
struct TrackerTickState {
    uint32_t remaining;
    uint32_t samples_per_tick;
};

// Saturating int32 -> int16. Device builds get the literal ARM __ssat
// instruction (tracker.md acceptance: "clipped with __ssat on store");
// __arm__ is unset when this header is compiled for tools/host_render, which
// gets a plain branch clamp with identical saturation semantics.
#if defined(__arm__)
#include <arm_acle.h>
inline int16_t tracker_clip16(int32_t x) { return (int16_t)__ssat(x, 16); }
#else
inline int16_t tracker_clip16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}
#endif

// Placement of the hot render path in RAM matters here specifically because
// of the access pattern: 32 voices reading sample data at scattered,
// non-integer strides would thrash the 8 KB XIP cache (and evict Core 0's
// code with it) if this code -- or the data it reads -- lived in flash. The
// real macro (pico/platform.h) wins when included before this header from
// the device build; tools/host_render never defines it, so it's a no-op.
#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

// Outer render loop (tracker.md "Render loop"): cuts each buffer into
// sub-blocks no larger than TRACKER_SUBBLOCK, also cut short at whatever
// `tick` boundary falls inside it. `out` is interleaved stereo int16_t,
// `frames` long in samples-per-channel. `nearest` picks mix_voice_nearest()
// over mix_voice() for every voice this call -- decided once per buffer, not
// per sample, so it costs nothing in the hot path either way.
inline void __not_in_flash_func(tracker_render_buffer)(
    TrackerVoice *voices, uint32_t num_voices,
    int16_t *out, uint32_t frames, TrackerTickState &tick, bool nearest = false) {
    uint32_t done = 0;
    while (done < frames) {
        if (tick.remaining == 0) tick.remaining = tick.samples_per_tick;

        uint32_t n = frames - done;
        if (tick.remaining < n) n = tick.remaining;
        if (TRACKER_SUBBLOCK < n) n = TRACKER_SUBBLOCK;

        int32_t accum[TRACKER_SUBBLOCK * 2];
        for (uint32_t i = 0; i < n * 2; i++) accum[i] = 0;

        if (nearest) {
            for (uint32_t v = 0; v < num_voices; v++) mix_voice_nearest(&voices[v], accum, n);
        } else {
            for (uint32_t v = 0; v < num_voices; v++) mix_voice(&voices[v], accum, n);
        }

        for (uint32_t i = 0; i < n; i++) {
            out[(done + i) * 2 + 0] = tracker_clip16(accum[i * 2 + 0]);
            out[(done + i) * 2 + 1] = tracker_clip16(accum[i * 2 + 1]);
        }

        done += n;
        tick.remaining -= n;
    }
}
