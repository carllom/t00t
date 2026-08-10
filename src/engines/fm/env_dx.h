#pragma once

#include "audio_common.h"
#include "fm_scale.h"
#include "patch.h"
#include <cmath>
#include <cstdint>

// EnvDX -- the DX7 envelope, rewritten by F3 (fm2.md §2/§5.7) as a direct
// port of Dexed's `Env` (Source/msfa/env.cc, Apache-2.0) rather than a
// re-derivation of its shape.
//
// The previous version was an honest reconstruction: 4 (rate, level) pairs
// stepped linearly in a log domain, with the operator's output level added
// separately as a static offset. F1's conformance harness measured what that
// cost, and the numbers are why this file was replaced rather than adjusted
// (fm2.md §5.4):
//
//   * an "instant" attack (rate 99) took 16.3 ms to reach -1 dB, against
//     Dexed's 0.0 ms -- because a linear ramp in the log domain has to
//     traverse the entire 40-octave floor before it becomes audible. Real DX7
//     rising stages do not traverse it: they JUMP to a fixed floor
//     (`jumptarget`, 1716) and then approach the target exponentially. That
//     jump *is* the DX7's instant attack, and no rate table can substitute
//     for it.
//   * rising and falling stages are different curve families on real
//     hardware -- exponential approach up, linear down. The old file used one
//     symmetric ramp for both.
//   * output level (TL) belongs INSIDE the envelope's own target
//     computation, not added to it afterwards. Dexed folds it in with a bias
//     and a floor clamp (`advance()` below); adding it separately put every
//     sustain stage in the wrong place, measured at ~60 dB low on E.PIANO 1.
//   * the slowest rates were capped at ~6 s by a `step < 1` clamp that this
//     formulation does not need at all.
//
// Everything below therefore follows Dexed's arithmetic exactly, in Dexed's
// own units, with exactly two documented deviations:
//
//   1. Control-block size. Dexed advances its envelope once per N=64 samples;
//      this engine advances once per FM_BLOCK (16 by default, op.h). `inc_`
//      is stored in Dexed's own per-64-sample units and scaled by the actual
//      block length at each step, so a 64-sample block reproduces Dexed
//      exactly and a shorter one samples the same curve more finely.
//   2. Output gain scale. Dexed's `Exp2::lookup(level_ - (14 << 24))` yields a
//      Q24 gain peaking at 2.0; this yields a linear gain peaking at op.h's
//      FM_GAIN_MAX. Same curve, anchored to this engine's own fixed-point
//      contract (F2). See eg_to_gain().

// ---------------------------------------------------------------------------
// DX7 parameter tables. All four of these were verified byte-identical to
// Dexed's by F1's conformance suite (fm2.md §5.4) and are unchanged by F3 --
// they were the parts of the old file that were already right.
// ---------------------------------------------------------------------------

// Dexed's `Env::scaleoutlevel()`. Levels 0-19 are a measured, non-formulaic
// table on real hardware; 20-99 is `28 + level`.
inline int dx7_scaleoutlevel(int level) {
    static constexpr int levellut[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29,
                                          31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
    return level >= 20 ? 28 + level : levellut[level];
}

// Dexed's `ScaleRate()` (dx7note.cc): a qrate DELTA from how far above a low
// reference note the played note is, scaled by the operator's own rate
// scaling (RS, 0-7). Higher notes get faster envelopes -- why plucked and
// bell patches tighten up correctly across the keyboard. Added to qrate at
// each stage transition, never to the converted speed.
inline int32_t dx7_scale_rate(int midinote, int sensitivity) {
    int x = midinote / 3 - 7;
    if (x < 0) x = 0;
    if (x > 31) x = 31;
    return (sensitivity * x) >> 3;
}

// Dexed's `ScaleCurve()`/`ScaleLevel()` (dx7note.cc). Curve values match DX7:
// 0=-LIN, 1=-EXP, 2=+EXP, 3=+LIN. Same raw unit scale as dx7_scaleoutlevel().
inline int dx7_scale_curve(int group, int depth, int curve) {
    static constexpr uint8_t exp_scale_data[33] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
        80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
    };
    int scale;
    if (curve == 0 || curve == 3) {
        scale = (group * depth * 329) >> 12;  // linear
    } else {
        int g = group > 32 ? 32 : group;
        scale = (exp_scale_data[g] * depth * 329) >> 15;  // exponential
    }
    return curve < 2 ? -scale : scale;
}

inline int32_t dx7_scale_level(int midinote, int break_pt, int left_depth, int right_depth,
                                int left_curve, int right_curve) {
    int offset = midinote - break_pt - 17;
    if (offset >= 0) return dx7_scale_curve((offset + 1) / 3, right_depth, right_curve);
    return dx7_scale_curve(-(offset - 1) / 3, left_depth, left_curve);
}

