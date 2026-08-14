#pragma once

#include <cstdint>

// Stripped 32-voice sample mixer (#15, module_tracker.md "Rendering Pipeline" /
// "Voice mixer"). Pure integer math, no pico-sdk, no ARM intrinsics — this
// header is included by both the on-device engine (audio_engine.cpp) and
// tools/host_render/render_tracker_mixer.cpp, which renders it to WAV for
// objective verification off-device (module_tracker.md "Testing": "against a host
// build of the same mixer"). Device-only concerns (profiling pin, DMA
// buffer handoff, __not_in_flash_func placement) stay out of this file.
//
// No voice_alloc, no envelopes, no LFO, no per-sample dispatch: a tracker
// voice is interpolate + scale + accumulate, nothing else (module_tracker.md
// "Performance Budget"). Pattern data, effects, and the ordered TickBlock
// ring land with the real player (build order steps 3-5); the tick cut here
// is a stub so sub-block-vs-tick-boundary slicing can be proven now.

// Sub-block size: a unit of parameter constancy, not of output. See
// module_tracker.md "Sub-block size" for the cost/resolution trade that fixed this
// at 64.
static constexpr uint32_t TRACKER_SUBBLOCK = 64;

// Position fixed-point format: Q18.14. Distinct from, and NOT compatible
// with, the existing engines' Q22.10 wavetable phase format (osc/common.h)
// -- that format is ~8.9 cents out of tune for sample playback at a typical
// tracker increment. See module_tracker.md "Fixed-Point Formats".
static constexpr uint32_t TRACKER_POS_FRAC_BITS = 14;

// Increment arrives as Q8.24 (module_tracker.md: "stored Q8.24 in the TickBlock").
static constexpr uint32_t TRACKER_INC_FRAC_BITS = 24;

// Matches blob_format.h's SampleHeader::loop_type convention exactly (raw
// 0/1/2, not a re-numbered local enum) so tracker_build_resident_samples()
// (player.h) can pass it straight through with no translation.
static constexpr uint8_t TRACKER_LOOP_NONE = 0;
static constexpr uint8_t TRACKER_LOOP_FORWARD = 1;
static constexpr uint8_t TRACKER_LOOP_PINGPONG = 2;

// One resident (SRAM) 8-bit sample. `data` must carry one guard sample
// appended past index [num_samples - 1] -- loop-start value if looped, last
// sample value if one-shot -- so idx+1 is always safe to read with no bounds
// check (module_tracker.md: "s[idx + 1] reads one past the end at the boundary").
// This still holds for a ping-pong loop: the guard is only ever reached when
// idx+1 == num_samples (loop_end == num_samples, the loop reaching the
// sample's physical end), and forward playback approaching that boundary
// reads forward exactly like a plain forward loop -- ping-pong's reflection
// happens at the *position* (mix_voice()/wrap_ping_pong() below), not by
// reading the buffer differently near the edge. #21 (see mixer.h's
// wrap_ping_pong()) verified this against openmpt123 rather than taking it
// on faith. Never points into XIP: 32 voices at scattered, non-integer
// strides would thrash the 8 KB XIP cache and evict Core 0's code with it.
struct TrackerSample {
    const int8_t *data;
    uint32_t num_samples;
    uint32_t loop_start;  // sample index
    uint32_t loop_end;    // sample index, exclusive; == num_samples for a loop reaching the physical end
    uint8_t loop_type;    // TRACKER_LOOP_{NONE,FORWARD,PINGPONG}
};

struct TrackerVoice {
    bool active;
    const TrackerSample *sample;
    uint32_t pos;                 // Q18.14
    uint32_t inc;                 // Q18.14 -- pre-shifted from Q8.24 once at latch time
    int32_t cur_volL, cur_volR;   // Q15, ramped per-sample toward target
    int32_t tgt_volL, tgt_volR;   // Q15
    // #21 ping-pong (module_tracker.md open question 2, "direction flag with a
    // mirrored read" -- the option chosen over host-side loop unrolling: #16
    // already measured ~20 points of Core 1 headroom at 32 voices, while
    // memory is the module's other hard limit (350-400 KB sample budget), so
    // this trades from the resource with slack rather than the one without
    // it. Meaningless (left false) for non-ping-pong samples -- mix_voice()
    // never reads it unless sample->loop_type == TRACKER_LOOP_PINGPONG.
    bool backward;
};

// Pre-shifts a Q8.24 increment to Q18.14 once, at (re)trigger time, so the
// per-sample path never touches it. "the shift is free" (module_tracker.md).
inline uint32_t tracker_latch_inc(uint32_t inc_q8_24) {
    return inc_q8_24 >> (TRACKER_INC_FRAC_BITS - TRACKER_POS_FRAC_BITS);
}

