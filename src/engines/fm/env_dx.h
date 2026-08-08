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
// Log-domain attenuation, via the DX7's nonlinear level table"). ~96 dB
// across levels 1-99, genuinely nonlinear in the LINEAR amplitude this
// eventually becomes via eg_to_linear's exp2 conversion -- not
// `gain = reference * level/99`, which is what "a linear approximation" in
// the acceptance criteria means and rejects. This is an honest
// approximation of the real DX7 curve's general shape, not a byte-exact
// reproduction of Yamaha's hardware table -- exact replication is P3/P6
// territory, once real .syx patches exist to compare against Dexed
// (fm.md §7's "fail loudly" converter, §11's calibration pass).
//
// The curve is QUADRATIC in dB, not uniform-per-unit (env_dx_init_level_table
// below): flatter near 99, steeper near 0 -- `db(level) = -(99-level)^2 *
// EG_LEVEL_DB_RANGE/99^2`. First cut used a uniform ~1 dB/unit, which put a
// typical "sustain" value like 60-70 about 30-38 dB below the attack peak --
// heard on real hardware (#45, Carl) as a loud attack that decays to
// near-nothing, not a held tone ("more like a marimba. Very weak sustain.").
// The quadratic shape keeps levels 70-99 within roughly 8 dB of the peak
// (audibly present, a real sustain) while still reaching the same -96 dB
// floor by level 0 -- closer to how real DX7 patches actually use 60-85 as
// an audible-but-softer sustain value, not a near-silent one.
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
// BLOCK-confirmation acceptance criterion). Exponential across the 0-99
// range, same "each unit roughly compounds" shape real DX7 rate parameters
// have: rate 0 sweeps EG_FLOOR_OCTAVES worth of range in ~20s (a "molasses"
// pad-style attack), rate 99 in ~6ms (DX7's documented near-instantaneous
// fastest segments). Approximate, not hardware-measured -- see the level
// table comment; what this issue's BLOCK-confirmation criterion actually
// needs is SOME fast segment whose transient is short enough to be
// BLOCK-size-sensitive, and rate 99 here comfortably is one (6ms is 8-17
// control blocks at BLOCK 8-16, so BLOCK's own granularity is a real
// fraction of the segment, not lost in the noise).
static constexpr float EG_RATE_SWEEP_OCTAVES = -(float)EG_FLOOR_OCTAVES;  // 40
static constexpr float EG_RATE_MIN_SECONDS = 20.0f;   // rate 0
static constexpr float EG_RATE_MAX_SECONDS = 0.006f;  // rate 99

static constexpr uint32_t DX7_LEVEL_COUNT = 100;
inline int32_t DX7_LEVEL_TO_LOG2[DX7_LEVEL_COUNT];   // fixed-point octave offset, <= 0, index 99 == 0

static constexpr uint32_t DX7_RATE_COUNT = 100;
inline int32_t DX7_RATE_TO_STEP[DX7_RATE_COUNT];     // fixed-point octaves/second, index r >= index r-1

static constexpr uint32_t EG_EXP2_BITS = EG_LOG2_FRAC_BITS;
static constexpr uint32_t EG_EXP2_SIZE = 1u << EG_EXP2_BITS;  // 256
inline uint16_t eg_exp2_table[EG_EXP2_SIZE];  // Q15: 2^(i/256), range [32768, 65280]

inline void env_dx_init_level_table() {
    DX7_LEVEL_TO_LOG2[0] = EG_LOG2_FLOOR;
    static constexpr float DB_PER_UNIT_SQ = EG_LEVEL_DB_RANGE / (99.0f * 99.0f);
    for (uint32_t level = 1; level < DX7_LEVEL_COUNT; level++) {
        float below = 99.0f - (float)level;               // 0 at level 99, 99 at level 0
        float db = -(below * below) * DB_PER_UNIT_SQ;       // quadratic: flatter near 99, steeper near 0
        float octaves = db / EG_DB_PER_OCTAVE;
        DX7_LEVEL_TO_LOG2[level] = (int32_t)(octaves * (float)EG_LOG2_ONE);
    }
}

inline void env_dx_init_rate_table() {
    for (uint32_t rate = 0; rate < DX7_RATE_COUNT; rate++) {
        float t = (float)rate / (float)(DX7_RATE_COUNT - 1);  // 0..1
        // Exponential interpolation between the slowest and fastest sweep
        // times, then convert "seconds for the full sweep" to "octaves per
        // second" by dividing the swept range by that time.
        float seconds = EG_RATE_MIN_SECONDS * powf(EG_RATE_MAX_SECONDS / EG_RATE_MIN_SECONDS, t);
        float octaves_per_sec = EG_RATE_SWEEP_OCTAVES / seconds;
        DX7_RATE_TO_STEP[rate] = (int32_t)(octaves_per_sec * (float)EG_LOG2_ONE);
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
// convention as envelope.h's advance_block().
inline void env_dx_step_block(EnvDX &eg, const FmOpParams &op, uint32_t n,
                               int32_t &log2_start, int32_t &log2_end) {
    log2_start = eg.current_log2;
    if (eg.stage == EG_IDLE) {
        log2_end = log2_start;
        return;
    }

    int32_t target = DX7_LEVEL_TO_LOG2[op.eg_level[eg.stage]];
    int32_t rate_per_sec = DX7_RATE_TO_STEP[op.eg_rate[eg.stage]];
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
