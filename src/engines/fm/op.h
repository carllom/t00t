#pragma once

#include "env_dx.h"
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
// rig.h's standalone bench copy), same fm_mul_gain convention, same
// phase-wrap-is-free indexing. The output shift is now a per-call parameter
// rather than a single compile-time FM_OUT_SHIFT constant (#57: carriers
// and modulators need very different headroom, see FM_OUT_SHIFT_CARRIER/
// FM_OUT_SHIFT_MODULATOR below) -- same instruction shape either way
// (disassembly-verified, #57: still 48 `smlawb` instances in the device
// build, same as #44/#45; the shift becomes a register operand instead of
// an immediate, resolved once per fm_voice_render_block() call, never
// inside the per-sample loop). fm.md §3.6's decisions are otherwise still
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
    int32_t  fb1, fb2;      // op_render_fb only: last two raw table outputs
    EnvDX    eg;            // #45: this operator's own 4-stage envelope
    int32_t  static_log2;   // #45: output level + velocity sensitivity, resolved once at note-on
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

// #57: two roles, two shifts, on top of `fm_mul_gain`'s own implicit >>16.
// A carrier's `out[]` becomes final int16-range audio (mixed with other
// voices, needs headroom for that sum) -- FM_OUT_SHIFT_CARRIER=6 is
// unchanged from #44/#45's already-hardware-verified value, so carrier
// loudness/headroom doesn't move at all. A modulator's `out[]` instead
// becomes the *next* operator's raw phase-modulation input, added directly
// into a full-circle (2^32 = 2*pi) uint32_t phase accumulator -- the same
// >>6 there capped deviation at ~0.03-0.05 rad even at gain=INT32_MAX (see
// patch.h's FM_TEST_PATCH comment, and #57's own writeup), nowhere near
// enough for real FM character. FM_OUT_SHIFT_MODULATOR=0 (i.e. only
// `fm_mul_gain`'s built-in >>16 applies) raises that ceiling to ~1.57 rad
// at gain=INT32_MAX -- 64x more headroom, comfortably inside the range
// expressive DX7-style patches actually use. Passed as a parameter (not a
// second constant baked into a duplicated function body) so the per-sample
// loop shape -- and #43's measured instruction count -- stays identical;
// only the shift's operand value differs per call, chosen once by
// fm_voice_render_block() from the routing, never inside the hot loop.
static constexpr int32_t FM_OUT_SHIFT_CARRIER = 6;
static constexpr int32_t FM_OUT_SHIFT_MODULATOR = 0;

// #57 part 2: FM_OUT_SHIFT_MODULATOR=0 already extracts the maximum
// magnitude a 32-bit `gain * sample` product can yield -- there is no
// smaller shift to give, that multiply structurally cannot produce a wider
// result. But real DX7 hardware (via Dexed, the ground-truth reference,
// fm.md §7) doesn't need a bigger raw magnitude at all: it uses a phase
// representation that only needs 2^24 units per full cycle (`Sin::lookup`'s
// table read ignores every bit above bit 23), not 2^32 like this engine's
// `phase`. That's the actual gap -- not raw output magnitude, but how much
// phase deviation a given magnitude buys. `FM_MOD_INPUT_SHIFT` closes it at
// the one place it needs to be closed: pre-scaling incoming modulation
// (`in[i]`, or feedback's own history) *before* it's added to `phase`,
// using unsigned wraparound (well-defined, and exactly how a real cycle
// works -- the same trick a narrower phase representation gets for free).
// Nothing is stored any wider than before (`in[]`/`out[]` stay `int32_t`,
// so #57 part 1's fan-in/carrier-count overflow fixes are untouched) --
// this only changes how aggressively the same stored value bends phase.
// Chosen generously (real DX7 patches routinely exceed one full modulation
// cycle for bright/bell/brass character) since `level`/`output_level`/the
// EG are still the real per-patch depth control, same as on real hardware
// -- this shift is not a substitute for tuning those, just headroom big
// enough that they're never the thing running out first. Host-measured
// (#57) across ROM1A's 28 converted patches: 2nd-harmonic/fundamental
// ratios now span ~0.0 (patches with no real modulation at that voicing --
// legitimate, not a bug) to several times the fundamental (BRASS 1 ~3.9x,
// ORCHESTRA ~7.0x) -- real per-patch variety, not a uniform near-zero.
// `FM_TEST_PATCH` (patch.h) needed its own modulator levels re-tuned down
// after this landed -- its old near-ceiling constants, chosen when this
// shift didn't exist, now over-drive badly enough at some (not all) EG
// rates to underflow `eg_to_linear()` during the attack (a real, caught-
// before-hardware bug: a 90-rate attack landing on an exact zero mid-ramp).
static constexpr int32_t FM_MOD_INPUT_SHIFT = 4;