// Hoists the loop-wrap / end-of-sample test out of the per-sample path: the
// largest run playable from `pos` (Q18.14) before it reaches `end_pos`
// (Q18.14), so the inner loop needs no compare+branch. Precondition: inc > 0
// (a silent voice is `active = false`, never inc == 0). Used for one-shot,
// plain forward loops, and ping-pong's forward-direction phase -- pos is
// allowed to overshoot `end_pos` (unsigned-safe: addition never wraps low),
// and the overshoot is resolved afterward by wrap_loop()/wrap_ping_pong().
inline uint32_t samples_to_loop_end(uint32_t pos, uint32_t end_pos, uint32_t inc) {
    if (pos >= end_pos) return 0;
    uint32_t remaining = end_pos - pos;
    return (remaining + inc - 1) / inc;
}

// Ceil-based mirror of samples_to_loop_end() for backward playback:
// the largest run playable from `pos` (Q18.14) before it reaches
// `start_pos` moving backward, i.e. `pos -= inc` each step. Ping-pong
// only -- one-shot and plain forward loops never move backward.
inline uint32_t samples_to_loop_start(uint32_t pos, uint32_t start_pos, uint32_t inc) {
    if (pos <= start_pos) return 0;
    uint32_t remaining = pos - start_pos;
    return (remaining + inc - 1) / inc;
}

// Resolves a ping-pong voice once its position has reached or passed
// either boundary, reflecting -- possibly more than once, for a loop
// region shorter than one increment (module_tracker.md's own "short loops just
// produce more run iterations" case, pushed to its ping-pong extreme) --
// until `pos` lands strictly inside (loop_start_pos, loop_end_pos), and
// updates v->backward to match. A no-op if `pos` is already in range (the
// common case when this call's run was capped by the caller's remaining
// `n` rather than by an actual boundary), so mix_voice()/mix_voice_nearest()
// can call this unconditionally after every batch rather than needing to
// pre-check.
//
// `pos` arrives as signed 64-bit, computed by the caller directly from the
// batch's entry position (`entry_pos +/- run*inc`) rather than trusted from
// the uint32_t `pos` the per-sample loop just advanced: a ping-pong
// reflection routinely needs to represent a position before the loop start
// or past the loop end by more than the boundary itself, which a Q18.14
// uint32_t position cannot hold, unlike the plain forward loop's always-
// safe unsigned overshoot (wrap_loop() below).
//
// The pivots are `loop_end_pos - 1` / `loop_start_pos + 1`, not the
// textbook `2*boundary - pos`, to avoid a fixed point: reflecting a *zero-
// overshoot* landing exactly on `loop_end_pos` with the textbook formula
// returns that same value unchanged, which -- since loop_end_pos is an
// exclusive bound no read may land on (mix_voice()'s s[idx+1] would be one
// past what the interpolator's guard sample covers) -- would leave the
// voice re-deriving the identical "still out of range" result forever.
// Landing exactly on an integer sample boundary requires the increment to
// divide the loop length exactly from a whole-sample start: a measure-zero
// coincidence for any real pitch/loop combination, so the 2-part-in-16384-
// of-a-sample bias this introduces on every bounce is inaudible.
inline void wrap_ping_pong(TrackerVoice *v, uint32_t loop_start_pos, uint32_t loop_end_pos, int64_t pos) {
    bool backward = v->backward;
    for (;;) {
        if (pos >= (int64_t)loop_end_pos) {
            pos = 2 * ((int64_t)loop_end_pos - 1) - pos;
            backward = true;
        } else if (pos <= (int64_t)loop_start_pos) {
            pos = 2 * ((int64_t)loop_start_pos + 1) - pos;
            backward = false;
        } else {
            break;
        }
    }
    v->pos = (uint32_t)pos;
    v->backward = backward;
}

