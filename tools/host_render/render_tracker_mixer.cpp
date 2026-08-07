// Host-side proof of src/engines/tracker/mixer.h (#15): renders the stripped
// 32-voice sample mixer to WAV and checks its behaviour objectively before
// trusting the on-device render. Referenced directly by tracker.md
// "Testing": "diff against device output (or against a host build of the
// same mixer)" -- this is that host build. No pico-sdk, no ARM intrinsics;
// mixer.h's __not_in_flash_func and __ssat both no-op/fall back to a plain
// clamp when __arm__ is unset (see mixer.h).
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_tracker_mixer
#include "../../src/engines/tracker/mixer.h"
#include "../../src/audio_common.h"
#include "../../src/pan.h"
#include "../../samples/sararr1.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// --- Test 1: volume ramps linearly to target, pitch does not ramp ---
//
// A constant-valued "sample" turns mix_voice's output into a pure readout of
// the volume ramp: every non-zero output sample is target_L/R scaled by
// whatever fraction of the ramp has elapsed. Verifies tracker.md's ramping
// rule (amplitude ramps per sub-block, increment is held fixed) exactly,
// rather than by ear.
static bool test_ramp_linearity() {
    static int8_t flat_data[TRACKER_SUBBLOCK + 2];
    for (auto &b : flat_data) b = 100;  // constant -> interpolated sample is always 100<<8
    TrackerSample flat = { flat_data, TRACKER_SUBBLOCK + 1, 0, TRACKER_SUBBLOCK + 1, TRACKER_LOOP_NONE };

    TrackerVoice v{};
    v.active = true;
    v.sample = &flat;
    v.pos = 0;
    v.inc = tracker_latch_inc(1u << TRACKER_INC_FRAC_BITS);  // ratio 1.0 -> one index per sample
    v.cur_volL = v.cur_volR = 0;
    v.tgt_volL = v.tgt_volR = 1000;

    const uint32_t n = TRACKER_SUBBLOCK;
    std::vector<int32_t> acc(n * 2, 0);
    mix_voice(&v, acc.data(), n);

    const int32_t smp = 100 << 8;
    int32_t expect_vol = 0;
    const int32_t dL = v.tgt_volL / (int32_t)n;  // matches mix_voice's own truncation
    bool ok = true;
    int32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        // mix_voice uses cur_vol *before* stepping it -- sample 0 is the
        // pre-ramp value (0), matching tracker.md's pseudocode.
        int32_t expected = (smp * expect_vol) >> 15;
        if (acc[i * 2] != expected || acc[i * 2 + 1] != expected) {
            printf("  FAIL sample %u: got L=%d R=%d, want %d\n", i, acc[i * 2], acc[i * 2 + 1], expected);
            ok = false;
        }
        // Monotonic, and no single-step jump bigger than one ramp increment
        // -- the discontinuity-free property "volume must ramp" exists for.
        if (acc[i * 2] < prev) {
            printf("  FAIL sample %u: ramp not monotonic (%d -> %d)\n", i, prev, acc[i * 2]);
            ok = false;
        }
        prev = acc[i * 2];
        expect_vol += dL;
    }
    if (v.cur_volL != expect_vol || v.cur_volR != expect_vol) {
        printf("  FAIL: cur_vol not carried across the call correctly (%d/%d vs %d)\n",
               v.cur_volL, v.cur_volR, expect_vol);
        ok = false;
    }
    printf("  %s: %u-sample ramp from 0 to %d, final=%d\n", ok ? "PASS" : "FAIL", n, v.tgt_volL, prev);
    return ok;
}