// Plain kernel: accumulates (+=) into `out`. fm.md §3.2's 13-instruction
// listing (as measured by #43) is this loop body -- `out_shift` replaces
// the #44/#45 compile-time FM_OUT_SHIFT constant (#57), same instruction
// shape either way.
inline void op_render(FmOp &op, uint32_t n, int32_t out_shift) {
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
        uint32_t idx = (phase + ((uint32_t)in[i] << FM_MOD_INPUT_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain) >> out_shift;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// First-writer variant: stores instead of accumulating, so its target bus
// never needs clearing (fm.md §4.3/§5.2).
inline void op_render_first(FmOp &op, uint32_t n, int32_t out_shift) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    const int32_t *in = op.in;
    int32_t *out = op.out;
    for (uint32_t i = 0; i < n; i++) {
        phase += inc;
        uint32_t idx = (phase + ((uint32_t)in[i] << FM_MOD_INPUT_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] = fm_mul_gain(sample, gain) >> out_shift;
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
inline void op_render_fb(FmOp &op, uint32_t n, int32_t out_shift) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    int32_t *out = op.out;
    int32_t fb1 = op.fb1, fb2 = op.fb2;
    for (uint32_t i = 0; i < n; i++) {
        // fb1/fb2 are raw table outputs (~+-32767), not gain-scaled at all --
        // FM_MOD_INPUT_SHIFT still applies (same phase-per-cycle gap as any
        // other modulation input) but self-feedback *depth* (real DX7's
        // separate 0-7 feedback-level parameter) is still no-op-or-full, a
        // known, separate, unresolved gap (patch.h's `feedback` is a bool) --
        // #57 doesn't claim to fix that here.
        int32_t fb_mod = (fb1 + fb2) >> 1;
        phase += inc;
        uint32_t idx = (phase + ((uint32_t)fb_mod << FM_MOD_INPUT_SHIFT)) >> FM_PHASE_SHIFT;
        int32_t sample = fm_sine_table[idx];
        out[i] += fm_mul_gain(sample, gain) >> out_shift;
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
        bool is_carrier = (r.out_bus[i] == FM_TARGET_OUT);
        op.in = (r.in_bus[i] == FM_BUS_ZERO) ? fm_zero_bus : bus.mod[r.in_bus[i]];
        op.out = is_carrier ? bus.out : bus.mod[r.out_bus[i]];
        int32_t out_shift = is_carrier ? FM_OUT_SHIFT_CARRIER : FM_OUT_SHIFT_MODULATOR;
        switch (r.kernel[i]) {
            case FM_KERNEL_FIRST:    op_render_first(op, n, out_shift); break;
            case FM_KERNEL_FEEDBACK: op_render_fb(op, n, out_shift);    break;
            default:                 op_render(op, n, out_shift);       break;
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
// `static_log2`: output level (TL) + velocity sensitivity, the two
// note-on-time-only pieces of #45's level chain (fm.md §5.6: both are
// "resolved once per note-on and never touched again"). `gain`/`gain_step`
// are deliberately NOT set here -- fm_voice_step_envelopes() sets them
// every block, starting from EG_LOG2_FLOOR (silence) on the very first
// block of this note, so there is no separate "initial gain" to get right
// here.
inline void fm_voice_note_on(FmOp ops[FM_NUM_OPS], const FmPatch &patch,
                              uint32_t note_inc, int16_t amplitude) {
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
        op.static_log2 = DX7_LEVEL_TO_LOG2[p.output_level] + eg_vel_sensitivity_log2(p.vel_sensitivity, amplitude);
    }
}

// Note-off: releases every operator's EG (env_dx.h's env_dx_release() --
// jump to stage 4 from wherever it currently is). Every operator releases,
// not just carriers -- a modulator's own decay shapes the carrier's timbre
// for as long as the carrier is still sounding (fm.md's EP patch: op4's
// fast decay is what makes the carrier's tone dim after the attack).
inline void fm_voice_note_off(FmOp ops[FM_NUM_OPS]) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        env_dx_release(ops[i].eg);
    }
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

// Re-derives every operator's `inc` from the note's current (possibly
// re-bent) base increment, without touching phase/gain/feedback history --
// called every buffer for a held voice so pitch bend stays live, while
// note-on-only state (the routing itself, each EG's stage) is untouched
// between notes.
inline void fm_voice_update_pitch(FmOp ops[FM_NUM_OPS], const FmPatch &patch, uint32_t note_inc) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        ops[i].inc = fm_op_inc(patch.op[i], note_inc);
    }
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
inline void fm_voice_step_envelopes(FmOp ops[FM_NUM_OPS], const FmPatch &patch, uint32_t n) {
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        const FmOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        int32_t log2_start, log2_end;
        env_dx_step_block(op.eg, p, n, log2_start, log2_end);
        int32_t gain_start = eg_to_linear(p.level, op.static_log2 + log2_start);
        int32_t gain_end = eg_to_linear(p.level, op.static_log2 + log2_end);
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
inline void fm_render_voice(FmOp ops[FM_NUM_OPS], const FmPatch &patch, const FmRouting &r,
                             const FmVoiceBuses &bus, int16_t pan,
                             int32_t *dry_l, int32_t *dry_r, uint32_t frames) {
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > FM_BLOCK) n = FM_BLOCK;
        fm_voice_step_envelopes(ops, patch, n);
        fm_voice_render_block(ops, r, bus, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = bus.out[i];
            dry_l[done + i] += (s * gain_l) >> 15;
            dry_r[done + i] += (s * gain_r) >> 15;
        }
        done += n;
        if (!fm_voice_active(ops, r)) break;
    }
}