// Dexed's `ScaleVelocity()` (dx7note.cc), ported by F3. Returns a delta in
// the same "microstep" units as `outlevel` (1/32 of a scaleoutlevel unit,
// i.e. 1/256 octave), to be added AFTER outlevel is shifted left by 5.
//
// Replaces the old `eg_vel_sensitivity_log2()`, a hand-rolled model that was
// linear in velocity with a 4-octave span. F1 measured that at roughly a
// third of the real curve's depth -- at sensitivity 7 and velocity 0, Dexed
// attenuates by 3344 microsteps (-78.6 dB) where the old model gave 1024
// (-24.0 dB) -- and 894 of 1024 (velocity, sensitivity) pairs differed.
inline int32_t dx7_scale_velocity(int velocity, int sensitivity) {
    static constexpr uint8_t velocity_data[64] = {
        0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
        166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
        205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
        233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
        252, 253, 254
    };
    int clamped = velocity < 0 ? 0 : (velocity > 127 ? 127 : velocity);
    int vel_value = (int)velocity_data[clamped >> 1] - 239;
    return ((sensitivity * vel_value + 7) >> 3) << 4;
}

// ---------------------------------------------------------------------------
// Level domain and gain conversion.
// ---------------------------------------------------------------------------

// Dexed's `level_` unit: 2^24 == one octave (one doubling). The usable range
// is 0 .. EG_LEVEL_MAX, i.e. 15 octaves, which `advance()`'s composition
// below produces at output level 99 / EG level 99.
static constexpr int32_t EG_LEVEL_ONE_OCTAVE = 1 << 24;
static constexpr int32_t EG_LEVEL_MAX = 15 * EG_LEVEL_ONE_OCTAVE;

// Q15 exp2 fraction table, 2^(i/256). The 8 fractional bits it resolves are
// bits 16-23 of `level_`, so no interpolation is needed.
static constexpr uint32_t EG_EXP2_BITS = 8;
static constexpr uint32_t EG_EXP2_SIZE = 1u << EG_EXP2_BITS;
inline uint16_t eg_exp2_table[EG_EXP2_SIZE];

inline void env_dx_init_tables() {
    for (uint32_t i = 0; i < EG_EXP2_SIZE; i++) {
        eg_exp2_table[i] = (uint16_t)(exp2f((float)i / (float)EG_EXP2_SIZE) * 32768.0f);
    }
}

// `level_` (Q24 octaves) -> the linear gain op.h's kernels multiply by.
//
// Dexed computes `gain = Exp2::lookup(level_ - (14 << 24))`, a Q24 value
// peaking at 2.0 (= 2^25 raw) when level_ is at its 15-octave maximum. This
// engine's contract (op.h, F2) instead wants FM_GAIN_MAX = 2^28 at that same
// point, so the exponent is offset by 13 octaves rather than 14:
//
//     gain = 2^(level_/2^24 + 13)     ->  2^28 at level_ = 15 octaves
//
// Identical curve, identical dynamic range, anchored to this engine's own
// full scale. The clamp is defensive only: `advance()` cannot produce a
// level_ above EG_LEVEL_MAX, and F2's overflow bound (proved by
// `render_fm_patch --check`) depends on that ceiling holding.
// Below this, an operator contributes nothing and is treated as exactly
// silent. This is Dexed's `kLevelThresh` (fm_core.cc, 1120 in its Q24 gain
// units, scaled by 8 into this engine's), and it is load-bearing rather than
// an optimisation: `advance()` floors every stage target at 16 microsteps, so
// a DX7 envelope never actually reaches zero -- it bottoms out around -90 dB
// and stays there. Dexed reaches real silence by skipping any operator under
// this threshold entirely; clamping the gain to zero here is numerically the
// same thing for every writer (a carrier adds 0, a first-writer stores 0),
// without restructuring the render loop. It is what makes "this voice is
// finished" a true statement about the audio and not just about the
// envelope's stage counter.
static constexpr int32_t EG_LEVEL_THRESH = 1120 * 8;

inline int32_t eg_to_gain(int32_t level) {
    if (level < 0) level = 0;
    if (level > EG_LEVEL_MAX) level = EG_LEVEL_MAX;
    int32_t octaves = level >> 24;                       // 0..15
    uint32_t frac = ((uint32_t)level >> 16) & 0xFF;      // 8 bits, matches the table
    int32_t m = (int32_t)eg_exp2_table[frac];            // Q15, [2^15, 2^16)
    int32_t shift = octaves - 2;                         // (octaves + 13) - 15
    int32_t gain = shift >= 0 ? (m << shift) : (m >> -shift);
    if (gain < EG_LEVEL_THRESH) return 0;
    return gain > FM_GAIN_MAX ? FM_GAIN_MAX : gain;
}

