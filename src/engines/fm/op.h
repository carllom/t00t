#pragma once

#include "env_dx.h"
#include "lfo.h"
#include "pan.h"
#include "patch.h"
#include "pitch_eg.h"
#include "sine_tab.h"
#include <cmath>
#include <cstdint>

#if defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#endif

// FM operator kernel (#44, fm.md §5.2): FmOp + the three per-sample
// variants, plus the note-on/block/voice glue that turns a resolved
// FmRouting (patch.h) into real audio. Per-sample bodies are the exact
// #42/#43-measured kernels (rig.h's op_render/op_render_first/op_render_fb)
// -- same table (sine_tab.h's real, hardware-verified 4096-entry table, not
// rig.h's standalone bench copy), same fm_mul_gain convention, same
// phase-wrap-is-free indexing. F2 (fm2.md §2) removed the per-call output
// shift entirely -- there is one output scale now, not a carrier one and a
// modulator one -- which makes the loop bodies slightly *shorter* than the
// #43-measured ones, not longer, so that measurement still bounds them. See
// "The fixed-point contract" below for the single anchor everything now
// derives from. fm.md §3.6's decisions are otherwise still
// baked in: SMULWB adopted (lever 4, "adopted where convenient"), plain
// `inline` in flash, not not_in_flash_func (lever 2, SRAM measured worse),
// no op-pair interleaving (lever 1, not adopted). No pico-sdk dependency
// beyond the conditional arm_acle.h include (device only), so this header
// is shared by the device engine and the host render/test harness, exactly
// like render.h/rig.h.

// fm.md §3.6 lever 3 / open question 3, closed by #45: BLOCK size. #43
// deferred the final call to here, since #43's rig has no EG/LFO to give
// the time-resolution side of the tradeoff anything real to measure against
// (fm.md: "confirm empirically against the fastest-attack patches"). See
// engine.md "FM P2 BLOCK Confirmation (#45)" for the comparison and the
// decision. Overridable at compile time (`-DT00T_FM_BLOCK=8/32`, wired
// through CMakeLists.txt/Makefile the same way DMA_BUFFER_SIZE is) so that
// comparison -- and any future one -- doesn't require hand-editing this
// file. Also the shared per-voice bus scratch size (fm.md §4.3).
#ifndef T00T_FM_BLOCK
#define T00T_FM_BLOCK 16
#endif
static constexpr uint32_t FM_BLOCK = T00T_FM_BLOCK;

struct FmOp {
    uint32_t phase;
    uint32_t inc;
    int32_t  gain;
    int32_t  gain_step;    // per-sample delta for this block, from EnvDX (#45, env_dx.h)
    const int32_t *in;     // modulation bus, or fm_zero_bus for an unmodulated operator
    int32_t *out;           // modulation bus, or the shared voice output bus
    int32_t  fb1, fb2;      // op_render_fb only: last two post-gain outputs (see op_render_fb's own comment)
    EnvDX    eg;            // #45: this operator's own 4-stage envelope
    int32_t  static_log2;   // #45/#48: output level + velocity sensitivity + key level scaling, resolved once at note-on
    int32_t  rate_scale_qrate; // #48: key rate scaling, resolved once at note-on, added to every stage's qrate (env_dx.h's env_dx_step_block)
};

// Read-only all-zero bus, shared by every operator nothing modulates.
inline int32_t fm_zero_bus[FM_BLOCK];

// Multiply-by-gain-then-shift. fm.md §3.6 lever 4: SMULWB fuses the
// gain-scale + multiply into one M33 DSP-extension instruction --
// (Rn * SignExtend16(Rm)) >> 16 -- measured -3.0% in #43, "adopted where
// convenient" (real win, no correctness cost), so unlike rig.h's
// FM_RIG_SMULWB toggle (a bench A/B lever) this is just how the real
// kernel works. Host builds get the portable 64-bit-multiply equivalent --
// same numeric result, not the real instruction (arm_acle.h doesn't exist
// off-target). __smlawb (multiply-accumulate, third operand forced to 0)
// stands in for SMULWB because this GCC's arm_acle.h has no standalone
// SMULWB wrapper -- same rig.h finding.
inline int32_t fm_mul_gain(int32_t sample, int32_t gain) {
#if defined(__ARM_FEATURE_DSP)
    return __smlawb(gain, sample, 0);
#else
    return (int32_t)(((int64_t)gain * (int16_t)sample) >> 16);
#endif
}