// --- Test 2: forward loop wrap never overruns loop_end ---
//
// A tiny 8-sample loop (region [2,6)) at exactly ratio 1.0 so idx is
// predictable. Steps one sample at a time via repeated mix_voice(n=1) calls
// and checks the played index never leaves [0, loop_end) and, after the
// first wrap, stays within the loop region -- samples_to_loop_end()'s
// hoisted bound is the only thing standing between this and an out-of-bounds
// read.
static bool test_loop_wrap() {
    static const int8_t data[9] = { 0, 10, 20, 30, 40, 50, 60, 70, 20 };  // [8]=guard (loop_start value, unused here since loop_end<8)
    TrackerSample sample = { data, 8, 2, 6, TRACKER_LOOP_FORWARD };

    TrackerVoice v{};
    v.active = true;
    v.sample = &sample;
    v.pos = 0;
    v.inc = tracker_latch_inc(1u << TRACKER_INC_FRAC_BITS);
    v.cur_volL = v.cur_volR = v.tgt_volL = v.tgt_volR = 32767;

    const uint32_t loop_end_pos = 6u << TRACKER_POS_FRAC_BITS;
    const uint32_t loop_len_pos = (6u - 2u) << TRACKER_POS_FRAC_BITS;

    bool ok = true;
    bool seen_wrap = false;
    uint32_t prev_idx = 0;
    for (int step = 0; step < 40; step++) {
        // mix_voice wraps lazily: a call that starts with pos already at
        // loop_end_pos wraps *before* reading anything, so the index this
        // call actually reads is pos-after-that-lazy-wrap, not raw pos.
        uint32_t pos = v.pos;
        if (pos >= loop_end_pos) pos -= loop_len_pos;
        uint32_t idx = pos >> TRACKER_POS_FRAC_BITS;

        int32_t acc[2] = { 0, 0 };
        mix_voice(&v, acc, 1);
        if (idx >= 8) {
            printf("  FAIL step %d: idx=%u out of bounds\n", step, idx);
            ok = false;
        }
        if (idx < prev_idx) seen_wrap = true;
        if (seen_wrap && (idx < 2 || idx >= 6)) {
            printf("  FAIL step %d: idx=%u outside loop region [2,6) after wrap\n", step, idx);
            ok = false;
        }
        prev_idx = idx;
    }
    printf("  %s: 40 steps through an 8-sample buffer with a [2,6) loop, wrapped=%s\n",
           ok && seen_wrap ? "PASS" : "FAIL", seen_wrap ? "yes" : "no");
    return ok && seen_wrap;
}

// --- Test 2b: ping-pong loop bounces within [loop_start, loop_end), never
// overruns either side (#21) ---
//
// Same 8-sample buffer/[2,6) region as test_loop_wrap() but with an
// increment (6 samples/step) larger than the loop region (loop_len=4
// samples) -- tracker.md's own "deliberately tight ... loop" measurement
// convention (#16) pushed to ping-pong's worst case, so wrap_ping_pong()'s
// multi-reflection path (more than one bounce per call) actually gets
// exercised, not just a single mirror.
static bool test_pingpong_wrap() {
    static const int8_t data[9] = { 0, 10, 20, 30, 40, 50, 60, 70, 10 };  // [8]=guard (loop_start value)
    TrackerSample sample = { data, 8, 2, 6, TRACKER_LOOP_PINGPONG };

    TrackerVoice v{};
    v.active = true;
    v.sample = &sample;
    v.pos = 2u << TRACKER_POS_FRAC_BITS;  // start at loop_start, matching a real trigger
    v.backward = false;
    v.inc = tracker_latch_inc(6u * (1u << TRACKER_INC_FRAC_BITS));  // ratio 6.0 -> 6 samples/step in Q18.14
    v.cur_volL = v.cur_volR = v.tgt_volL = v.tgt_volR = 32767;

    bool ok = true;
    bool seen_forward_to_backward = false, seen_backward_to_forward = false;
    bool prev_backward = v.backward;
    for (int step = 0; step < 60; step++) {
        // mix_voice() guarantees v.pos strictly inside [loop_start_pos,
        // loop_end_pos) between calls for a ping-pong voice (never left
        // at/past a boundary the way a plain forward loop's lazy overshoot-
        // then-wrap can -- wrap_ping_pong() is called unconditionally after
        // every batch instead, see its header comment), so idx must stay
        // inside [2,6) before every call.
        uint32_t idx = v.pos >> TRACKER_POS_FRAC_BITS;
        if (idx < 2 || idx >= 6) {
            printf("  FAIL step %d: idx=%u outside [2,6) before a call\n", step, idx);
            ok = false;
        }

        int32_t acc[2] = { 0, 0 };
        mix_voice(&v, acc, 1);

        if (prev_backward != v.backward) {
            if (v.backward) seen_forward_to_backward = true;
            else seen_backward_to_forward = true;
        }
        prev_backward = v.backward;
    }
    bool bounced = seen_forward_to_backward && seen_backward_to_forward;
    printf("  %s: 60 steps through a [2,6) ping-pong loop at 6x rate (loop shorter than one increment), "
           "stayed in-bounds, bounced=%s\n", ok && bounced ? "PASS" : "FAIL", bounced ? "yes" : "no");
    return ok && bounced;
}

