#pragma once

#include "audio_common.h"
#include "patch.h"
#include <cmath>
#include <cstdint>

// EnvDX -- the DX7 envelope (#45, fm.md §5.3): 4 x (rate, level) pairs per
// operator, log-domain, stepped once per control block. Deliberately NOT
// envelope.h's ADSR (fm.md §5.3: "Do not reuse envelope.* ... An ADSR bent
// into that shape would be both slower and less accurate") -- DX7 stages
// are direction-agnostic (any stage can ramp up OR down to any target,
// unlike ADSR's fixed attack-up/decay-down/release-down shape) and the
// hardware itself is an adder in log domain plus one exponential lookup,
// not a per-sample multiply.
//
// Everything below works in a single fixed-point "log2 offset from
// reference" domain: 0 = the operator's full reference level (patch.h's
// FmOpParams::level, i.e. what op_render's `gain` would be at 100% output
// level, 100% EG level, and velocity-sensitivity-neutral), more negative =
// quieter. Three independent 0-99 DX7 parameters -- operator output level
// (TL), each EG stage's target level, and velocity sensitivity's effect --
// all resolve to offsets in this SAME domain and simply ADD (fm.md §5.6:
// "none of it belongs in the render loop" -- all of this runs at note-on or
// block-rate, never per sample). The per-sample kernel (op.h's
// op_render/op_render_first/op_render_fb) is completely unchanged by this
// file: it still just does `gain += gain_step` -- this file only decides
// what `gain`/`gain_step` are handed at each block boundary.

// One octave = EG_LOG2_ONE fixed-point units (Q iiii.8: 8 fractional bits,
// chosen to match the 256-entry exp2 table 1:1 -- no interpolation needed
// for the fractional lookup).
static constexpr int32_t EG_LOG2_FRAC_BITS = 8;
static constexpr int32_t EG_LOG2_ONE = 1 << EG_LOG2_FRAC_BITS;  // 256

// Level-parameter (0-99) dynamic range, applied to BOTH the operator output
// level (TL) and each EG stage's target level -- real DX7 hardware shares
// one curve between the two (fm.md §7: "Operator output level (0-99) ->
// Log-domain attenuation, via the DX7's nonlinear level table").
// ~96 dB across levels 1-99, genuinely nonlinear -- not
// `gain = reference * level/99`, which is what "a linear approximation" in
// the acceptance criteria means and rejects.
//
// History: #45's first cut used a uniform ~1 dB/unit, which put a typical
// "sustain" value like 60-70 about 30-38 dB below the attack peak -- heard
// on real hardware as a loud attack decaying to near-nothing, not a held
// tone ("more like a marimba. Very weak sustain."). #45 then tried a
// hand-fit quadratic-in-dB curve (flatter near 99, steeper near 0), which
// fixed that complaint but was still considerably flatter than the real
// DX7 curve across most of the range (#58 measured -0.8 dB at level 90 here
// vs the genuine -6.8 dB, by porting and running Dexed's actual Env/Exp2
// pipeline rather than re-deriving from a formula guess) -- real enough to
// let #57's newly-unlocked modulation depth run hot on anything short of a
// near-maxed operator, surfacing as "sometimes overdriven." #58 replaced
// the hand-fit curve with `dx7_scaleoutlevel()` (below), a direct port of
// Dexed's own `Env::scaleoutlevel()`, closing that gap for real instead of
// re-fitting another approximation.
static constexpr float EG_LEVEL_DB_RANGE = 96.0f;
static constexpr float EG_DB_PER_OCTAVE = 6.0206f;  // 20*log10(2)

// Level 0 is a dedicated, much deeper floor than level 1's ~-95 dB --
// real DX7 hardware treats level 0 as "off", not merely "very quiet".
// Deep enough that eg_to_linear() underflows to an exact int32 zero for
// ANY valid reference (references are always < 2^31; 2^31 * 2^-40 < 1),
// which is what makes "voice reports itself free" a real guarantee instead
// of an epsilon guess -- the tracker's #21 bug ("key-off never frees a
// voice") was exactly a missing version of this guarantee.
static constexpr int32_t EG_FLOOR_OCTAVES = -40;
static constexpr int32_t EG_LOG2_FLOOR = EG_FLOOR_OCTAVES * EG_LOG2_ONE;