// ===========================================================================
// The fixed-point contract (F2, fm2.md §2/§5).
//
// ONE anchor, from which every other number here is derived:
//
//     an operator's output, in `out[]` units, is phase deviation --
//     FM_CYCLE units == one full cycle (2*pi) on whatever it modulates.
//
// That is Dexed's own contract, measured rather than assumed (tools/fm_ref;
// fm2.md §5.6): `Sin::lookup` is full-scale 2^24, its maximum operator gain
// is exactly 2.0, and 24 bits is one cycle of its phase -- so a max-level
// Dexed operator peaks at exactly TWO full cycles of deviation, and a
// *unity*-gain one at exactly one. Carriers are not a special case there and
// are not one here: a carrier's output is the same number, reinterpreted as
// audio instead of as phase. Everything else the DX7 does -- output level,
// EG level, key scaling, velocity -- is attenuation in the log domain
// beneath this single ceiling.
//
// This replaces six mutually-cancelling constants (FM_OUT_SHIFT_CARRIER,
// FM_OUT_SHIFT_MODULATOR, FM_MOD_INPUT_SHIFT, FmOpParams::level, and
// syx2patch.py's FM_CARRIER_LEVEL_REF/FM_MODULATOR_LEVEL_REF), each of which
// existed to compensate for another. fm2.md §5.1 measured what that cost:
// per-patch level errors from -12 dB to -96 dB and brightness from 0.30x to
// 6.31x on the same build, because with no shared anchor every patch landed
// somewhere different.
//
// Do not "tune" anything in this block. If a patch is too loud or too dim,
// the answer is in its output level, its EG, or its key scaling -- exactly as
// it would be on real hardware. That discipline is the entire point.
// ===========================================================================

// One full cycle of phase deviation, in bus units. 2^26 leaves 5 bits of
// headroom over the 2^27 maximum a single operator can emit (see
// FM_GAIN_MAX), which covers both fan-in (several modulators summing onto one
// bus) and several carriers summing into the output bus, without ever
// approaching int32 overflow -- the failure mode fm2.md §1.1(a) found in the
// old scaling, where a max-level modulator reached 2^33.4 and wrapped the
// phase accumulator several times per sample.
static constexpr uint32_t FM_CYCLE_BITS = 26;
static constexpr int32_t  FM_CYCLE = 1 << FM_CYCLE_BITS;

// Bus units -> the 32-bit phase accumulator (2^32 == one cycle). Unsigned
// wraparound past a full cycle is correct by construction, not a lucky
// accident: that is exactly what a phase accumulator is for.
static constexpr uint32_t FM_MOD_SHIFT = 32 - FM_CYCLE_BITS;

// The gain at log2 offset 0 -- i.e. output level 99, EG at level 99, no key
// scaling, no velocity attenuation. Derived, not chosen: fm_mul_gain() does
// (gain * sample) >> 16 with the sine table full-scale at 2^15, and the
// contract says that product must be 2 * FM_CYCLE at maximum level (Dexed's
// measured 2.0 gain ceiling), so FM_GAIN_MAX = 2^(1 + 26 + 16 - 15) = 2^28.
// env_dx.h's eg_to_linear() returns exactly this for a log2 offset of 0.
static constexpr int32_t FM_GAIN_MAX = 1 << 28;

// Voice output bus -> int16 audio, applied once in fm_render_voice(). A
// single max-level carrier lands at 2^14, i.e. half of int16 full scale, so
// a two-carrier patch at full tilt reaches 0 dBFS and busier algorithms rely
// on the mixer's existing saturation -- the same bargain the hardware makes.
// This is the one master headroom choice in the engine; it is a property of
// how loud a *voice* should be, not of any operator, and nothing else in the
// signal path is allowed to have an opinion about level.
static constexpr int32_t FM_VOICE_OUT_SHIFT = FM_CYCLE_BITS - 12;

