#pragma once

#include "audio_common.h"
#include "patch.h"
#include <cstdint>

// FmPitchEg -- the DX7 pitch envelope (#49, module_fm.md §5.4): one PER VOICE (not
// per operator, unlike EnvDX), same 4-stage (rate, level) shape and
// block-rate-stepping convention as env_dx.h's EnvDX, but its per-block
// result is consumed differently: instead of handing the kernel a
// gain/gain_step pair, op.h's fm_voice_step_pitch_and_mod() converts it (plus
// lfo.h's own pitch-mod output) into a ratio that scales every non-fixed-
// frequency operator's `inc` once per control block -- module_fm.md's own "applied
// by scaling all six operator increments at each block boundary ... zero
// per-sample cost". Nothing here ever touches op_render/op_render_first/
// op_render_fb (op.h) or the kernel's `gain`/`gain_step` fields at all.
//
// Rate and level tables ported verbatim from Dexed's own PitchEnv
// (Source/msfa/pitchenv.{h,cc}, Apache-2.0) -- same "port real hardware-
// calibrated data, don't re-derive a formula guess" rigor #58/#59 already
// established for env_dx.h's own DX7 parameter tables.
// The combination formula around those tables (below) is NOT a bit-exact
// port, though -- Dexed's own PitchEnv works in a Q24-octave/N-sample-block
// fixed-point domain tuned to Dexed's own internal block size and unit
// scale, none of which this file needs to replicate to get the same real
// numbers. Instead each table was cross-checked by porting-and-*running*
// Dexed's actual formula in a standalone calibration harness (same method
// #57's beta calibration and #58/#59's curve checks used) to pin down real
// cents/octaves-per-second anchors, then re-expressed in this file's own
// plain-float cents domain against those anchors:
//   - level 0/50/99  -> -4800 / 0 / +4762.5 cents (DX7_PITCHENV_LEVEL's own
//     +-128 raw span IS the real DX7's +-4-octave pitch EG range, so the
//     conversion is a single multiply, no rescaling surprises).
//   - rate 0/99 -> 0.047 / 11.97 octaves/sec -- a rate-99 pitch EG sweeps
//     its full ~8-octave span in ~0.67s, a real, audible swoop, NOT instant
//     like the amplitude EG's own rate 99 (env_dx.h). That is correct DX7
//     character, not a bug: the "attack blip" / "brass scoop" this issue
//     names is a fast RELATIVE move over a few tens or hundreds of cents
//     (well under the 0.67s full-span figure), not a full-range sweep.

// Dexed's `pitchenv_tab` (Source/msfa/pitchenv.cc, Apache-2.0), ported
// verbatim -- 100 raw units (index 0-99) spanning the DX7's real pitch EG
// range. See this file's own header comment for the real cents anchors.
inline constexpr int8_t DX7_PITCHENV_LEVEL[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43,
    -41, -39, -37, -35, -33, -32, -31, -30, -29, -28, -27, -26, -25, -24,
    -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11, -10,
    -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73,
    82, 92, 103, 115, 127
};

// Dexed's `pitchenv_rate` (Source/msfa/pitchenv.cc, Apache-2.0), ported
// verbatim -- 100 raw units (index 0-99), real octaves/sec = raw/21.3 (the
// `21.3` constant is Dexed's own `PitchEnv::init()`, and cancels out
// Dexed's own sample-rate/block-size normalization entirely -- see this
// file's header comment for the derivation and the real rate 0/99 anchors).
inline constexpr uint8_t DX7_PITCHENV_RATE[100] = {
    1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 41, 42, 44, 46, 47,
    49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82,
    85, 88, 91, 94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147,
    153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255
};

inline float dx7_pitchenv_level_to_cents(int level) {
    // DX7_PITCHENV_LEVEL's +-128 raw span == the real DX7's own +-4-octave
    // (+-4800-cent) pitch EG range -- 4800/128 = 37.5 cents/raw-unit.
    return (float)DX7_PITCHENV_LEVEL[level] * 37.5f;
}

inline float dx7_pitchenv_rate_to_octaves_per_sec(int rate) {
    return (float)DX7_PITCHENV_RATE[rate] * (1.0f / 21.3f);
}