// eg_to_linear()'s fast-path threshold: comfortably below EG_FLOOR_OCTAVES
// so the EG's own level-0 target always takes the fast (exact zero) path,
// but shallow enough that it also catches "EG floor plus a few octaves of
// velocity-sensitivity offset" without needing every caller to reason about
// the combination. Below this, the shift-based conversion would underflow
// to 0 anyway (see eg_to_linear's comment) -- this is purely a clarity/
// cheap-exit optimization, not a separate correctness mechanism.
static constexpr int32_t EG_SILENCE_THRESHOLD_OCTAVES = -32;
static constexpr int32_t EG_LOG2_SILENT = EG_SILENCE_THRESHOLD_OCTAVES * EG_LOG2_ONE;

// Velocity sensitivity (0-7, fm.md §5.6): unlike output level/EG level,
// this only ever ATTENUATES (max velocity = 0 offset regardless of
// sensitivity; softer hits are progressively quieter, scaled by
// sensitivity) -- never boosts past the operator's configured reference,
// so it composes with the level offsets above via plain addition without
// ever needing to reason about exceeding 0 dB. Sensitivity 0 is exactly
// "no velocity effect" (#44's pre-#45 behavior, now the correct DX7
// default rather than a placeholder). ~24 dB (4 octaves) of range at
// sensitivity 7 hitting velocity 0 -- audible, in a plausible DX7-ish
// range; not a claim of exact hardware fidelity (see the level-table
// comment above).
static constexpr float EG_VEL_SENS_MAX_OCTAVES = 4.0f;

// Rate-parameter (0-99) dynamic range: fixed-point OCTAVES PER SECOND,
// independent of block size (BLOCK only decides how finely that per-second
// rate gets sampled -- see op.h's FM_BLOCK / T00T_FM_BLOCK and this issue's
// BLOCK-confirmation acceptance criterion).
//
// History: originally a smooth single exponential across 0-99 (rate 0 ~20s,
// rate 99 ~6ms for a full EG_FLOOR_OCTAVES sweep) -- an "approximate" guess,
// like #45's original level curve. #59 found it badly wrong by the same
// method #58 used for the level curve: porting and running Dexed's actual
// `Env::advance()`/`getsample()` rate-to-speed formula (Source/msfa/env.cc)
// rather than re-deriving from a shape guess. Real DX7 rate isn't a smooth
// exponential -- it's piecewise, built from a `qrate` value and a
// `(4+(qrate&3)) << (2+6+(qrate>>2))` bit-shift step -- and it accelerates
// far more steeply toward the high end than any single smooth exponential
// can: 1.35x faster than this curve at rate 0, growing to ~23x faster at
// rate 99. That gap is a direct, well-evidenced explanation for "envelopes
// feel sluggish" / "noticeable delay from note-on until audible" (Carl,
// after #58) -- R1=99 (an extremely common "instant attack" choice in real
// patches, including FM_TEST_PATCH) was landing roughly 20x slower than
// real hardware. `dx7_rate_to_octaves_per_sec()` below replaces the
// exponential fit with the real formula; `EG_RATE_SWEEP_OCTAVES` stays as
// this file's own floor-to-unity range definition (unrelated to the fix).
static constexpr float EG_RATE_SWEEP_OCTAVES = -(float)EG_FLOOR_OCTAVES;  // 40

static constexpr uint32_t DX7_LEVEL_COUNT = 100;
inline int32_t DX7_LEVEL_TO_LOG2[DX7_LEVEL_COUNT];   // fixed-point octave offset, <= 0, index 99 == 0

static constexpr uint32_t DX7_RATE_COUNT = 100;
inline int32_t DX7_RATE_TO_STEP[DX7_RATE_COUNT];     // fixed-point octaves/second, index r >= index r-1

