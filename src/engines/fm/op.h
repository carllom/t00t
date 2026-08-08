#pragma once

#include "pan.h"
#include "patch.h"
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
// rig.h's standalone bench copy), same fm_mul_gain/FM_OUT_SHIFT convention,
// same phase-wrap-is-free indexing. fm.md §3.6's decisions are already
// baked in: SMULWB adopted (lever 4, "adopted where convenient"), plain
// `inline` in flash, not not_in_flash_func (lever 2, SRAM measured worse),
// no op-pair interleaving (lever 1, not adopted). No pico-sdk dependency
// beyond the conditional arm_acle.h include (device only), so this header
// is shared by the device engine and the host render/test harness, exactly
// like render.h/rig.h.

// fm.md §3.6 lever 3: BLOCK size. Stays provisional at 16 per #43's
// decision (final call deferred to P2, once EnvDX's time resolution is a
// real tradeoff to measure against) -- also the shared per-voice bus
// scratch size (fm.md §4.3).
static constexpr uint32_t FM_BLOCK = 16;

struct FmOp {
    uint32_t phase;
    uint32_t inc;
    int32_t  gain;
    int32_t  gain_step;    // 0 for the whole P1 voice lifetime -- no EG yet (EnvDX is P2)
    const int32_t *in;     // modulation bus, or fm_zero_bus for an unmodulated operator
    int32_t *out;           // modulation bus, or the shared voice output bus
    int32_t  fb1, fb2;      // op_render_fb only: last two raw table outputs
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
static constexpr int32_t FM_OUT_SHIFT = 6;

// Plain kernel: accumulates (+=) into `out`. fm.md §3.2's 13-instruction
// listing (as measured by #43) is this loop body.
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
        uint32_t idx = (phase + (uint32_t)in[i]) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain) >> FM_OUT_SHIFT;
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
        uint32_t idx = (phase + (uint32_t)in[i]) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] = fm_mul_gain(sample, gain) >> FM_OUT_SHIFT;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// Self-feedback: the modulation input is this operator's own last two raw
// table outputs, averaged, instead of an external bus (DX7-style). Always
// accumulates (+=) -- patch.h's routing compiler guarantees this is safe by
// either scheduling a non-feedback first-writer ahead of it on a shared
// bus, or (clear_bus_mask) pre-zeroing a bus whose only writer is this op.
inline void op_render_fb(FmOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    int32_t *out = op.out;
    int32_t fb1 = op.fb1, fb2 = op.fb2;
    for (uint32_t i = 0; i < n; i++) {
        int32_t fb_mod = (fb1 + fb2) >> 1;
        phase += inc;
        uint32_t idx = (phase + (uint32_t)fb_mod) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain) >> FM_OUT_SHIFT;
        fb2 = fb1;
        fb1 = sample;
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
        op.in = (r.in_bus[i] == FM_BUS_ZERO) ? fm_zero_bus : bus.mod[r.in_bus[i]];
        op.out = (r.out_bus[i] == FM_TARGET_OUT) ? bus.out : bus.mod[r.out_bus[i]];
        switch (r.kernel[i]) {
            case FM_KERNEL_FIRST:    op_render_first(op, n); break;
            case FM_KERNEL_FEEDBACK: op_render_fb(op, n);    break;
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

// Note-on: resolves every operator's phase/inc/gain from the patch, the
// note's base increment (already pitch-bent by Core 0) and velocity.
// gain_step stays 0 -- fixed gains for the whole voice life, no EG yet
// (fm.md P1: "Fixed gains still -- the EG is the next slice"). Velocity
// reaches only the carriers' output level ("velocity as plain amplitude",
// #44); modulator index is untouched by velocity until #45's sensitivity.
inline void fm_voice_note_on(FmOp ops[FM_NUM_OPS], const FmPatch &patch,
                              uint32_t note_inc, int16_t amplitude) {
    float amp_ratio = (float)amplitude / 32767.0f;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        op.phase = 0;
        op.inc = fm_op_inc(p, note_inc);
        bool is_carrier = (p.mod_target == FM_TARGET_OUT);
        op.gain = is_carrier ? (int32_t)((float)p.level * amp_ratio) : p.level;
        op.gain_step = 0;
        op.fb1 = 0;
        op.fb2 = 0;
        op.in = fm_zero_bus;
        op.out = nullptr;  // assigned per sub-block by fm_voice_render_block()
    }
}

// Re-derives every operator's `inc` from the note's current (possibly
// re-bent) base increment, without touching phase/gain/feedback history --
// called every buffer for a held voice so pitch bend stays live, while
// note-on-only state (gain, the routing itself) is untouched between notes.
inline void fm_voice_update_pitch(FmOp ops[FM_NUM_OPS], const FmPatch &patch, uint32_t note_inc) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        ops[i].inc = fm_op_inc(patch.op[i], note_inc);
    }
}

// Renders `frames` samples of one voice in FM_BLOCK-sized sub-blocks,
// panning the shared output bus into dry_l/dry_r (accumulate: callers with
// multiple voices must clear dry_l/dry_r once up front, same convention as
// speech's speech_render_voice()).
inline void fm_render_voice(FmOp ops[FM_NUM_OPS], const FmRouting &r, const FmVoiceBuses &bus,
                             int16_t pan, int32_t *dry_l, int32_t *dry_r, uint32_t frames) {
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > FM_BLOCK) n = FM_BLOCK;
        fm_voice_render_block(ops, r, bus, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = bus.out[i];
            dry_l[done + i] += (s * gain_l) >> 15;
            dry_r[done + i] += (s * gain_r) >> 15;
        }
        done += n;
    }
}