// --- Test 2c: ping-pong at an increment that divides the loop length
// exactly, so every bounce lands on an *exact* integer sample boundary
// (#21 regression) ---
//
// wrap_ping_pong()'s reflection pivot is deliberately offset by one Q18.14
// unit specifically to avoid a fixed point at a zero-overshoot landing --
// without it, this exact configuration (loop_len=4, inc=2 -- every bounce
// lands precisely on loop_start_pos or loop_end_pos with zero fractional
// remainder) hangs mix_voice() forever. Any regression to the textbook
// `2*boundary - pos` reflection formula reproduces that hang here.
static bool test_pingpong_exact_boundary() {
    static const int8_t data[9] = { 0, 10, 20, 30, 40, 50, 60, 70, 10 };
    TrackerSample sample = { data, 8, 2, 6, TRACKER_LOOP_PINGPONG };

    TrackerVoice v{};
    v.active = true;
    v.sample = &sample;
    v.pos = 2u << TRACKER_POS_FRAC_BITS;
    v.backward = false;
    v.inc = tracker_latch_inc(2u * (1u << TRACKER_INC_FRAC_BITS));  // ratio 2.0 -- divides loop_len(4) exactly
    v.cur_volL = v.cur_volR = v.tgt_volL = v.tgt_volR = 32767;

    bool ok = true;
    bool bounced = false;
    bool prev_backward = v.backward;
    for (int step = 0; step < 100; step++) {
        uint32_t idx = v.pos >> TRACKER_POS_FRAC_BITS;
        if (idx < 2 || idx >= 6) {
            printf("  FAIL step %d: idx=%u outside [2,6) before a call\n", step, idx);
            ok = false;
        }
        int32_t acc[2] = { 0, 0 };
        mix_voice(&v, acc, 1);  // must return -- a fixed-point regression here is an infinite loop, not a wrong answer
        if (prev_backward != v.backward) bounced = true;
        prev_backward = v.backward;
    }
    printf("  %s: 100 steps at an exact-integer-boundary-aligned rate completed without hanging, bounced=%s\n",
           ok && bounced ? "PASS" : "FAIL", bounced ? "yes" : "no");
    return ok && bounced;
}

// --- Test 3: one-shot sample goes inactive exactly at its end, no overrun ---
static bool test_oneshot_end() {
    static const int8_t guarded[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 8 };  // guard = last value
    TrackerSample sample = { guarded, 8, 0, 8, TRACKER_LOOP_NONE };

    TrackerVoice v{};
    v.active = true;
    v.sample = &sample;
    v.pos = 0;
    v.inc = tracker_latch_inc(1u << TRACKER_INC_FRAC_BITS);
    v.cur_volL = v.cur_volR = v.tgt_volL = v.tgt_volR = 32767;

    bool ok = true;
    int played = 0;
    for (int step = 0; step < 8; step++) {
        if (!v.active) { printf("  FAIL: went inactive early at step %d\n", step); ok = false; break; }
        int32_t acc[2] = { 0, 0 };
        mix_voice(&v, acc, 1);
        played++;
    }
    if (!v.active) {
        printf("  FAIL: went inactive before playing all %d samples (played %d)\n", 8, played);
        ok = false;
    }
    // The 9th call is what notices the sample ran out (tries to read past
    // the last index and finds nothing left to wrap to) -- that's when
    // `active` clears. You can't know sample N was the last one until you
    // try to go past it.
    int32_t acc9[2] = { 0, 0 };
    mix_voice(&v, acc9, 1);
    if (v.active) {
        printf("  FAIL: still active after the sample ran out\n");
        ok = false;
    }
    // Further calls on an inactive voice must be no-ops (silent, no crash).
    int32_t acc[2] = { 5, 5 };
    mix_voice(&v, acc, 1);
    if (acc[0] != 5 || acc[1] != 5) {
        printf("  FAIL: inactive voice still wrote into the accumulator\n");
        ok = false;
    }
    printf("  %s: one-shot voice played %d/%d samples, went inactive on the call after\n", ok ? "PASS" : "FAIL", played, 8);
    return ok;
}