static constexpr uint32_t EG_EXP2_BITS = EG_LOG2_FRAC_BITS;
static constexpr uint32_t EG_EXP2_SIZE = 1u << EG_EXP2_BITS;  // 256
inline uint16_t eg_exp2_table[EG_EXP2_SIZE];  // Q15: 2^(i/256), range [32768, 65280]

// Dexed's own `Env::scaleoutlevel()` (Source/msfa/env.cc, Apache-2.0) --
// ported verbatim, not re-derived, since #58 found the earlier "honest
// approximation" (quadratic-in-dB, above) was still considerably flatter
// than the real curve across most of the 0-99 range (e.g. -0.8 dB at
// level 90 here vs the real -6.8 dB; -8.2 dB at level 70 here vs -21.9 dB
// real), letting anything short of a near-maxed operator run noticeably
// hotter than authentic DX7 behavior -- the actual root cause behind #57's
// "sometimes overdriven" report, not modulation depth itself. Levels 0-19
// are a measured, non-formulaic table on real hardware; 20-99 is `28 + level`.
inline int dx7_scaleoutlevel(int level) {
    static constexpr int levellut[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29,
                                          31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
    return level >= 20 ? 28 + level : levellut[level];
}

// #58: replaces the #45 quadratic-in-dB approximation with Dexed's actual
// curve, ported exactly rather than re-fit. Verified by direct port-and-run
// against Dexed's real Exp2/Env pipeline (not hand-derived): one unit of
// `dx7_scaleoutlevel(level) * 32` is exactly 1/256 octave in Dexed's own
// fixed-point convention -- the same Q iiii.8 scale this file already uses
// (EG_LOG2_ONE), so the conversion is a direct subtraction, no rescaling.
// Real DX7 hardware shares this exact curve between operator output level
// (TL) and each EG stage's target level (fm.md §7) -- Dexed's own formula
// for the two differs only by an integer-truncation rounding difference
// this table doesn't distinguish (both go through the same
// scaleoutlevel()*32 shape), matching the "one shared curve" convention
// this file already documents above.
inline void env_dx_init_level_table() {
    DX7_LEVEL_TO_LOG2[0] = EG_LOG2_FLOOR;  // deliberately deeper than real hardware's ~-96 dB -- see this constant's own comment (exact-zero voice-free guarantee, not a fidelity claim)
    const int32_t reference = dx7_scaleoutlevel(99) * 32;  // = 4064
    for (uint32_t level = 1; level < DX7_LEVEL_COUNT; level++) {
        DX7_LEVEL_TO_LOG2[level] = (dx7_scaleoutlevel((int)level) * 32) - reference;
    }
}

// Dexed's own `Env::advance()` rate derivation (Source/msfa/env.cc,
// Apache-2.0), ported verbatim (#59), split into its two halves so #48's
// rate scaling can add a qrate delta between them (real DX7 hardware adds
// keyboard rate scaling directly to qrate, once per stage transition, not
// as a separate offset applied after the octaves/second conversion --
// `Env::advance()`'s own `qrate += rate_scaling_`).
//
// A second real bug, found by building Dexed's actual env.cc/exp2.cc/
// fm_op_kernel.cc standalone (not re-derived) and A/B-rendering a real
// ROM1A patch (E.PIANO 1) against this engine: `inc_` is NOT a Q24
// octaves/SAMPLE delta, despite what this comment used to say -- it's a Q24
// octaves-per-*Dexed-render-call* delta, and one Dexed render call
// (`Dx7Note::compute()`, via `FmCore::render()`) always advances exactly
// `N = 1 << LG_N = 64` samples (synth.h). `Env::getsample()` -- the only
// thing that ever applies `inc_` to `level_` -- is called exactly once per
// that 64-sample call, not once per sample (confirmed by reading
// `Dx7Note::compute()`'s op loop: one `env_[op].getsample()` per call, and
// each call is handed a fixed N=64-sample output buffer to fill). The old
// conversion (`* SAMPLE_RATE >> 16`) implicitly assumed a per-sample delta
// (block size 1), so it came out exactly `N` = 64x too fast: every stage of
// every envelope -- attacks, decays, releases -- ran two orders of
// magnitude quicker than real DX7 hardware. Measured effect, not
// theoretical: the buggy formula puts a real rate-25 stage (E.PIANO 1's own
// carrier decay, ROM1A) at a 372ms full-floor sweep; the standalone Dexed
// build of the *same patch* is still clearly sounding, mid-decay, past
// 2000ms held. A rate-99 attack goes from an implausible ~0.1ms (under 5
// samples -- not even one control block) to ~6.6ms, which matches commonly
// cited real DX7 "instant attack" timing. This is almost certainly why
// patches kept sounding "thin and dull" after #57/#58/#59's other fixes:
// with every envelope stage compressed 64x, a patch's bright attack
// transient collapses into a fraction of a control block (often literally
// unresolvable at BLOCK=16) and whatever should have been a multi-second
// natural decay/evolution (this E.PIANO's own long, mellowing decay being
// the textbook case) instead reaches near-silence almost immediately --
// modulation depth and level curve were never the problem for those
// patches, timing was.
//
// Fixed by adding the missing `>> LG_N` (LG_N=6, Dexed's own per-block
// exponent, already baked into `inc`'s own shift above and named
// explicitly here so the two `6`s are visibly the same constant): converts
// "Q24 octaves per Dexed-internal 64-sample block" into "Q24 octaves per
// second" by dividing by that block's real-time duration
// (SAMPLE_RATE/N seconds⁻¹), before the existing Q24->Q iiii.8
// (EG_LOG2_ONE) rescale. t00t's own BLOCK (FM_BLOCK/T00T_FM_BLOCK, op.h) is
// a separate, independent choice (16, not 64) for *this* engine's own
// control-rate stepping granularity -- LG_N here is Dexed's internal
// constant being ported, not a reference to that.
inline int dx7_rate_to_qrate(int rate) {
    int qrate = (rate * 41) >> 6;
    return qrate > 63 ? 63 : qrate;
}

inline int32_t dx7_qrate_to_octaves_per_sec_q8(int qrate) {
    if (qrate < 0) qrate = 0;
    if (qrate > 63) qrate = 63;
    constexpr int32_t DEXED_LG_N = 6;  // Dexed's own per-block sample count, 1<<6 = 64 (synth.h's LG_N)
    int64_t inc = (int64_t)(4 + (qrate & 3)) << (2 + DEXED_LG_N + (qrate >> 2));
    return (int32_t)((inc * (int64_t)SAMPLE_RATE) >> (16 + DEXED_LG_N));
}

inline int32_t dx7_rate_to_octaves_per_sec_q8(int rate) {
    return dx7_qrate_to_octaves_per_sec_q8(dx7_rate_to_qrate(rate));
}

// Dexed's `ScaleRate()` (dx7note.cc, Apache-2.0), ported verbatim (#48): a
// qrate DELTA from how far above a low reference note the played note is
// (`x`, clamped 0-31 -- roughly the octave-and-a-bit starting around A1),
// scaled by the operator's own rate scaling (RS, 0-7). Real DX7: higher
// notes get faster envelopes -- why bells/plucked patches tighten up
// correctly across the keyboard instead of the same shape at every pitch.
// Resolved once at note-on (op.h's fm_voice_note_on) into `FmOp`'s own
// `rate_scale_qrate`, then added to `dx7_rate_to_qrate(rate)` at every
// stage transition (env_dx_step_block) -- matching Dexed's own "added to
// qrate, not to the final speed" order exactly.
inline int32_t dx7_scale_rate(int midinote, int sensitivity) {
    int x = midinote / 3 - 7;
    if (x < 0) x = 0;
    if (x > 31) x = 31;
    return (sensitivity * x) >> 3;
}

// Dexed's `ScaleCurve()`/`ScaleLevel()` (dx7note.cc, Apache-2.0), ported
// verbatim (#48). Same raw-unit scale as `dx7_scaleoutlevel()` above (both
// feed the identical pre-`<<5` "outlevel" pipeline on real hardware) --
// `fm_voice_note_on()` multiplies the result by 32 to fold it into this
// file's octave (`EG_LOG2_ONE`) convention, same as
// `env_dx_init_level_table()` does for TL. Curve values match DX7: 0=-LIN,
// 1=-EXP, 2=+EXP, 3=+LIN.
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

inline void env_dx_init_rate_table() {
    for (uint32_t rate = 0; rate < DX7_RATE_COUNT; rate++) {
        DX7_RATE_TO_STEP[rate] = dx7_rate_to_octaves_per_sec_q8((int)rate);
    }
}

inline void env_dx_init_exp2_table() {
    for (uint32_t i = 0; i < EG_EXP2_SIZE; i++) {
        float frac = (float)i / (float)EG_EXP2_SIZE;
        eg_exp2_table[i] = (uint16_t)(exp2f(frac) * 32768.0f);
    }
}

inline void env_dx_init_tables() {
    env_dx_init_level_table();
    env_dx_init_rate_table();
    env_dx_init_exp2_table();
}

// Converts a fixed-point log2 offset (<= 0, in EG_LOG2_ONE units) plus a
// raw linear `reference` (patch.h's FmOpParams::level) into the same linear
// gain scale op.h's kernels consume. Below EG_SILENCE_THRESHOLD_OCTAVES,
// returns an exact 0 -- see that constant's comment for why this is a real
// guarantee, not an approximation, for any valid int32 reference.
inline int32_t eg_to_linear(int32_t reference, int32_t log2_fp) {
    if (log2_fp <= EG_LOG2_SILENT) return 0;
    int32_t oct = log2_fp >> EG_LOG2_FRAC_BITS;             // floor (arithmetic shift, two's complement)
    uint32_t frac = (uint32_t)log2_fp & (uint32_t)(EG_LOG2_ONE - 1);
    uint32_t mult_q15 = eg_exp2_table[frac];
    int64_t g = (int64_t)reference * (int64_t)mult_q15;
    int32_t shift = (EG_LOG2_FRAC_BITS + 7) - oct;  // 15 - oct; oct <= 0 here, so shift >= 15
    if (shift >= 63) return 0;
    return (int32_t)(g >> shift);
}

// Velocity sensitivity -> log2 offset (see EG_VEL_SENS_MAX_OCTAVES's
// comment): 0 at sensitivity 0 (regardless of velocity) or at max velocity
// (regardless of sensitivity), increasingly negative for softer hits on a
// more sensitive operator. `amplitude` is VoiceParams' existing Q15
// velocity (0-32767).
inline int32_t eg_vel_sensitivity_log2(uint8_t sensitivity, int16_t amplitude) {
    if (sensitivity == 0) return 0;
    float vel_frac = (float)amplitude / 32767.0f;
    float octaves = -((float)sensitivity / 7.0f) * EG_VEL_SENS_MAX_OCTAVES * (1.0f - vel_frac);
    return (int32_t)(octaves * (float)EG_LOG2_ONE);
}

// EG_STAGE_1..4 index directly into FmOpParams::eg_rate/eg_level (0-3).
// EG_IDLE is the terminal state, reached only by completing stage 4 (i.e.
// only after an explicit note-off) -- envelope.h's Envelope::active()
// equivalent, and the only thing fm_voice_active() (op.h) trusts to decide
// whether a voice may be reported free.
enum EgStage : uint8_t { EG_STAGE_1, EG_STAGE_2, EG_STAGE_3, EG_STAGE_4, EG_IDLE };

struct EnvDX {
    int32_t current_log2;  // this operator's own dynamic offset, <= 0
    uint8_t stage;

    bool active() const { return stage != EG_IDLE; }
};

// Matches envelope.h's Envelope::init(): the power-on/never-triggered
// state. Zero-initializing an EnvDX (e.g. a fresh static array) is NOT
// equivalent to this -- stage 0 is EG_STAGE_1, not EG_IDLE, since stages
// 1-4 need to double as direct eg_rate[]/eg_level[] indices (0-3). Every
// voice slot's EGs must call this explicitly at boot, same as
// envelope[v].init() in the subtractive engine.
inline void env_dx_init(EnvDX &eg) {
    eg.current_log2 = EG_LOG2_FLOOR;
    eg.stage = EG_IDLE;
}

// Start stage 1 from silence -- matches envelope.h's Envelope::trigger()
// convention (a fresh note-on always restarts from zero, even mid-release).
inline void env_dx_trigger(EnvDX &eg) {
    eg.current_log2 = EG_LOG2_FLOOR;
    eg.stage = EG_STAGE_1;
}

// Jump directly to the release stage from wherever the EG currently is --
// matches envelope.h's Envelope::release() ("transitions to RELEASE using
// the current level as the starting point"). A no-op if already idle.
inline void env_dx_release(EnvDX &eg) {
    if (eg.stage != EG_IDLE) eg.stage = EG_STAGE_4;
}

// Advances `eg` by one control block of `n` samples, returning the log2
// offset at the start and end of the block (the caller converts both to
// linear via eg_to_linear() and interpolates gain_step across the block --
// op.h's fm_voice_step_envelopes()). A stage transition that would land
// mid-block is deferred to the next block boundary (clamped exactly at the
// target for this block instead) -- same "bounded by one block, inaudible"
// convention as envelope.h's advance_block(). `rate_qrate_delta` is #48's
// key rate scaling, resolved once at note-on (FmOp::rate_scale_qrate) and
// added directly to this stage's own qrate every time a stage starts --
// matching Dexed's `qrate += rate_scaling_` exactly, rather than scaling
// the already-converted octaves/second value (a different, less faithful
// order of operations). 0 for every existing caller/patch until #48 wires
// real DX7 data through -- identical to #59's own DX7_RATE_TO_STEP[] table
// lookup in that case (dx7_rate_to_octaves_per_sec_q8() IS
// dx7_qrate_to_octaves_per_sec_q8(dx7_rate_to_qrate(rate) + 0)), so this
// change is behavior-neutral for every patch that doesn't use rate scaling.
inline void env_dx_step_block(EnvDX &eg, const FmOpParams &op, uint32_t n,
                               int32_t &log2_start, int32_t &log2_end,
                               int32_t rate_qrate_delta = 0) {
    log2_start = eg.current_log2;
    if (eg.stage == EG_IDLE) {
        log2_end = log2_start;
        return;
    }

    int32_t target = DX7_LEVEL_TO_LOG2[op.eg_level[eg.stage]];
    int32_t rate_per_sec = rate_qrate_delta == 0
        ? DX7_RATE_TO_STEP[op.eg_rate[eg.stage]]
        : dx7_qrate_to_octaves_per_sec_q8(dx7_rate_to_qrate(op.eg_rate[eg.stage]) + rate_qrate_delta);
    int32_t step = (int32_t)(((int64_t)rate_per_sec * n) / SAMPLE_RATE);
    if (step < 1) step = 1;  // guarantee forward progress every block, even at rate 0 with a short final sub-block

    int32_t new_log2;
    bool reached;
    if (target > log2_start) {
        new_log2 = log2_start + step;
        reached = new_log2 >= target;
    } else if (target < log2_start) {
        new_log2 = log2_start - step;
        reached = new_log2 <= target;
    } else {
        new_log2 = log2_start;
        reached = true;
    }

    if (reached) {
        new_log2 = target;
        if (eg.stage == EG_STAGE_4) {
            eg.stage = EG_IDLE;          // release complete -- the only way to reach EG_IDLE
        } else if (eg.stage != EG_STAGE_3) {
            eg.stage = (uint8_t)(eg.stage + 1);  // 1->2 or 2->3; stage 3 holds at its target until note-off
        }
    }

    eg.current_log2 = new_log2;
    log2_end = new_log2;
}