// Plain kernel: accumulates (+=) into `out`. fm.md §3.2's 13-instruction
// listing (as measured by #43) is still this loop body -- F2 removed the
// per-call `out_shift` parameter (there is one output scale now, not a
// carrier one and a modulator one) and left the shape otherwise untouched,
// so #43's measured cost still stands.
inline void op_render(FmOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    const int32_t *in = op.in;
    int32_t *out = op.out;
    for (uint32_t i = 0; i < n; i++) {
        phase += inc;
        // Phase wrap is free: the shift alone produces exactly a
        // FM_TABLE_BITS-wide index, no mask, no modulo (fm.md §3.2).
        uint32_t idx = (phase + ((uint32_t)in[i] << FM_MOD_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain);
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// First-writer variant: stores instead of accumulating, so its target bus
// never needs clearing (fm.md §4.3/§5.2).
inline void op_render_first(FmOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    const int32_t *in = op.in;
    int32_t *out = op.out;
    for (uint32_t i = 0; i < n; i++) {
        phase += inc;
        uint32_t idx = (phase + ((uint32_t)in[i] << FM_MOD_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] = fm_mul_gain(sample, gain);
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// Self-feedback: the modulation input is this operator's own last two
// outputs, averaged and attenuated by `fb_shift` (patch.h's FmRouting,
// resolved at note-on from FmOpParams::feedback_level, DX7 units 0-7),
// instead of an external bus (DX7-style). Always accumulates (+=) --
// patch.h's routing compiler guarantees this is safe by either scheduling a
// non-feedback first-writer ahead of it on a shared bus, or (clear_bus_mask)
// pre-zeroing a bus whose only writer is this op.
//
// Two real bugs fixed here, found by comparing against Dexed's actual
// `compute_fb`/`fb_buf` (Source/msfa/fm_op_kernel.cc, Apache-2.0) after #57's
// shift fixes still left real ROM1A patches "soft" on hardware:
//
// 1. `fb_shift` used to not exist at all -- self-feedback was no-op-or-full
//    (a bool), always at the single fixed depth this kernel happened to
//    produce. Real DX7 hardware (dx7note.cc: `fb_shift_ = feedback ? 8 -
//    feedback : 16`; fm_op_kernel.cc: `scaled_fb = (y0+y) >> (fb_shift+1)`)
//    spans a 64x (2^6) depth range across levels 1-7 -- collapsing that to
//    "on" threw away exactly the parameter DX7 patches use for their edge
//    (brass bite, EP pluck, etc). `fb_shift` (patch.h's FmRouting::fb_shift,
//    `8 - feedback_level`) restores the real per-level spacing, anchored so
//    level 7 (max) reproduces this kernel's old, already-safe "always full"
//    >>1 behavior exactly -- see patch.h's FmRouting::fb_shift comment.
// 2. `fb1`/`fb2` used to store the *raw* table lookup (`sample`, before
//    `fm_mul_gain`) -- constant magnitude regardless of the operator's own
//    envelope, so a decaying/releasing operator's feedback "buzz" never
//    faded with it. Dexed's own `fb_buf` stores `y` *after* the gain
//    multiply (fm_op_kernel.cc's `compute_fb`: `y = (y*gain)>>24; ...
//    fb_buf[1] = y`) -- feedback intensity tracks the envelope on real
//    hardware, which is why it "breathes" with the note.
//
// F2 note: the old "pre-out_shift" caveat is gone with the shift itself --
// there is one output scale now, so `scaled` and `out[i]` are the same
// number and feedback strength cannot depend on whether this operator
// happens to be a carrier in the current algorithm.
//
// STILL DIVERGENT, deliberately: Dexed's compute_fb uses
// `(y0 + y) >> (fb_shift + 1)`; this is `>> fb_shift`, i.e. 2x too hot
// (fm2.md §1.1(c)). Left alone by F2 so that the F0 scorecard delta
// attributes cleanly to the scaling contract and nothing else -- it is F4's
// one-character fix.
inline void op_render_fb(FmOp &op, uint32_t n, int32_t fb_shift) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    int32_t *out = op.out;
    int32_t fb1 = op.fb1, fb2 = op.fb2;
    for (uint32_t i = 0; i < n; i++) {
        int32_t fb_mod = (fb1 + fb2) >> fb_shift;
        phase += inc;
        uint32_t idx = (phase + ((uint32_t)fb_mod << FM_MOD_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        int32_t scaled = fm_mul_gain(sample, gain);
        out[i] += scaled;
        fb2 = fb1;
        fb1 = scaled;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
    op.fb1 = fb1;
    op.fb2 = fb2;
}

// One voice's shared bus scratch (fm.md §4.3: "one shared scratch for the
// whole engine, not per-voice" -- callers reuse the same arrays across
// every voice, sequentially).
struct FmVoiceBuses {
    int32_t *mod[FM_NUM_OPS];
    int32_t *out;
};

// Renders one sub-block (<= FM_BLOCK samples) of all six operators,
// following the routing resolved once at note-on (patch.h's FmRouting) --
// nothing here depends on note/velocity/bend, only on `r` and each op's
// already-set phase/inc/gain. This is fm.md §4.1's claim in code: `r.order`
// plus `r.in_bus`/`r.out_bus`/`r.kernel` (bus *pointers* and a processing
// *position*, both resolved at note-on) are the entire routing
// implementation -- nothing about which patch is playing appears inside
// op_render/op_render_first/op_render_fb themselves.
inline void fm_voice_render_block(FmOp ops[FM_NUM_OPS], const FmRouting &r,
                                   const FmVoiceBuses &bus, uint32_t n) {
    for (uint8_t b = 0; b < FM_NUM_OPS; b++) {
        if (r.clear_bus_mask & (1u << b)) {
            for (uint32_t i = 0; i < n; i++) bus.mod[b][i] = 0;
        }
    }
    if (r.clear_bus_mask & (1u << FM_TARGET_OUT)) {
        for (uint32_t i = 0; i < n; i++) bus.out[i] = 0;
    }

    for (uint8_t k = 0; k < FM_NUM_OPS; k++) {
        uint8_t i = r.order[k];
        FmOp &op = ops[i];
        // Carrier vs modulator decides only which *bus* this operator writes
        // to, never how loudly it writes -- F2's contract (see FM_CYCLE): the
        // number is phase deviation either way, and the output bus is simply
        // read as audio instead of as phase.
        bool is_carrier = (r.out_bus[i] == FM_TARGET_OUT);
        op.in = (r.in_bus[i] == FM_BUS_ZERO) ? fm_zero_bus : bus.mod[r.in_bus[i]];
        op.out = is_carrier ? bus.out : bus.mod[r.out_bus[i]];
        switch (r.kernel[i]) {
            case FM_KERNEL_FIRST:    op_render_first(op, n); break;
            case FM_KERNEL_FEEDBACK: op_render_fb(op, n, r.fb_shift[i]); break;
            default:                 op_render(op, n);       break;
        }
    }
}

// Q32 phase increment for one operator (fm.md §5.6: "Coarse/fine ratio,
// detune ... fixed-frequency mode -> the Q32 increment"), from the note's
// own (already bend-scaled) increment. `exp2f` only runs for a nonzero
// detune -- the common case (0 cents) skips it.
inline uint32_t fm_op_inc(const FmOpParams &p, uint32_t note_inc) {
    if (p.fixed_freq) return fm_phase_inc(p.fixed_hz);
    float detune_ratio = (p.detune_cents != 0.0f) ? exp2f(p.detune_cents / 1200.0f) : 1.0f;
    return (uint32_t)((float)note_inc * p.ratio * detune_ratio);
}

// Note-on: resolves every operator's phase/inc from the patch and the
// note's base increment (already pitch-bent by Core 0), triggers its EG
// (env_dx.h's env_dx_trigger() -- stage 1 from silence), and resolves
// `static_log2`: output level (TL) + velocity sensitivity + #48's key
// level scaling, and `rate_scale_qrate` (#48's key rate scaling) -- the
// note-on-time-only pieces of #45/#48's level/rate chain (fm.md §5.6: all
// of it is "resolved once per note-on and never touched again"). `midinote`
// (raw MIDI 0-127) is the one piece those two DX7 features need that
// `note_inc` alone can't give back -- see engine.h's VoiceParams::note.
// `gain`/`gain_step` are deliberately NOT set here -- fm_voice_step_envelopes()
// sets them every block, starting from EG_LOG2_FLOOR (silence) on the very
// first block of this note, so there is no separate "initial gain" to get
// right here.
inline void fm_voice_note_on(FmOp ops[FM_NUM_OPS], const FmPatch &patch,
                              uint32_t note_inc, int16_t amplitude, uint8_t midinote,
                              FmPitchEg *peg = nullptr, FmLfo *lfo = nullptr) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        op.phase = 0;
        op.inc = fm_op_inc(p, note_inc);
        op.gain = 0;
        op.gain_step = 0;
        op.fb1 = 0;
        op.fb2 = 0;
        op.in = fm_zero_bus;
        op.out = nullptr;  // assigned per sub-block by fm_voice_render_block()
        env_dx_trigger(op.eg);
        // Key level scaling combines with TL *before* converting to log2,
        // clamped to [0,127] exactly like Dexed's own `outlevel =
        // min(127, outlevel)` (dx7note.cc) -- not added as a separate,
        // unclamped log2 offset. A boosting curve (DX7 curve 2/3, "+EXP"/
        // "+LIN") at high depth and an extreme note can otherwise push the
        // combined value well past what a single operator's reference
        // `level` was ever meant to represent, and eg_to_linear()'s shift
        // has no defined behavior for a large positive log2 offset -- this
        // clamp is what keeps it in the range that function already
        // guarantees is safe (DX7_LEVEL_TO_LOG2[]'s own [-16,0]-octave-ish
        // span), the same way Dexed's own hardware-matching clamp does.
        int32_t combined_level = dx7_scaleoutlevel(p.output_level)
                                + dx7_scale_level(midinote, p.scale_breakpoint, p.scale_left_depth,
                                                   p.scale_right_depth, p.scale_left_curve, p.scale_right_curve);
        combined_level = combined_level < 0 ? 0 : (combined_level > 127 ? 127 : combined_level);
        int32_t output_and_key_log2 = (combined_level - dx7_scaleoutlevel(99)) * 32;
        op.static_log2 = output_and_key_log2 + eg_vel_sensitivity_log2(p.vel_sensitivity, amplitude);
        op.rate_scale_qrate = dx7_scale_rate(midinote, p.rate_scaling);
    }
    // #49: pitch EG/LFO are per-VOICE (not per-operator), so their state
    // lives in the caller's own arrays (audio_engine.cpp), not FmOp --
    // optional pointers so callers that don't care about #49's feature set
    // (older/simpler host tests) can still call this unchanged.
    if (peg) fm_pitch_eg_trigger(*peg, patch.pitch_eg);
    if (lfo) fm_lfo_trigger(*lfo, patch.lfo.key_sync);
}

// Note-off: releases every operator's EG (env_dx.h's env_dx_release() --
// jump to stage 4 from wherever it currently is). Every operator releases,
// not just carriers -- a modulator's own decay shapes the carrier's timbre
// for as long as the carrier is still sounding (fm.md's EP patch: op4's
// fast decay is what makes the carrier's tone dim after the attack).
inline void fm_voice_note_off(FmOp ops[FM_NUM_OPS], FmPitchEg *peg = nullptr) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        env_dx_release(ops[i].eg);
    }
    if (peg) fm_pitch_eg_release(*peg);  // #49: jump to the pitch EG's own release stage from wherever it is, same convention as the amplitude EGs above
}

// A voice may be reported free only once every operator that actually
// reaches the final mix (a carrier, r.out_bus[i] == FM_TARGET_OUT) has
// finished its release -- env_dx.h's EG_IDLE, reached only after an
// explicit note-off's stage-4 ramp completes. Modulators are irrelevant
// here: their EGs shape the sound but never reach the ear directly, so a
// modulator still mid-decay can't keep a voice "active" once its carriers
// are silent. This is the guarantee the tracker's #21 bug ("key-off never
// frees a voice") lacked -- EG_IDLE is a real terminal state, not an
// epsilon-on-a-decaying-value guess.
inline bool fm_voice_active(const FmOp ops[FM_NUM_OPS], const FmRouting &r) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (r.out_bus[i] == FM_TARGET_OUT && ops[i].eg.active()) return true;
    }
    return false;
}

// #49: supersedes #44's fm_voice_update_pitch() -- re-derives every
// operator's `inc` from the note's current (possibly re-bent) base
// increment, same as before, but now ALSO steps the voice's pitch EG
// (pitch_eg.h) and LFO (lfo.h) by this control block and folds their
// combined cents output into the same increment recompute (fm.md §5.4/§5.5:
// "applied by scaling all six operator increments at each block
// boundary"). Called once per control block from fm_render_voice() below
// (previously fm_voice_update_pitch() ran once per whole audio buffer from
// audio_engine.cpp -- strictly finer-grained now, not a behavior change for
// plain pitch bend, since BLOCK-rate recompute can only make a held bend
// glide MORE accurately timed, never less). Fixed-frequency operators
// (`p.fixed_freq`) skip the pitch EG/LFO term entirely -- matches Dexed's
// own `osc_freq()` fixed-mode branch, which only ever receives pitch bend
// (`pitch_base`) and never `pitch_mod` (pitchenv + LFO): a fixed-frequency
// bell/percussion partial's absolute pitch doesn't wobble with the rest of
// the voice's vibrato on real hardware, so it doesn't here either. Returns
// this block's amplitude-mod attenuation fraction (0..1, before each
// operator's own AM sensitivity weights it) for the caller to hand to
// fm_voice_step_envelopes().
inline float fm_voice_step_pitch_and_mod(FmOp ops[FM_NUM_OPS], const FmPatch &patch,
                                          FmPitchEg &peg, FmLfo &lfo, uint32_t note_inc, uint32_t n,
                                          int16_t mod_wheel) {
    float peg_cents = fm_pitch_eg_step_block(peg, patch.pitch_eg, n);
    float mod_wheel_frac = (float)mod_wheel / 32767.0f;
    float lfo_pitch_cents, lfo_amp_atten;
    fm_lfo_step_block(lfo, patch.lfo, n, mod_wheel_frac, lfo_pitch_cents, lfo_amp_atten);

    float total_cents = peg_cents + lfo_pitch_cents;
    float pitch_ratio = (total_cents != 0.0f) ? exp2f(total_cents * (1.0f / 1200.0f)) : 1.0f;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        uint32_t base_inc = fm_op_inc(p, note_inc);
        ops[i].inc = p.fixed_freq ? base_inc : (uint32_t)((float)base_inc * pitch_ratio);
    }
    return lfo_amp_atten;
}

// Steps every operator's EG by one control block (env_dx.h's
// env_dx_step_block()) and sets `gain`/`gain_step` from the result --
// fm.md §5.3's "the kernel is handed gain (start) and gain_step (per-sample
// delta) for that block". Everything here runs once per block, not once
// per sample: this is the only place outside note-on that touches `gain`,
// and op_render/op_render_first/op_render_fb (unchanged since #44) never
// see any of env_dx.h -- from the kernel's point of view this could be a
// fixed gain, a hand-set ramp, or an EG, and it wouldn't know the
// difference (fm.md §4.1's routing claim, restated for the EG).
//
// #49: `amp_atten` (0..1, lfo.h's own tremolo-attenuation output, default 0
// so every pre-#49 caller is behavior-neutral) is weighted by each
// operator's own `am_sensitivity` (0-3, DX7 AMS) and multiplied straight
// into the already-computed linear gain -- fm.md's own "amplitude mod folds
// into each operator's gain/gain_step computation according to its AM
// sensitivity", applied AFTER eg_to_linear() rather than as another log2
// offset, since a multiplicative tremolo on top of the EG's own linear gain
// is the natural place for it (no interaction with EnvDX's own state at
// all).
inline void fm_voice_step_envelopes(FmOp ops[FM_NUM_OPS], const FmPatch &patch, uint32_t n,
                                     float amp_atten = 0.0f) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        int32_t log2_start, log2_end;
        env_dx_step_block(op.eg, p, n, log2_start, log2_end, op.rate_scale_qrate);
        // FM_GAIN_MAX, not a per-operator reference: F2's contract puts every
        // operator on one scale, and what makes this one quieter than that one
        // is its output level / EG / key scaling, all of which are already
        // folded into the log2 offset being passed in here.
        int32_t gain_start = eg_to_linear(FM_GAIN_MAX, op.static_log2 + log2_start);
        int32_t gain_end = eg_to_linear(FM_GAIN_MAX, op.static_log2 + log2_end);
        if (amp_atten > 0.0f && p.am_sensitivity > 0) {
            float mult = 1.0f - amp_atten * (DX7_AMP_MOD_SENS[p.am_sensitivity & 3]);
            if (mult < 0.0f) mult = 0.0f;
            gain_start = (int32_t)((float)gain_start * mult);
            gain_end = (int32_t)((float)gain_end * mult);
        }
        op.gain = gain_start;
        op.gain_step = (gain_end - gain_start) / (int32_t)n;
    }
}