// Plain forward loops only -- ping-pong uses wrap_ping_pong() above instead,
// dispatched by mix_voice()/mix_voice_nearest() on sample->loop_type before
// ever calling either. One-shot voices go inactive at their end instead of
// wrapping.
inline void wrap_loop(TrackerVoice *v) {
    if (v->sample->loop_type == TRACKER_LOOP_FORWARD) {
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
// increment is held constant -- both per module_tracker.md's ramping rules (a
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
    const bool pingpong = v->sample->loop_type == TRACKER_LOOP_PINGPONG;
    const uint32_t loop_start_pos = v->sample->loop_start << TRACKER_POS_FRAC_BITS;
    const uint32_t end_pos = (v->sample->loop_type != TRACKER_LOOP_NONE ? v->sample->loop_end : v->sample->num_samples)
                              << TRACKER_POS_FRAC_BITS;

    while (n > 0) {
        // #21: direction only ever matters for a ping-pong voice -- every
        // other loop_type always reads forward, matching mix_voice()'s
        // pre-#21 shape exactly (same samples_to_loop_end() call, same
        // pos += inc loop below) with zero added branches on that path.
        bool backward = pingpong && v->backward;
        uint32_t run = backward ? samples_to_loop_start(pos, loop_start_pos, inc)
                                 : samples_to_loop_end(pos, end_pos, inc);
        if (run > n) run = n;
        const uint32_t entry_pos = pos;

        if (!backward) {
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
        } else {
            // Ping-pong's backward phase: same fetch/interpolate as forward
            // (idx/idx+1 stay adjacent-in-buffer regardless of which way pos
            // is moving), pos just counts down instead of up. The very last
            // `pos -= inc` of a boundary-crossing run can underflow a
            // uint32_t -- harmless here since `pos` is never read again
            // after that (the batch's exact end position is recomputed
            // below from `entry_pos`, not trusted from this local `pos`).
            for (uint32_t i = 0; i < run; i++) {
                uint32_t idx = pos >> TRACKER_POS_FRAC_BITS;
                int32_t frac = (int32_t)(pos & ((1u << TRACKER_POS_FRAC_BITS) - 1));
                int32_t a = (int32_t)s[idx] << 8;
                int32_t b = (int32_t)s[idx + 1] << 8;
                int32_t smp = a + (((b - a) * frac) >> TRACKER_POS_FRAC_BITS);

                acc[0] += (smp * volL) >> 15;
                acc[1] += (smp * volR) >> 15;
                acc += 2;

                pos -= inc;
                volL += dL;
                volR += dR;
            }
        }

        n -= run;
        if (pingpong) {
            // Always resolved -- not gated behind `n > 0` the way the plain
            // forward-loop branch below is -- because the result becomes
            // v->pos either way, and a ping-pong voice's `pos` must never be
            // left at/past a boundary between calls (unlike a forward loop's
            // lazy overshoot, this one isn't representable: see
            // wrap_ping_pong()'s header comment). Computed from `entry_pos`
            // with signed 64-bit arithmetic rather than the local `pos`
            // above, which the backward branch's last step may have just
            // taken out of uint32_t range. A no-op when still in range (the
            // common case when `run` was capped by `n` rather than by an
            // actual boundary).
            int64_t exact = (int64_t)entry_pos + (backward ? -1 : 1) * (int64_t)run * (int64_t)inc;
            wrap_ping_pong(v, loop_start_pos, end_pos, exact);
            pos = v->pos;
        } else if (n > 0) {
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

// Nearest-neighbour sibling of mix_voice() (#16, module_tracker.md open question 2):
// same loop-wrap/end-of-sample structure, but the inner sample fetch skips
// the interpolation lerp entirely. Exists to measure whether the saving is
// worth a build flag -- see history_tracker.md's tracker interpolation measurement.
inline void mix_voice_nearest(TrackerVoice *v, int32_t *acc, uint32_t n) {
    if (!v->active || n == 0) return;

    int32_t volL = v->cur_volL, volR = v->cur_volR;
    int32_t dL = (v->tgt_volL - volL) / (int32_t)n;
    int32_t dR = (v->tgt_volR - volR) / (int32_t)n;

    uint32_t pos = v->pos;
    const uint32_t inc = v->inc;
    const int8_t *s = v->sample->data;
    const bool pingpong = v->sample->loop_type == TRACKER_LOOP_PINGPONG;
    const uint32_t loop_start_pos = v->sample->loop_start << TRACKER_POS_FRAC_BITS;
    const uint32_t end_pos = (v->sample->loop_type != TRACKER_LOOP_NONE ? v->sample->loop_end : v->sample->num_samples)
                              << TRACKER_POS_FRAC_BITS;

    while (n > 0) {
        bool backward = pingpong && v->backward;
        uint32_t run = backward ? samples_to_loop_start(pos, loop_start_pos, inc)
                                 : samples_to_loop_end(pos, end_pos, inc);
        if (run > n) run = n;
        const uint32_t entry_pos = pos;

        if (!backward) {
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
        } else {
            for (uint32_t i = 0; i < run; i++) {
                uint32_t idx = pos >> TRACKER_POS_FRAC_BITS;
                int32_t smp = (int32_t)s[idx] << 8;

                acc[0] += (smp * volL) >> 15;
                acc[1] += (smp * volR) >> 15;
                acc += 2;

                pos -= inc;
                volL += dL;
                volR += dR;
            }
        }

        n -= run;
        if (pingpong) {
            int64_t exact = (int64_t)entry_pos + (backward ? -1 : 1) * (int64_t)run * (int64_t)inc;
            wrap_ping_pong(v, loop_start_pos, end_pos, exact);
            pos = v->pos;
        } else if (n > 0) {
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
// instruction (module_tracker.md acceptance: "clipped with __ssat on store");
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

// Outer render loop (module_tracker.md "Render loop"): cuts each buffer into
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
