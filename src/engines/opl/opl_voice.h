#pragma once

#include "../fm/op.h"  // FmOp, op_render/op_render_first/op_render_fb, fm_voice_render_block, FmVoiceBuses -- reused unchanged
#include "env_opl.h"
#include "patch.h"
#include "waveforms.h"
#include <cmath>

// The hand-written OPL voice glue: note-on/note-off/envelope-step/active/
// render, mirroring the shape of ../fm/op.h's own fm_voice_note_on()/
// fm_voice_step_envelopes()/fm_voice_note_off()/fm_voice_active()/
// fm_render_voice(), but for OplPatch/EnvOpl instead of FmPatch/EnvDX. Those
// FM functions aren't reused directly here: they take a `const FmPatch&`
// (DX7-shaped patch data) and read/write `FmOp::eg` as a concrete `EnvDX`,
// neither of which fits OPL's own patch or envelope types. What IS reused
// unchanged is everything below that layer -- FmOp itself, the three
// per-sample kernels, and fm_voice_render_block()'s routing interpreter.
//
// A voice's real operator count (2 or 4) is `routing.num_ops`, set once at
// note-on from the patch's chosen Algorithm (patch.h's OPL_ROUTINGS[]) --
// every function below loops `i < routing.num_ops` rather than a hardcoded
// bound, so the same glue drives 2-op and 4-op voices alike. Operator slots
// past `num_ops` (up to FM_NUM_OPS=6) are simply never visited by the
// per-sample kernel -- not computed at zero gain, just skipped, so they cost
// nothing per sample. `env`/`ops` are always allocated 4/6 wide regardless
// of which patch is currently playing on that voice slot; a voice that
// switches between a 2-op and a 4-op patch across notes leaves stale state
// in the unused upper slots, but nothing ever reads it, since every loop
// below is bounded by the *current* note's own `routing.num_ops`.

inline void opl_voice_init_inert(FmOp ops[FM_NUM_OPS]) {
    for (uint8_t i = 4; i < FM_NUM_OPS; i++) {
        FmOp &op = ops[i];
        op.phase = 0;
        op.inc = 0;
        op.base_inc = 0;
        op.gain = 0;
        op.gain_step = 0;
        op.fb1 = 0;
        op.fb2 = 0;
        op.in = fm_zero_bus;
        op.out = nullptr;
        op.table = opl_wave_sine;
    }
}

// Fixed-rate vibrato applied to both operators together, scaled by the mod
// wheel. Real OPL2 hardware has one global fixed-rate vibrato LFO, not a
// per-patch-configurable one, so this is a small purpose-built
// implementation rather than a general LFO module.
static constexpr float OPL_VIBRATO_HZ = 6.1f;
static constexpr float OPL_VIBRATO_MAX_CENTS = 20.0f;

struct OplVibrato {
    uint32_t phase;
};

inline float opl_vibrato_step(OplVibrato &vib, uint32_t n, int16_t mod_wheel) {
    if (mod_wheel == 0) return 0.0f;  // resting wheel: skip the sinf() call, not just its result
    static const uint32_t inc = (uint32_t)((OPL_VIBRATO_HZ / (float)SAMPLE_RATE) * 4294967296.0);
    vib.phase += inc * n;
    float s = sinf(2.0f * (float)M_PI * (float)vib.phase / 4294967296.0f);
    float depth = (float)mod_wheel / 32767.0f;
    return s * depth * OPL_VIBRATO_MAX_CENTS;
}