// --- Test 4: mix_voice_nearest() skips interpolation entirely (#16) ---
//
// Non-constant sample data + a non-integer increment (ratio 1.5, so every
// other step lands on a non-zero fractional position) makes nearest and
// linear diverge predictably. Checks mix_voice_nearest()'s output against
// the un-interpolated s[idx] exactly, and cross-checks against mix_voice()
// on an identical voice to confirm the two genuinely differ where the
// fractional position is non-zero -- i.e. that nearest is really skipping
// the lerp, not coincidentally matching it.
static bool test_nearest_neighbor() {
    static const int8_t data[7] = { 0, 100, -100, 40, -40, 20, 20 };  // [6]=guard (last value)
    TrackerSample sample = { data, 6, 0, 6, TRACKER_LOOP_NONE };

    auto make_voice = [&]() {
        TrackerVoice v{};
        v.active = true;
        v.sample = &sample;
        v.pos = 0;
        v.inc = tracker_latch_inc((uint32_t)(1.5f * (float)(1u << TRACKER_INC_FRAC_BITS)));  // ratio 1.5
        v.cur_volL = v.cur_volR = v.tgt_volL = v.tgt_volR = 32767;
        return v;
    };
    TrackerVoice vn = make_voice();
    TrackerVoice vl = make_voice();

    bool ok = true;
    bool saw_divergence = false;
    for (int step = 0; step < 4; step++) {
        uint32_t idx = vn.pos >> TRACKER_POS_FRAC_BITS;
        uint32_t frac = vn.pos & ((1u << TRACKER_POS_FRAC_BITS) - 1);
        int32_t expected = ((int32_t)data[idx] << 8) * 32767 >> 15;

        int32_t accn[2] = { 0, 0 };
        mix_voice_nearest(&vn, accn, 1);
        if (accn[0] != expected || accn[1] != expected) {
            printf("  FAIL step %d: nearest got L=%d R=%d, want %d (idx=%u)\n", step, accn[0], accn[1], expected, idx);
            ok = false;
        }

        int32_t accl[2] = { 0, 0 };
        mix_voice(&vl, accl, 1);
        if (frac != 0 && accl[0] != accn[0]) saw_divergence = true;
    }
    if (!saw_divergence) {
        printf("  FAIL: nearest and linear never diverged -- test isn't exercising the lerp\n");
        ok = false;
    }
    printf("  %s: nearest matches un-interpolated s[idx] exactly, diverges from linear at fractional positions\n",
           ok ? "PASS" : "FAIL");
    return ok;
}

// --- Test 5: the full hardcoded 32-voice configuration (#15 acceptance) ---
//
// Same shape as tracker/audio_engine.cpp's setup_test_voices(): half looped,
// half one-shot, +-1 octave detuned chorus per half, panned across the
// stereo field. Renders 2 seconds through tracker_render_buffer() (the real
// sub-block/tick-cut outer loop, not mix_voice() directly) and writes a WAV
// for listening, plus sanity-checks the aggregate behaviour.
// Mirrors tracker/engine.h's MAX_VOICES (not pulled in directly: engine.h
// drags in engine_base.h -> hardware/sync.h, a pico-sdk header this
// standalone host build has no access to).
static constexpr uint32_t MAX_VOICES = 32;