// ---------------------------------------------------------------------------
// The envelope itself -- Dexed's Env, field for field.
// ---------------------------------------------------------------------------

// Dexed's own per-block sample count (synth.h's `N = 1 << LG_N`). `inc_` and
// the staticcount table are both expressed against it; deviation 1 above is
// implemented by scaling to the real block length at each step.
static constexpr int32_t EG_DEXED_LG_N = 6;
static constexpr int32_t EG_DEXED_N = 1 << EG_DEXED_LG_N;

struct EnvDX {
    uint8_t rates[4];
    uint8_t levels[4];
    int32_t outlevel;        // microsteps, already scaled by key level + velocity
    int32_t rate_scaling;    // qrate delta from dx7_scale_rate()
    int32_t level;           // Q24 octaves (Dexed's level_)
    int32_t targetlevel;
    int32_t inc;             // per EG_DEXED_N samples
    int32_t staticcount;     // samples remaining in a "hold" stage, 0 when not holding
    uint8_t ix;              // stage 0-3, or 4 == finished
    bool    rising;
    bool    down;            // key held

    // Dexed's `Env::isActive()`. A stage-4 (release) level of 0 is what makes
    // this a real terminal state rather than an epsilon guess -- and
    // tools/syx2patch.py forces exactly that on every carrier, so the voice
    // allocator's "this voice is free" decision stays sound (op.h's
    // fm_voice_active()).
    bool active() const { return ix < 4 || levels[3] > 0; }
};

// Dexed's `Env::advance()`. Recomputes the stage target, direction, rate and
// hold count. Called on init, on key-up, and whenever a stage completes.
inline void env_dx_advance(EnvDX &eg, int newix) {
    eg.ix = (uint8_t)newix;
    if (eg.ix >= 4) return;

    int newlevel = eg.levels[eg.ix];

    // The level composition F1 found missing (fm2.md §5.4). Output level is
    // folded into the stage target here, with a bias of 4256 and a floor of
    // 16 -- NOT added separately afterwards. The floor is what stops a
    // "silent" stage from being infinitely quiet: 16 microsteps is about
    // -90 dB below full scale, which is where Dexed's own envelopes bottom
    // out and what F1 measures as its resting level.
    int actuallevel = dx7_scaleoutlevel(newlevel) >> 1;
    actuallevel = (actuallevel << 6) + eg.outlevel - 4256;
    if (actuallevel < 16) actuallevel = 16;
    eg.targetlevel = actuallevel << 16;
    eg.rising = eg.targetlevel > eg.level;

    int qrate = (eg.rates[eg.ix] * 41) >> 6;
    qrate += eg.rate_scaling;
    if (qrate > 63) qrate = 63;
    if (qrate < 0) qrate = 0;

    // A stage that has nowhere to go still takes real time on hardware --
    // Dexed's ACCURATE_ENVELOPE path, an empirically measured dwell table.
    // Without it a same-level stage completes instantly and the envelope runs
    // through its whole shape early.
    if (eg.targetlevel == eg.level || (eg.ix == 0 && newlevel == 0)) {
        static constexpr int statics[77] = {
            1764000, 1764000, 1411200, 1411200, 1190700, 1014300, 992250,
            882000, 705600, 705600, 584325, 507150, 502740, 441000, 418950,
            352800, 308700, 286650, 253575, 220500, 220500, 176400, 145530,
            145530, 125685, 110250, 110250, 88200, 88200, 74970, 61740,
            61740, 55125, 48510, 44100, 37485, 31311, 30870, 27562, 27562,
            22050, 18522, 17640, 15435, 14112, 13230, 11025, 9261, 9261, 7717,
            6615, 6615, 5512, 5512, 4410, 3969, 3969, 3439, 2866, 2690, 2249,
            1984, 1896, 1808, 1411, 1367, 1234, 1146, 926, 837, 837, 705,
            573, 573, 529, 441, 441
        };
        int staticrate = eg.rates[eg.ix] + eg.rate_scaling;
        if (staticrate > 99) staticrate = 99;
        if (staticrate < 0) staticrate = 0;
        eg.staticcount = staticrate < 77 ? statics[staticrate] : 20 * (99 - staticrate);
        if (staticrate < 77 && eg.ix == 0 && newlevel == 0) {
            eg.staticcount /= 20;  // attack is scaled faster
        }
        // Dexed rescales by sr_multiplier here; this engine is fixed at
        // SAMPLE_RATE, and the table is already in samples at 44100.
        eg.staticcount = (int32_t)(((int64_t)eg.staticcount * 44100) / SAMPLE_RATE);
    } else {
        eg.staticcount = 0;
    }

    // Per EG_DEXED_N samples. Note there is no `step < 1` clamp anywhere in
    // this file: the smallest possible inc (qrate 0) is 4 << 8 = 1024, which
    // over the full 15-octave span works out to roughly six minutes -- the
    // genuinely slow envelopes the old formulation could not express.
    eg.inc = (4 + (qrate & 3)) << (2 + EG_DEXED_LG_N + (qrate >> 2));
}