// Resolves `patch`'s algorithm into `routing` (a copy of one of patch.h's
// OPL_ROUTINGS[] literals -- 2-op or 4-op, whichever the patch picked) and
// triggers every real operator's phase/table and envelope, `routing.num_ops`
// of them. `routing` is a per-voice copy, not a pointer straight at the
// constexpr literal, because the patch's own feedback amount (not
// compile-time) still needs patching into kernel[0]/fb_shift[0]/
// clear_bus_mask below -- always op0, 2-op or 4-op alike (see OplPatch's
// `feedback` field comment, patch.h).
inline void opl_voice_note_on(FmOp ops[FM_NUM_OPS], EnvOpl env[4], FmRouting &routing,
                               const OplPatch &patch, uint32_t note_inc, int16_t amplitude,
                               uint8_t midinote, OplVibrato &vib) {
    routing = opl_routing_for(patch.algorithm);
    if (patch.feedback > 0) {
        routing.kernel[0] = FM_KERNEL_FEEDBACK;
        routing.fb_shift[0] = (uint8_t)(8 - patch.feedback);
        routing.clear_bus_mask |= (uint8_t)(1u << routing.out_bus[0]);
    }

    vib.phase = 0;
    for (uint8_t i = 0; i < routing.num_ops; i++) {
        const OplOpParams &p = patch.op[i];
        FmOp &op = ops[i];
        op.phase = 0;
        op.base_inc = (uint32_t)((float)note_inc * opl_mult_table[p.mult & 15]);
        op.inc = op.base_inc;
        op.gain = 0;
        op.gain_step = 0;
        op.fb1 = 0;
        op.fb2 = 0;
        op.in = fm_zero_bus;
        op.out = nullptr;  // assigned per sub-block by fm_voice_render_block()
        op.table = opl_waveform_table(p.ws);
        env_opl_init(env[i], p, midinote, amplitude);
    }
}

inline void opl_voice_note_off(EnvOpl env[4], const FmRouting &routing) {
    for (uint8_t i = 0; i < routing.num_ops; i++) env_opl_release(env[i]);
}

// A voice is active only while at least one of its CARRIERS (operators that
// reach the final mix, routing.out_bus[i] == FM_TARGET_OUT) still has an
// envelope running. Reads the separate `env` array rather than `FmOp::eg` --
// that field's type is hardcoded to EnvDX, so it never holds this engine's
// own envelope state.
inline bool opl_voice_active(const EnvOpl env[4], const FmRouting &routing) {
    for (uint8_t i = 0; i < routing.num_ops; i++) {
        if (routing.out_bus[i] == FM_TARGET_OUT && env[i].active()) return true;
    }
    return false;
}

inline void opl_voice_step_envelopes(FmOp ops[FM_NUM_OPS], EnvOpl env[4], const FmRouting &routing, uint32_t n) {
    for (uint8_t i = 0; i < routing.num_ops; i++) {
        int32_t level_start = env[i].level;
        int32_t level_end = env_opl_step_block(env[i], n);
        int32_t gain_start = eg_to_gain(level_start);
        int32_t gain_end = eg_to_gain(level_end);
        ops[i].gain = gain_start;
        ops[i].gain_step = (gain_end - gain_start) / (int32_t)n;
    }
}

// Renders `frames` samples in FM_BLOCK-sized sub-blocks, panning the shared
// output bus into dry_l/dry_r (accumulate -- callers with multiple voices
// must clear dry_l/dry_r once up front). Stops early once opl_voice_active()
// goes false mid-buffer, mirroring ../fm/op.h's own fm_render_voice().
inline void opl_render_voice(FmOp ops[FM_NUM_OPS], EnvOpl env[4], const FmRouting &routing,
                              const FmVoiceBuses &bus, int16_t pan, int32_t *dry_l, int32_t *dry_r,
                              uint32_t frames, OplVibrato &vib, int16_t mod_wheel) {
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > FM_BLOCK) n = FM_BLOCK;

        // Always reassigns `inc` (even back to `base_inc` at pitch_ratio==1.0)
        // rather than skipping the write when cents==0 -- a held note whose
        // vibrato depth (mod wheel) drops back to 0 mid-note must have its
        // increment reset, not left at its last nonzero deviation.
        float cents = opl_vibrato_step(vib, n, mod_wheel);
        float pitch_ratio = (cents != 0.0f) ? exp2f(cents * (1.0f / 1200.0f)) : 1.0f;
        for (uint8_t i = 0; i < routing.num_ops; i++) {
            ops[i].inc = (uint32_t)((float)ops[i].base_inc * pitch_ratio);
        }

        opl_voice_step_envelopes(ops, env, routing, n);
        fm_voice_render_block<OPL_TABLE_BITS>(ops, routing, bus, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = bus.out[i] >> FM_VOICE_OUT_SHIFT;
            dry_l[done + i] += (s * gain_l) >> 15;
            dry_r[done + i] += (s * gain_r) >> 15;
        }
        done += n;
        if (!opl_voice_active(env, routing)) break;
    }
}