// Renders `frames` samples of one voice in FM_BLOCK-sized sub-blocks,
// panning the shared output bus into dry_l/dry_r (accumulate: callers with
// multiple voices must clear dry_l/dry_r once up front, same convention as
// speech's speech_render_voice()). Stops early once every carrier's EG has
// gone idle mid-buffer (fm_voice_active()) -- the remaining sub-blocks
// would render silence anyway; this just skips paying for it, mirroring
// the subtractive engine's `if (!envelope[v].active()) break;`.
//
// #49: `peg`/`lfo` are optional (default nullptr, same convention
// fm_voice_note_on()/fm_voice_note_off() use) -- when both are given, every
// sub-block also steps the voice's pitch EG and LFO (fm_voice_step_pitch_and_mod(),
// superseding #44's fm_voice_update_pitch(), which ran this same increment
// recompute once per whole buffer instead of once per control block) and
// folds the LFO's amplitude-mod output into fm_voice_step_envelopes(). A
// caller that omits them gets exactly #44/#45/#48's old behavior (no live
// pitch bend recompute at all) -- every real caller (device and host) is
// updated to pass real state; nullptr only exists for isolated kernel-only
// tests that don't care about any of this.
inline void fm_render_voice(FmOp ops[FM_NUM_OPS], const FmPatch &patch, const FmRouting &r,
                             const FmVoiceBuses &bus, int16_t pan,
                             int32_t *dry_l, int32_t *dry_r, uint32_t frames,
                             FmPitchEg *peg = nullptr, FmLfo *lfo = nullptr,
                             uint32_t note_inc = 0, int16_t mod_wheel = 0) {
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > FM_BLOCK) n = FM_BLOCK;
        if (peg && lfo) {
            float amp_atten = fm_voice_step_pitch_and_mod(ops, patch, *peg, *lfo, note_inc, n, mod_wheel);
            fm_voice_step_envelopes(ops, patch, n, amp_atten);
        } else {
            fm_voice_step_envelopes(ops, patch, n);
        }
        fm_voice_render_block(ops, r, bus, n);
        for (uint32_t i = 0; i < n; i++) {
            // The output bus is in FM_CYCLE units like every other bus; this
            // is the single point where it stops being phase-deviation and
            // becomes audio (F2, see FM_VOICE_OUT_SHIFT).
            int32_t s = bus.out[i] >> FM_VOICE_OUT_SHIFT;
            dry_l[done + i] += (s * gain_l) >> 15;
            dry_r[done + i] += (s * gain_r) >> 15;
        }
        done += n;
        if (!fm_voice_active(ops, r)) break;
    }
}