static bool test_full_mix() {
    osc_init_sine();  // pan_gains_q15's quadrature source

    static int8_t sample_ram[sararr1_NUM_SAMPLES + 1];
    for (uint32_t i = 0; i < sararr1_NUM_SAMPLES; i++) sample_ram[i] = sararr1_data[i];
    sample_ram[sararr1_NUM_SAMPLES] = sararr1_data[sararr1_NUM_SAMPLES - 1];

    TrackerSample looped  = { sample_ram, sararr1_NUM_SAMPLES, sararr1_LOOP_START, sararr1_LOOP_END, TRACKER_LOOP_FORWARD };
    TrackerSample oneshot = { sample_ram, sararr1_NUM_SAMPLES, 0, sararr1_NUM_SAMPLES, TRACKER_LOOP_NONE };

    TrackerVoice voices[MAX_VOICES];
    const float base_ratio = (float)sararr1_SAMPLE_RATE / (float)SAMPLE_RATE;
    constexpr int32_t VOICE_AMP_Q15 = 1024;
    constexpr uint32_t HALF = MAX_VOICES / 2;

    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        TrackerVoice &v = voices[i];
        bool looped_group = (i < HALF);
        uint32_t group_i = looped_group ? i : (i - HALF);
        float octaves = ((float)group_i - 7.5f) / 15.0f * 2.0f;
        float ratio = base_ratio * exp2f(octaves);
        uint32_t inc_q8_24 = (uint32_t)(ratio * (float)(1u << TRACKER_INC_FRAC_BITS));

        int16_t pan = (int16_t)(((int32_t)i * 65535 / (MAX_VOICES - 1)) - 32768);
        int32_t gain_l, gain_r;
        pan_gains_q15(pan, gain_l, gain_r);

        v.active = true;
        v.sample = looped_group ? &looped : &oneshot;
        v.pos = 0;
        v.inc = tracker_latch_inc(inc_q8_24);
        v.cur_volL = v.cur_volR = 0;
        v.tgt_volL = (VOICE_AMP_Q15 * gain_l) >> 15;
        v.tgt_volR = (VOICE_AMP_Q15 * gain_r) >> 15;
    }

    TrackerTickState tick{};
    tick.samples_per_tick = SAMPLE_RATE / 50;
    tick.remaining = tick.samples_per_tick;

    // The slowest one-shot voice (group_i=0, -1 octave, ratio = base_ratio/2)
    // takes num_samples/ratio ~= 101k frames (~2.3s) to finish; render past
    // that with margin so every one-shot has definitely gone inactive.
    const uint32_t total_frames = SAMPLE_RATE * 3;
    std::vector<int16_t> out(total_frames * 2);

    std::vector<int16_t> buf(SAMPLES_PER_BUFFER * 2);
    uint32_t done = 0;
    int32_t peak = 0;
    uint64_t full_scale_count = 0;
    uint32_t active_at_start = 0, active_at_end = 0;
    while (done < total_frames) {
        uint32_t n = std::min<uint32_t>(SAMPLES_PER_BUFFER, total_frames - done);
        tracker_render_buffer(voices, MAX_VOICES, buf.data(), n, tick);
        for (uint32_t i = 0; i < n * 2; i++) {
            out[done * 2 + i] = buf[i];
            int32_t mag = buf[i] < 0 ? -buf[i] : buf[i];
            if (mag > peak) peak = mag;
            if (mag >= 32767) full_scale_count++;
        }
        if (done == 0) {
            for (uint32_t v = 0; v < MAX_VOICES; v++) active_at_start += voices[v].active ? 1 : 0;
        }
        done += n;
    }
    for (uint32_t v = 0; v < MAX_VOICES; v++) active_at_end += voices[v].active ? 1 : 0;

    write_wav_pcm16("tracker_mixer_32voice.wav", out, SAMPLE_RATE, 2);

    bool ok = true;
    // The 16 one-shot voices must have finished (sararr1 is much shorter
    // than 2s at these ratios); the 16 looped voices must still be sounding.
    if (active_at_end != HALF) {
        printf("  FAIL: expected %u voices still active (looped half only) at end, got %u\n", HALF, active_at_end);
        ok = false;
    }
    double full_scale_frac = (double)full_scale_count / (double)out.size();
    if (full_scale_frac > 0.01) {
        printf("  FAIL: %.2f%% of samples at full scale -- volume budget is clipping heavily\n", full_scale_frac * 100.0);
        ok = false;
    }
    printf("  %s: %u voices active at start, %u at end (want %u); peak=%d; full-scale=%.3f%%\n",
           ok ? "PASS" : "FAIL", active_at_start, active_at_end, HALF, peak, full_scale_frac * 100.0);
    printf("  wrote tracker_mixer_32voice.wav\n");
    return ok;
}

int main() {
    bool ok = true;

    printf("== ramp linearity (volume ramps, pitch does not) ==\n");
    ok = test_ramp_linearity() && ok;

    printf("\n== forward loop wrap bounds ==\n");
    ok = test_loop_wrap() && ok;

    printf("\n== ping-pong loop wrap bounds + bounce (#21) ==\n");
    ok = test_pingpong_wrap() && ok;

    printf("\n== ping-pong at an exact integer boundary (#21 regression) ==\n");
    ok = test_pingpong_exact_boundary() && ok;

    printf("\n== one-shot end-of-sample ==\n");
    ok = test_oneshot_end() && ok;

    printf("\n== nearest-neighbour vs linear interpolation ==\n");
    ok = test_nearest_neighbor() && ok;

    printf("\n== full 32-voice hardcoded configuration ==\n");
    ok = test_full_mix() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
