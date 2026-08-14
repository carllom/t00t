#pragma once

#include "env_dx.h"
#include "fm_scale.h"
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

// FM operator kernel (#44, module_fm.md §5.2): FmOp + the three per-sample
// variants, plus the note-on/block/voice glue that turns a resolved
// FmRouting (patch.h) into real audio. Per-sample bodies use the same table
// (sine_tab.h's real 4096-entry table, not rig.h's standalone bench copy),
// the same fm_mul_gain convention, and the same phase-wrap-is-free indexing
// as rig.h's op_render/op_render_first/op_render_fb. See "The fixed-point
// contract" below (fm_scale.h) for the single anchor everything derives
// from. module_fm.md §3.6 covers the tuning-lever decisions baked in here
// (SMULWB, flash placement, no op-pair interleaving). No pico-sdk dependency
// beyond the conditional arm_acle.h include (device only), so this header
// is shared by the device engine and the host render/test harness, exactly
// like render.h/rig.h.

// BLOCK size (module_fm.md §3.6, closed by #45). Overridable at compile time
// (`-DT00T_FM_BLOCK=8/32`, wired through CMakeLists.txt/Makefile the same
// way DMA_BUFFER_SIZE is) so comparisons don't require hand-editing this
// file. Also sets the shared per-voice bus scratch size (module_fm.md §4.3).
#ifndef T00T_FM_BLOCK
#define T00T_FM_BLOCK 16
#endif
static constexpr uint32_t FM_BLOCK = T00T_FM_BLOCK;

struct FmOp {
    uint32_t phase;
    uint32_t inc;
    // F5: the operator's increment at neutral pitch, resolved once at note-on
    // (fm_op_base_inc). Ratio, detune and fixed-frequency all fold into it
    // there, so the per-block path is a single multiply by the pitch
    // bend/EG/LFO ratio instead of re-deriving them every block.
    uint32_t base_inc;
    int32_t  gain;
    int32_t  gain_step;    // per-sample delta for this block, from EnvDX (#45, env_dx.h)
    const int32_t *in;     // modulation bus, or fm_zero_bus for an unmodulated operator
    int32_t *out;           // modulation bus, or the shared voice output bus
    int32_t  fb1, fb2;      // op_render_fb only: last two post-gain outputs (see op_render_fb's own comment)
    EnvDX    eg;            // this operator's own 4-stage envelope
    // There is no separate `static_log2`/`rate_scale_qrate` field: output
    // level, key level scaling and velocity compose into the envelope's own
    // `outlevel` (env_dx.h's dx7_note_outlevel), and key rate scaling into
    // its `rate_scaling`, exactly as Dexed's Dx7Note::init does.
};

// Read-only all-zero bus, shared by every operator nothing modulates.
inline int32_t fm_zero_bus[FM_BLOCK];

// Multiply-by-gain-then-shift. SMULWB (module_fm.md §3.6 lever 4) fuses the
// gain-scale + multiply into one M33 DSP-extension instruction --
// (Rn * SignExtend16(Rm)) >> 16 -- adopted because it's a real win with no
// correctness cost; unlike rig.h's FM_RIG_SMULWB toggle (a bench A/B lever),
// this is just how the real kernel works. Host builds get the portable
// 64-bit-multiply equivalent -- same numeric result, not the real
// instruction (arm_acle.h doesn't exist off-target). __smlawb
// (multiply-accumulate, third operand forced to 0) stands in for SMULWB
// because this GCC's arm_acle.h has no standalone SMULWB wrapper.
inline int32_t fm_mul_gain(int32_t sample, int32_t gain) {
#if defined(__ARM_FEATURE_DSP)
    return __smlawb(gain, sample, 0);
#else
    return (int32_t)(((int64_t)gain * (int16_t)sample) >> 16);
#endif
}


