#pragma once

#include <cmath>
#include <cstdint>

// FM P0 measurement rig (#42, module_fm.md §11 "P0 -- measure"): a stripped
// operator mixer -- N voices x 6 operators, fixed phase increments, fixed
// gains/gain-steps, no EG, no LFO, no patch logic, no MIDI. This produces a
// build whose numbers the *next* slice reads; it does not decide anything
// itself (module_fm.md's provisional MAX_VOICES=16, table size, BLOCK size, etc.
// all stay exactly as #41 left them -- this file is a separate, self-
// contained rig, not a modification of the real engine skeleton).
//
// Every module_fm.md §3.6 tuning lever is a compile-time switch (Makefile/CMake,
// same pattern as speech's SPEECH_PROFILE -- see the Makefile's FM_RIG_*
// comments):
//   FM_RIG_VOICES        -- voice count (default comfortably past the
//                            predicted 12-21 ceiling, module_fm.md §3.4)
//   FM_RIG_BLOCK          -- sub-block length: 8 / 16 / 32
//   FM_RIG_TABLE_BITS     -- table size: 10 (1024) or 12 (4096, default)
//   FM_RIG_INTERLEAVE     -- 0/1: interleave the op0/op1 pair in one loop
//                            body instead of two sequential calls
//   FM_RIG_NOT_IN_FLASH   -- 0 (default): inlined, flash -- unchanged from
//                            #42. 1: SRAM, via __no_inline_not_in_flash_func
//                            (plain __not_in_flash_func measured zero effect
//                            in #43 -- it targets an out-of-line symbol that
//                            inlining had already erased, so noinline is
//                            required to give it something to place). 2:
//                            noinline, still flash -- a control so diffing 2
//                            against 1 isolates the SRAM-vs-flash placement
//                            effect from the call/return overhead noinline
//                            itself adds. (The table itself is already a
//                            runtime-generated, non-const array -- always
//                            SRAM/.bss, never flash/.rodata, so this axis
//                            only ever measured code placement.)
//   FM_RIG_SMULWB         -- 0/1: fuse the gain>>8 + mul step with the M33
//                            DSP extension's __smulwb (arm_acle.h); host
//                            builds get a portable equivalent expression for
//                            correctness, not the real instruction
//   FM_RIG_FB             -- 0/1 (default 1): op3 uses op_render_fb
//                            (self-feedback) when 1, or plain op_render when
//                            0 -- lets #43's bench session diff self-feedback
//                            against a plain operator on an otherwise
//                            identical topology (module_fm.md §3.3's 22-vs-17-cycle
//                            budget), which the fixed topology alone can't
//                            isolate since op3 was always op_render_fb.

#ifndef FM_RIG_VOICES
#define FM_RIG_VOICES 24
#endif
#ifndef FM_RIG_BLOCK
#define FM_RIG_BLOCK 16
#endif
#ifndef FM_RIG_TABLE_BITS
#define FM_RIG_TABLE_BITS 12
#endif
#ifndef FM_RIG_INTERLEAVE
#define FM_RIG_INTERLEAVE 0
#endif
#ifndef FM_RIG_NOT_IN_FLASH
#define FM_RIG_NOT_IN_FLASH 0
#endif
#ifndef FM_RIG_SMULWB
#define FM_RIG_SMULWB 0
#endif
#ifndef FM_RIG_FB
#define FM_RIG_FB 1
#endif

// arm_acle.h only exists for ARM targets -- the host build (tools/host_render)
// compiles this same header on x86, so it must never see this include even
// when FM_RIG_SMULWB=1 asks for the intrinsic (fm_rig_mul_gain() below falls
// back to a portable equivalent expression whenever __ARM_FEATURE_DSP isn't
// defined, which covers both "device build without the M33 DSP extension"
// and "host build entirely").
#if FM_RIG_SMULWB && defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#endif

static constexpr uint32_t FM_RIG_TABLE_SIZE = 1u << FM_RIG_TABLE_BITS;
static constexpr uint32_t FM_RIG_PHASE_SHIFT = 32 - FM_RIG_TABLE_BITS;