// Power-on / never-triggered state. Distinct from a triggered envelope: `ix`
// is past the end and `level` is at the floor, so active() is false.
inline void env_dx_reset(EnvDX &eg) {
    for (int i = 0; i < 4; i++) { eg.rates[i] = 0; eg.levels[i] = 0; }
    eg.outlevel = 0;
    eg.rate_scaling = 0;
    eg.level = 0;
    eg.targetlevel = 0;
    eg.inc = 0;
    eg.staticcount = 0;
    eg.ix = 4;
    eg.rising = false;
    eg.down = false;
}

// Dexed's `Env::init()`. Note `level` restarts at 0 (silence) on every
// note-on, matching both Dexed and the old file's trigger convention.
inline void env_dx_init(EnvDX &eg, const uint8_t rates[4], const uint8_t levels[4],
                        int32_t outlevel, int32_t rate_scaling) {
    for (int i = 0; i < 4; i++) { eg.rates[i] = rates[i]; eg.levels[i] = levels[i]; }
    eg.outlevel = outlevel;
    eg.rate_scaling = rate_scaling;
    eg.level = 0;
    eg.down = true;
    env_dx_advance(eg, 0);
}

// Dexed's `Env::keydown(false)`: jump to the release stage from wherever the
// envelope currently is.
inline void env_dx_release(EnvDX &eg) {
    if (eg.down) {
        eg.down = false;
        env_dx_advance(eg, 3);
    }
}

// Dexed's `Env::getsample()`, advanced by `n` samples instead of a fixed 64.
// Returns the level at the END of the block -- the same point Dexed's own
// return value represents, which fm_core.cc consumes as that block's `gain2`.
//
// The two branches are the heart of F3. Rising is an exponential approach
// toward a fixed ceiling (17 octaves) with a hard jump to `jumptarget` first;
// falling is a plain linear ramp in the same log domain. They are different
// curve families, and the old file's single symmetric ramp is what made
// "instant" attacks take 16 ms.
inline int32_t env_dx_step_block(EnvDX &eg, uint32_t n) {
    if (eg.staticcount) {
        eg.staticcount -= (int32_t)n;
        if (eg.staticcount <= 0) {
            eg.staticcount = 0;
            env_dx_advance(eg, eg.ix + 1);
        }
    }

    if (eg.ix < 3 || (eg.ix < 4 && !eg.down)) {
        if (eg.staticcount) {
            // holding -- level does not move this block
        } else {
            // Deviation 1: Dexed's `inc` is per-64-samples; scale it to the
            // real block length. n == 64 reproduces Dexed exactly.
            int32_t inc_n = (int32_t)(((int64_t)eg.inc * (int64_t)n) >> EG_DEXED_LG_N);
            if (eg.rising) {
                const int32_t jumptarget = 1716;
                if (eg.level < (jumptarget << 16)) eg.level = jumptarget << 16;
                eg.level += (((17 << 24) - eg.level) >> 24) * inc_n;
                if (eg.level >= eg.targetlevel) {
                    eg.level = eg.targetlevel;
                    env_dx_advance(eg, eg.ix + 1);
                }
            } else {
                eg.level -= inc_n;
                if (eg.level <= eg.targetlevel) {
                    eg.level = eg.targetlevel;
                    env_dx_advance(eg, eg.ix + 1);
                }
            }
        }
    }
    return eg.level;
}

// Note-on `outlevel`, in microsteps -- Dexed's `Dx7Note::init()` composition,
// in its exact order: scaled output level, plus key level scaling, clamped to
// 127, shifted into microsteps, plus velocity, floored at 0. The clamp before
// the shift is load-bearing: a boosting key-scaling curve (+EXP/+LIN) at high
// depth can otherwise push an operator past the full-scale reference that
// op.h's FM_GAIN_MAX and F2's overflow bound both assume.
inline int32_t dx7_note_outlevel(const FmOpParams &p, int midinote, int velocity) {
    int outlevel = dx7_scaleoutlevel(p.output_level);
    outlevel += dx7_scale_level(midinote, p.scale_breakpoint, p.scale_left_depth,
                                 p.scale_right_depth, p.scale_left_curve, p.scale_right_curve);
    if (outlevel > 127) outlevel = 127;
    if (outlevel < 0) outlevel = 0;
    outlevel <<= 5;
    outlevel += dx7_scale_velocity(velocity, p.vel_sensitivity);
    return outlevel < 0 ? 0 : outlevel;
}