// Stage indices double as direct FmPitchEgParams::rate[]/level[] indices
// (0-3), same convention env_dx.h's EgStage uses -- but unlike EnvDX there
// is no EG_IDLE terminal state: pitch has no "silence" to report, so
// nothing needs to know when this is "done" (fm_voice_active(), op.h, only
// ever looks at each operator's own amplitude EG). Stage 4 (index 3, the
// release target) simply holds forever once reached, the same way stage 3
// already holds while a note is held -- verified against Dexed's own
// `PitchEnv::getsample()` condition (`ix_<3 || (ix_<4 && !down_)`): indices
// 0-2 always auto-advance, index 3 only advances while released, matching
// this file's stage 3 (never advances while held) / stage 4 (entered only
// by fm_pitch_eg_release(), same "jump from wherever it is" convention as
// env_dx_release()) exactly.
enum PitchEgStage : uint8_t { PEG_STAGE_1, PEG_STAGE_2, PEG_STAGE_3, PEG_STAGE_4 };

struct FmPitchEg {
    float   current_cents;
    uint8_t stage;
};

// Matches env_dx_init()'s role: the power-on/never-triggered resting state.
// Harmless regardless of value since fm_pitch_eg_trigger() always runs
// before this is ever read for a real note -- but stage must still be a
// valid array index (0-3), same "must be explicit, zero looks plausible but
// isn't necessarily right" caution env_dx.h's own init comment gives.
inline void fm_pitch_eg_init(FmPitchEg &eg) {
    eg.current_cents = 0.0f;
    eg.stage = PEG_STAGE_4;
}

// Starts stage 1 from the voice's own L4 (release level) as the initial
// position -- matches Dexed's real `PitchEnv::set()` exactly
// (`level_ = pitchenv_tab[l[3]] << 19`), and is why FmPitchEgParams::level
// needs an explicit, correct L4 rather than an implicit silence floor: for
// pitch there IS no silence to fall back to, only "wherever the previous
// note's release left it" (approximated here, as on real hardware, by L4
// itself rather than tracking true inter-note continuity).
inline void fm_pitch_eg_trigger(FmPitchEg &eg, const FmPitchEgParams &p) {
    eg.current_cents = dx7_pitchenv_level_to_cents(p.level[PEG_STAGE_4]);
    eg.stage = PEG_STAGE_1;
}

// Jump directly to the release stage from wherever the EG currently is --
// matches env_dx_release()'s convention (and real Dexed: `keydown(false)`
// unconditionally re-targets L4/R4 regardless of current stage).
inline void fm_pitch_eg_release(FmPitchEg &eg) {
    eg.stage = PEG_STAGE_4;
}

// Advances by one control block of `n` samples, returns the cents value to
// use for this whole block (held constant across it -- the pitch-domain
// counterpart of EnvDX's per-sample gain_step interpolation, but without
// needing per-sample interpolation at all, since nothing here reaches the
// kernel). Same stage-transition shape as env_dx_step_block(): stage 1-2
// auto-advance on reaching their target, stage 3 holds forever until an
// explicit release, stage 4 (post-release) holds forever once its own
// target is reached.
inline float fm_pitch_eg_step_block(FmPitchEg &eg, const FmPitchEgParams &p, uint32_t n) {
    float target_cents = dx7_pitchenv_level_to_cents(p.level[eg.stage]);
    float rate_per_sec_cents = dx7_pitchenv_rate_to_octaves_per_sec(p.rate[eg.stage]) * 1200.0f;
    float step_cents = rate_per_sec_cents * (float)n / (float)SAMPLE_RATE;
    if (step_cents < 0.01f) step_cents = 0.01f;  // guarantee forward progress every block, matching env_dx_step_block's own "step < 1" floor

    float new_cents;
    bool reached;
    if (target_cents > eg.current_cents) {
        new_cents = eg.current_cents + step_cents;
        reached = new_cents >= target_cents;
    } else if (target_cents < eg.current_cents) {
        new_cents = eg.current_cents - step_cents;
        reached = new_cents <= target_cents;
    } else {
        new_cents = eg.current_cents;
        reached = true;
    }

    if (reached) {
        new_cents = target_cents;
        if (eg.stage == PEG_STAGE_1 || eg.stage == PEG_STAGE_2) {
            eg.stage = (uint8_t)(eg.stage + 1);  // 1->2 or 2->3; stage 3 holds until release, stage 4 holds forever
        }
    }

    eg.current_cents = new_cents;
    return new_cents;
}