// Own table, independent of #41's sine_tab.h (FM_TABLE_BITS is fixed at 12
// there -- the real engine's decided value). This rig's table size is itself
// one of the levers being bench-tested, so it needs to be independently
// configurable without touching the already hardware-verified #41 skeleton.
inline int16_t fm_rig_table[FM_RIG_TABLE_SIZE];

inline void fm_rig_init_table() {
    uint32_t quarter = FM_RIG_TABLE_SIZE / 4;
    for (uint32_t i = 0; i <= quarter; i++) {
        fm_rig_table[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / (float)FM_RIG_TABLE_SIZE));
    }
    for (uint32_t i = 1; i < quarter; i++) {
        fm_rig_table[2 * quarter - i] = fm_rig_table[i];
    }
    for (uint32_t i = 0; i < 2 * quarter; i++) {
        fm_rig_table[2 * quarter + i] = (int16_t)(-fm_rig_table[i]);
    }
}

inline uint32_t fm_rig_phase_inc(float freq_hz, float sample_rate_hz) {
    return (uint32_t)((freq_hz / sample_rate_hz) * 4294967296.0);
}

// The host build never defines the real pico-sdk macro; device builds pull
// it in from pico/platform.h before this header. Same fallback pattern as
// src/engines/tracker/mixer.h.
#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif
#ifndef __no_inline_not_in_flash_func
#define __no_inline_not_in_flash_func(func) func
#endif
#ifndef __noinline
#define __noinline
#endif

// #43 finding: plain __not_in_flash_func() has NO effect here, because
// op_render() etc. are `inline` and fully inline into audio_engine_run() at
// -O3 -- the attribute places an *out-of-line* function's code in SRAM, and
// inlining erases the separate symbol before that ever applies (confirmed
// via objdump: FM_RIG_NOT_IN_FLASH=0 and =1 produced byte-identical .text).
// The pico-sdk documents this exact trap and ships the fix:
// __no_inline_not_in_flash_func() adds noinline so there's a real symbol for
// the section-placement attribute to act on.
//
//   FM_RIG_NOT_IN_FLASH=0 (default) -- inlined, flash. Unchanged from #42;
//     matches every reading already taken.
//   FM_RIG_NOT_IN_FLASH=1 -- noinline, SRAM (.time_critical). A genuine
//     placement test, but it also adds real call/return overhead that the
//     inlined default never pays -- comparing =1 against the default
//     baseline conflates "moved to SRAM" with "stopped being inlined."
//   FM_RIG_NOT_IN_FLASH=2 -- noinline, flash (control). Same call overhead
//     as =1, same code, only placement differs -- diff =2 against =1 to
//     isolate the SRAM-vs-flash effect alone.
#if FM_RIG_NOT_IN_FLASH == 1
#define FM_RIG_HOT(func) __no_inline_not_in_flash_func(func)
#elif FM_RIG_NOT_IN_FLASH == 2
#define FM_RIG_HOT(func) __noinline func
#else
#define FM_RIG_HOT(func) func
#endif

// module_fm.md §5.2's FmOp, plus the self-feedback history pair (fb1/fb2) that a
// real per-operator kernel-selection scheme would only allocate for
// operators actually using op_render_fb -- inlined into every operator here
// since this rig has a single fixed topology, not a compiled routing table.
struct FmRigOp {
    uint32_t phase;
    uint32_t inc;
    int32_t  gain;
    int32_t  gain_step;
    const int32_t *in;    // modulation bus, or fm_rig_zero_bus for no input
    int32_t *out;          // output bus
    int32_t  fb1, fb2;      // op_render_fb only: last two raw table outputs
};

// Read-only all-zero bus, shared by every "no external modulation" operator
// (module_fm.md §5.2: "points at a zero bus for pure carriers"). Sized to the
// largest configured BLOCK; never written.
inline int32_t fm_rig_zero_bus[FM_RIG_BLOCK];