// Plain kernel: accumulates (+=) into `out`. F2 removed the per-call
// `out_shift` parameter -- there is one output scale now, not a carrier one
// and a modulator one -- and left the shape otherwise unchanged
// (module_fm.md §3.2).
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
        // FM_TABLE_BITS-wide index, no mask, no modulo (module_fm.md §3.2).
        uint32_t idx = (phase + ((uint32_t)in[i] << FM_MOD_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain);
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// First-writer variant: stores instead of accumulating, so its target bus
// never needs clearing (module_fm.md §4.3/§5.2).
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
// post-gain outputs, averaged and attenuated by `fb_shift + 1` (`fb_shift`
// from patch.h's FmRouting, resolved at note-on from FmOpParams::
// feedback_level, real DX7 units 0-7 -- a 64x depth range across levels
// 1-7, not an on/off switch), instead of an external bus (DX7-style).
// Matches Dexed's own `compute_fb` (`scaled_fb = (y0+y) >> (fb_shift+1)`,
// Source/msfa/fm_op_kernel.cc) exactly, including the `+1`: a bare
// `>> fb_shift` is exactly 2x too much feedback at every level, and because
// feedback is what a DX7 patch's "edge" is voiced around, that error reads
// as a general excess of brightness rather than a feedback bug. Storing
// `fb1`/`fb2` *after* the gain multiply (not the raw table lookup) is what
// makes feedback intensity track the operator's own envelope -- why it
// "breathes" with the note on real hardware, same as Dexed's own `fb_buf`.
//
// Always accumulates (+=) -- patch.h's routing compiler guarantees this is
// safe by either scheduling a non-feedback first-writer ahead of it on a
// shared bus, or (clear_bus_mask) pre-zeroing a bus whose only writer is
// this op.
inline void op_render_fb(FmOp &op, uint32_t n, int32_t fb_shift) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    int32_t *out = op.out;
    int32_t fb1 = op.fb1, fb2 = op.fb2;
    for (uint32_t i = 0; i < n; i++) {
        int32_t fb_mod = (fb1 + fb2) >> (fb_shift + 1);
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

// One voice's shared bus scratch (module_fm.md §4.3) -- callers reuse the
// same arrays across every voice, sequentially.
struct FmVoiceBuses {
    int32_t *mod[FM_NUM_OPS];
    int32_t *out;
};

// Renders one sub-block (<= FM_BLOCK samples) of all six operators,
// following the routing resolved once at note-on (patch.h's FmRouting) --
// nothing here depends on note/velocity/bend, only on `r` and each op's
// already-set phase/inc/gain. This is module_fm.md §4.1's claim in code: `r.order`
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

// How many cents one unit of DX7 detune is worth on a given note, in ratio
// mode -- Dexed's `Dx7Note::osc_freq()`:
//
//     detuneRatio = 0.0209 * exp(-0.396 * log2(f_note)) / 7
//     logfreq    += detuneRatio * logfreq * (detune - 7)
//
// which, since `logfreq` is Q24 log2 of the note's frequency, is a cents
// offset of `detuneRatio * log2(f_note) * 1200` per unit.
//
// The load-bearing word is *note*. Real DX7 detune is not a fixed cents
// offset: it falls off as the note rises, from 2.46 cents/unit at C1 to
// 0.68 at C6 -- getting this wrong changes the beating rate between two
// operators sharing a ratio, which is the entire point of detune.
//
// Depends only on the note, so it is computed once per note-on and shared by
// all six operators.
inline float dx7_detune_cents_per_unit(uint8_t midinote) {
    // Q24 log2(frequency), same base as shim/tuning.h's standard tuning:
    // 50857777 / 2^24 is log2 of MIDI note 0 (8.1758 Hz).
    const float log2_f = (float)midinote / 12.0f + (50857777.0f / 16777216.0f);
    const float detune_ratio = 0.0209f * expf(-0.396f * log2_f) / 7.0f;
    return detune_ratio * log2_f * 1200.0f;
}

// Fixed-frequency mode uses a different, much simpler detune rule that only
// ever sharpens (Dexed: `logfreq += detune > 7 ? 13457 * (detune - 7) : 0`).
// 13457 / 2^24 * 1200 = 0.9624 cents per unit, note-independent.
static constexpr float DX7_FIXED_DETUNE_CENTS_PER_UNIT = 0.96244f;

// The operator's Q32 phase increment at neutral pitch -- no bend, no pitch EG,
// no LFO. Resolved once at note-on into FmOp::base_inc; the per-block path
// only scales it (fm_voice_step_pitch_and_mod), so the `exp2f`/`expf` here
// never runs in the render loop.
inline uint32_t fm_op_base_inc(const FmOpParams &p, uint32_t note_inc,
                                float detune_cents_per_unit) {
    if (p.fixed_freq) {
        float cents = (p.detune_offset > 0)
            ? (float)p.detune_offset * DX7_FIXED_DETUNE_CENTS_PER_UNIT
            : 0.0f;
        float hz = (cents != 0.0f) ? p.fixed_hz * exp2f(cents / 1200.0f) : p.fixed_hz;
        return fm_phase_inc(hz);
    }
    float cents = (float)p.detune_offset * detune_cents_per_unit;
    float detune_ratio = (cents != 0.0f) ? exp2f(cents / 1200.0f) : 1.0f;
    return (uint32_t)((float)note_inc * p.ratio * detune_ratio);
}

// Note-on: resolves every operator's phase/inc from the patch and the
// note's base increment (already pitch-bent by Core 0), triggers its EG
// (env_dx.h's env_dx_init() -- stage 0 from silence), which also folds in
// output level, key level scaling, velocity and key rate scaling -- the
// note-on-time-only pieces of #45/#48's level/rate chain (module_fm.md §5.6: all
// of it is "resolved once per note-on and never touched again"). `midinote`
// (raw MIDI 0-127) is the one piece those two DX7 features need that
// `note_inc` alone can't give back -- see engine.h's VoiceParams::note.
// `gain`/`gain_step` are deliberately NOT set here -- fm_voice_step_envelopes()
// sets them every block, starting from silence on the very
// first block of this note, so there is no separate "initial gain" to get
// right here.
inline void fm_voice_note_on(FmOp ops[FM_NUM_OPS], const FmPatch &patch,
                              uint32_t note_inc, int16_t amplitude, uint8_t midinote,
                              FmPitchEg *peg = nullptr, FmLfo *lfo = nullptr) {
    const float detune_scale = dx7_detune_cents_per_unit(midinote);
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        op.phase = 0;
        op.base_inc = fm_op_base_inc(p, note_inc, detune_scale);
        op.inc = op.base_inc;
        op.gain = 0;
        op.gain_step = 0;
        op.fb1 = 0;
        op.fb2 = 0;
        op.in = fm_zero_bus;
        op.out = nullptr;  // assigned per sub-block by fm_voice_render_block()
        // F3: one call, Dexed's own composition order. `outlevel` folds
        // output level, key level scaling and velocity together (env_dx.h's
        // dx7_note_outlevel), and the envelope itself folds THAT into each
        // stage's target -- rather than the engine carrying a separate static
        // offset and adding it afterwards.
        //
        // `amplitude` is VoiceParams' Q15 velocity; Dexed's ScaleVelocity
        // wants the raw MIDI 0-127 it was derived from, so it is recovered
        // here. The round trip is exact for every velocity, since Core 0
        // built it as velocity/127 * 32767 (midi_controller.cpp).
        int velocity = ((int)amplitude * 127 + 16383) / 32767;
        env_dx_init(op.eg, p.eg_rate, p.eg_level,
                    dx7_note_outlevel(p, midinote, velocity),
                    dx7_scale_rate(midinote, p.rate_scaling));
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
// for as long as the carrier is still sounding (module_fm.md's EP patch: op4's
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

// #49: re-derives every operator's `inc` from the note's current (possibly
// re-bent) base increment, and also steps the voice's pitch EG (pitch_eg.h)
// and LFO (lfo.h) by this control block, folding their combined cents
// output into the same increment recompute (module_fm.md §5.4/§5.5). Called
// once per control block from fm_render_voice() below -- BLOCK-rate
// recompute can only make a held bend glide MORE accurately timed than a
// once-per-buffer recompute, never less. Fixed-frequency operators
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
        // Fixed-frequency operators ignore the pitch EG and LFO entirely --
        // matching Dexed's own osc_freq() fixed branch, which never receives
        // pitch_mod. A bell partial pinned to an absolute frequency does not
        // wobble with the rest of the voice's vibrato on real hardware.
        ops[i].inc = patch.op[i].fixed_freq
            ? ops[i].base_inc
            : (uint32_t)((float)ops[i].base_inc * pitch_ratio);
    }
    return lfo_amp_atten;
}

// Steps every operator's EG by one control block (env_dx.h's
// env_dx_step_block()) and sets `gain`/`gain_step` from the result
// (module_fm.md §5.3). Everything here runs once per block, not once
// per sample: this is the only place outside note-on that touches `gain`,
// and op_render/op_render_first/op_render_fb never see any of env_dx.h --
// from the kernel's point of view this could be a fixed gain, a hand-set
// ramp, or an EG, and it wouldn't know the difference (module_fm.md §4.1).
//
// `amp_atten` (0..1, from lfo.h) is subtracted from each operator's
// LOG-domain level, BEFORE the exp lookup that turns it into a gain --
// matching Dexed, which subtracts amp mod at the same point rather than
// multiplying it into the already-computed linear gain. That distinction
// matters: a fixed linear factor is a fixed dB attenuation, whereas the
// log-domain subtraction is proportional to the current level, so the two
// curves disagree more the further the envelope has decayed. See
// dx7_am_level_factor() (lfo.h) for the curve.
//
// The guard is `am_sensitivity != 0` alone, matching Dexed, NOT
// `amp_mod > 0`: the curve is deliberately non-zero at zero mod (~1 dB), so
// an operator with AMS > 0 is slightly attenuated even on a patch with
// AMD = 0. Skipping that when the LFO happens to be at rest would put a
// step discontinuity into a patch's level the moment the wheel moved.
inline void fm_voice_step_envelopes(FmOp ops[FM_NUM_OPS], const FmPatch &patch, uint32_t n,
                                     float amp_mod = 0.0f) {
    // At most three distinct factors are possible (AMS 1-3), so this is
    // hoisted out of the per-operator loop rather than computed six times.
    // Only worth the branch when some operator actually has AMS > 0, which on
    // the ROM banks is a minority of patches.
    float am_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool am_any = false;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        if (patch.op[i].am_sensitivity & 3) { am_any = true; break; }
    }
    if (am_any) {
        for (uint8_t s = 1; s < 4; s++) {
            am_factor[s] = dx7_am_level_factor(amp_mod * DX7_AMP_MOD_SENS[s]);
        }
    }

    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];

        // F3: the envelope is now stepped once and read twice -- its level
        // before and after this block -- rather than returning a pair of log2
        // offsets to be composed with a separate static term. `gain` is the
        // block's starting gain and `gain_step` the per-sample delta, exactly
        // as before, so the per-sample kernel is untouched by any of this.
        int32_t level_start = op.eg.level;
        int32_t level_end = env_dx_step_block(op.eg, n);

        // Amp mod lands here, in the log domain, between the envelope and
        // eg_to_gain() -- exactly where Dexed puts it (`level -= ldiff` just
        // before `params_[op].level_in = level`). eg_to_gain() clamps a level
        // driven negative by a full tremolo dip to silence, so no clamp here.
        uint8_t ams = p.am_sensitivity & 3;
        if (ams) {
            level_start -= (int32_t)((float)level_start * am_factor[ams]);
            level_end   -= (int32_t)((float)level_end * am_factor[ams]);
        }

        int32_t gain_start = eg_to_gain(level_start);
        int32_t gain_end = eg_to_gain(level_end);
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
// sub-block also steps the voice's pitch EG and LFO
// (fm_voice_step_pitch_and_mod()) and folds the LFO's amplitude-mod output
// into fm_voice_step_envelopes(). A caller that omits them gets no live
// pitch bend recompute at all -- every real caller (device and host) passes
// real state; nullptr only exists for isolated kernel-only tests that don't
// care about any of this.
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