// Multiply-by-gain-then-shift, shared by every kernel below. FM_RIG_SMULWB
// fuses the `gain >> 8` + multiply into one M33 DSP-extension instruction --
// (Rn * SignExtend16(Rm)) >> 16 -- which changes the net shift from >>22
// (8 + 14, the plain form) to >>16, so the final accumulate shift changes
// from >>14 to >>6 to keep the two forms numerically equivalent. There's no
// standalone SMULWB wrapper in this GCC's arm_acle.h (only __smlawb, the
// multiply-*accumulate* form) -- __smlawb(gain, sample, 0) is exactly
// SMULWB with its accumulate operand forced to zero, i.e. a plain multiply.
// Host builds get the same arithmetic via plain 64-bit multiply -- correct,
// not the real instruction, since arm_acle.h only exists on-target.
#if FM_RIG_SMULWB
inline int32_t fm_rig_mul_gain(int32_t sample, int32_t gain) {
#if defined(__ARM_FEATURE_DSP)
    return __smlawb(gain, sample, 0);
#else
    return (int32_t)(((int64_t)gain * (int16_t)sample) >> 16);
#endif
}
static constexpr int32_t FM_RIG_OUT_SHIFT = 6;
#else
inline int32_t fm_rig_mul_gain(int32_t sample, int32_t gain) {
    return (sample * (gain >> 8));
}
static constexpr int32_t FM_RIG_OUT_SHIFT = 14;
#endif

// Plain kernel: accumulates (+=) into `out` -- module_fm.md §3.2's 13-instruction
// listing is this loop body. Block-inner form: called once per operator per
// sub-block, so phase/gain/in/out stay resident for the whole inner loop.
inline void FM_RIG_HOT(op_render)(FmRigOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    const int32_t *in = op.in;
    int32_t *out = op.out;
    for (uint32_t i = 0; i < n; i++) {
        phase += inc;
        // Phase wrap is free: the shift alone produces exactly a
        // FM_RIG_TABLE_BITS-wide index, no mask, no modulo (module_fm.md §3.2).
        uint32_t idx = (phase + (uint32_t)in[i]) >> FM_RIG_PHASE_SHIFT;
        int32_t sample = fm_rig_table[idx];
        out[i] += fm_rig_mul_gain(sample, gain) >> FM_RIG_OUT_SHIFT;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// First-writer variant: stores instead of accumulating, eliminating the need
// to clear its target bus before this operator runs (module_fm.md §4.3/§5.2).
inline void FM_RIG_HOT(op_render_first)(FmRigOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    const int32_t *in = op.in;
    int32_t *out = op.out;
    for (uint32_t i = 0; i < n; i++) {
        phase += inc;
        uint32_t idx = (phase + (uint32_t)in[i]) >> FM_RIG_PHASE_SHIFT;
        int32_t sample = fm_rig_table[idx];
        out[i] = fm_rig_mul_gain(sample, gain) >> FM_RIG_OUT_SHIFT;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
}

// Self-feedback: DX7-style, the modulation input is this operator's own last
// two raw table outputs averaged, instead of an external bus (module_fm.md §5.2's
// "Self-feedback (DX7 2-sample average)" kernel variant). Accumulates (+=),
// same as op_render -- in this rig's fixed topology it's never the sole
// writer of its target bus (see fm_rig_render_voice_block()'s topology
// comment), so accumulating onto an already first-written bus is correct.
inline void FM_RIG_HOT(op_render_fb)(FmRigOp &op, uint32_t n) {
    uint32_t phase = op.phase;
    uint32_t inc = op.inc;
    int32_t gain = op.gain;
    int32_t gain_step = op.gain_step;
    int32_t *out = op.out;
    int32_t fb1 = op.fb1, fb2 = op.fb2;
    for (uint32_t i = 0; i < n; i++) {
        int32_t fb_mod = (fb1 + fb2) >> 1;
        phase += inc;
        uint32_t idx = (phase + (uint32_t)fb_mod) >> FM_RIG_PHASE_SHIFT;
        int32_t sample = fm_rig_table[idx];
        out[i] += fm_rig_mul_gain(sample, gain) >> FM_RIG_OUT_SHIFT;
        fb2 = fb1;
        fb1 = sample;
        gain += gain_step;
    }
    op.phase = phase;
    op.gain = gain;
    op.fb1 = fb1;
    op.fb2 = fb2;
}

// module_fm.md §3.6 lever 1: two independent operators' loop bodies interleaved
// into one loop, instead of two sequential op_render_first() calls -- the
// `lsrs` -> `ldrsh` -> `mul` chain in each is a serial load-use-dependent
// chain, and interleaving two independent ones gives the pipeline something
// else to do while each stalls. Both operands must be mutually independent
// (disjoint in/out buses, neither reads the other's output) -- true of the
// op0/op1 pair fm_rig_render_voice_block() below always passes here, not
// true in general, which is why this isn't just op_render_first() called
// twice in a loop of 2.
inline void FM_RIG_HOT(op_render_pair)(FmRigOp &a, FmRigOp &b, uint32_t n) {
    uint32_t pa = a.phase, pb = b.phase;
    uint32_t inca = a.inc, incb = b.inc;
    int32_t ga = a.gain, gb = b.gain;
    int32_t gsa = a.gain_step, gsb = b.gain_step;
    const int32_t *ina = a.in, *inb = b.in;
    int32_t *outa = a.out, *outb = b.out;
    for (uint32_t i = 0; i < n; i++) {
        pa += inca;
        pb += incb;
        uint32_t idxa = (pa + (uint32_t)ina[i]) >> FM_RIG_PHASE_SHIFT;
        uint32_t idxb = (pb + (uint32_t)inb[i]) >> FM_RIG_PHASE_SHIFT;
        int32_t sa = fm_rig_table[idxa];
        int32_t sb = fm_rig_table[idxb];
        outa[i] = fm_rig_mul_gain(sa, ga) >> FM_RIG_OUT_SHIFT;
        outb[i] = fm_rig_mul_gain(sb, gb) >> FM_RIG_OUT_SHIFT;
        ga += gsa;
        gb += gsb;
    }
    a.phase = pa; a.gain = ga;
    b.phase = pb; b.gain = gb;
}

// One voice's shared bus scratch (module_fm.md §4.3: "6 modulation buses + 1 output
// bus ... one shared scratch for the whole engine, not per-voice"). Callers
// own these and reuse the same arrays across every voice, sequentially --
// this struct just names the pointers so fm_rig_render_voice_block() doesn't
// take seven separate arguments.
struct FmRigBuses {
    int32_t *mod[6];
    int32_t *out;
};

// Fixed 6-operator topology for this rig (no patch, no DAG compiler -- #42
// is explicitly "no patch logic"). Every operator that ACCUMULATES onto a
// bus (op_render's `+=`, op_render_fb's `+=`) is guaranteed a preceding
// first-writer on that same bus earlier in this function -- the property
// the real engine's note-on-time routing compiler is responsible for, done
// by hand here since there's no compiler yet:
//
//   op0 -> bus[0]            first-writer  (op_render_first, in=zero)
//   op1 -> bus[1]            first-writer  (op_render_first, in=zero)
//                             -- op0/op1 interleaved when FM_RIG_INTERLEAVE
//   op2 -> bus[1] (+=)       accumulate    (op_render, in=bus[0])
//   op3 -> bus[1] (+=)       accumulate    (op_render_fb if FM_RIG_FB else
//                                           plain op_render, self-feedback)
//   op4 -> bus[3]            first-writer  (op_render_first, in=bus[1])
//   op5 -> OUT               first-writer  (op_render_first, in=bus[3])
//
// bus[2], bus[4], bus[5] are allocated (FmRigBuses always carries all six)
// but unused by this particular fixed chain -- a real algorithm with more
// parallel modulators would use them; this rig only needs enough routing
// variety to exercise all three kernels correctly.
inline void fm_rig_render_voice_block(FmRigOp ops[6], const FmRigBuses &bus, uint32_t n) {
    ops[0].in = fm_rig_zero_bus; ops[0].out = bus.mod[0];
    ops[1].in = fm_rig_zero_bus; ops[1].out = bus.mod[1];
#if FM_RIG_INTERLEAVE
    op_render_pair(ops[0], ops[1], n);
#else
    op_render_first(ops[0], n);
    op_render_first(ops[1], n);
#endif

    ops[2].in = bus.mod[0]; ops[2].out = bus.mod[1];
    op_render(ops[2], n);

    ops[3].out = bus.mod[1];
#if FM_RIG_FB
    op_render_fb(ops[3], n);
#else
    // Plain accumulate, unmodulated (in=zero) -- the direct comparison point
    // for isolating op_render_fb's self-feedback overhead: same position in
    // the chain, same accumulate-onto-bus[1] role, only the kernel differs.
    ops[3].in = fm_rig_zero_bus;
    op_render(ops[3], n);
#endif

    ops[4].in = bus.mod[1]; ops[4].out = bus.mod[3];
    op_render_first(ops[4], n);

    ops[5].in = bus.mod[3]; ops[5].out = bus.out;
    op_render_first(ops[5], n);
}

// Fixed increments/gains/gain-steps (module_fm.md P0: "fixed phase increments,
// fixed gains" -- no EG, no patch data). Each operator in the chain gets a
// harmonic-multiple frequency purely so the rendered tone isn't a plain
// sine (worth something for the host WAV's by-eye/by-ear sanity check);
// gain is a flat mid-scale modulation depth for every operator except the
// carrier (op5), which gets full scale so the final audio isn't quiet.
// gain_step is 0 throughout: the field and the kernels' `gain += gain_step`
// instruction still exist and still execute every sample (the actual cost
// being measured), it just never moves -- avoids any overflow/clipping
// bookkeeping a real EG ramp would need, which is explicitly out of scope
// here (module_fm.md P2, not P0).
inline void fm_rig_init_voice(FmRigOp voice_ops[6], float base_freq_hz, float sample_rate_hz) {
    // (sample * (gain >> 8)) >> 14 == sample (unity) when gain == 1<<22 --
    // gain >> 8 == 1<<14, cancelling the final >>14 exactly. Scaled down 1/64
    // from that unity point (1<<16 instead of 1<<22) so up to FM_RIG_VOICES
    // (default 24, up to a few dozen in practice) summed carriers stay well
    // clear of int16 saturation -- unlike the real engine, this rig has no
    // per-voice level/velocity to lean on, and a WAV that's flat-clipped the
    // whole way through can't tell a healthy render from a broken one.
    static constexpr int32_t MOD_GAIN = 1 << 15;      // ~0.5x of the scaled-down carrier -- modulation depth for ops0-4
    static constexpr int32_t CARRIER_GAIN = 1 << 16;  // scaled-down carrier (op5)
    for (int i = 0; i < 6; i++) {
        FmRigOp &op = voice_ops[i];
        op.phase = 0;
        op.inc = fm_rig_phase_inc(base_freq_hz * (float)(i + 1), sample_rate_hz);
        op.gain = (i == 5) ? CARRIER_GAIN : MOD_GAIN;
        op.gain_step = 0;
        op.in = fm_rig_zero_bus;
        op.out = nullptr;  // assigned per-call by fm_rig_render_voice_block()
        op.fb1 = 0;
        op.fb2 = 0;
    }
}

// Renders `frames` samples of `num_voices` (<= FM_RIG_VOICES) voices, mixed
// equally into both channels (no pan -- module_fm.md P0 doesn't need a stereo
// image, just a real per-sample cost). `voices` is FM_RIG_VOICES worth of
// persistent per-voice operator state (phase/gain/feedback history carried
// across calls); `bus` is the shared scratch, reused across every voice and
// every sub-block within this call -- proving the "not per-voice" claim by
// construction, since there is only one FmRigBuses passed in regardless of
// num_voices. No pico-sdk dependency, so both the device path
// (audio_engine.cpp's T00T_FM_PROFILE branch) and the host path
// (tools/host_render/render_fm_rig.cpp) call this exact function.
inline void fm_rig_render_buffer(FmRigOp (&voices)[FM_RIG_VOICES][6], const FmRigBuses &bus,
                                  uint32_t num_voices, int32_t *dry_l, int32_t *dry_r, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) { dry_l[i] = 0; dry_r[i] = 0; }

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > FM_RIG_BLOCK) n = FM_RIG_BLOCK;

        for (uint32_t v = 0; v < num_voices; v++) {
            fm_rig_render_voice_block(voices[v], bus, n);
            for (uint32_t i = 0; i < n; i++) {
                dry_l[done + i] += bus.out[i];
                dry_r[done + i] += bus.out[i];
            }
        }
        done += n;
    }
}
