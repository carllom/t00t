#pragma once

#include "engine.h"
#include "fm_module.h"
#include "op.h"
#include "patch/receiver.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "fx/phaser.h"
#include "fx/flanger.h"
#include "fx/chorus.h"
#include "fx/bitcrusher.h"
#include "fx/overdrive.h"
#include "pan.h"
#include <arm_acle.h>
#include <cstdint>

// The FM engine's per-buffer render: every voice from its own copy of its
// Part's cooked patch, the Multi's mix and FX, and the stereo interleave,
// with no hardware access -- shared by Core 1's audio_engine_run() and the
// host tests, so both run the same code.
//
// A note copies its Part's cooked patch when it starts: from the Part's
// current cook if that is the cook the note was played with or a later one
// of the same Patch, from the Part's previous cook if the note belongs to
// the Patch that cook replaced. If the note's cook hasn't arrived yet the
// note waits a buffer, so a note gets every edit made before it. While its
// Part keeps the note's Patch, live edits reach the note's copy
// (FmModule::copy_live()); once the Part loads another Patch the note
// finishes on what it has.

struct FmRenderState {
    PartReceiverT<FmCookedPatch> part[FM_PART_COUNT];

    // Per-voice state
    FmCookedPatch voice_patch[MAX_VOICES];  // the note's own copy of its Patch
    uint8_t  voice_part[MAX_VOICES];
    uint16_t voice_load[MAX_VOICES];        // its Patch load: only that load's edits reach it
    bool     voice_started[MAX_VOICES];     // has a Patch with valid routing
    FmOp     ops[MAX_VOICES][FM_NUM_OPS];
    FmPitchEg peg[MAX_VOICES];
    FmLfo    lfo[MAX_VOICES];
    uint8_t  last_trigger[MAX_VOICES];
    bool     gated[MAX_VOICES];
    uint32_t active_mask;

    // The Multi's cooked mix and FX.
    FmMultiCooked multi;

    // Shared bus scratch -- reused across every voice, sequentially.
    int32_t bus_mod[FM_NUM_OPS][FM_BLOCK];
    int32_t bus_out[FM_BLOCK];

    // Stereo dry mix, the share of it the Parts don't send to the FX (only
    // written in a buffer where a sounding Part's send is below full), and
    // the mono FX send/return.
    int32_t dry_l[SAMPLES_PER_BUFFER];
    int32_t dry_r[SAMPLES_PER_BUFFER];
    int32_t cut_l[SAMPLES_PER_BUFFER];
    int32_t cut_r[SAMPLES_PER_BUFFER];
    int32_t fx_buf[SAMPLES_PER_BUFFER];

    FxDelay   fx_delay;
    FxReverb  fx_reverb;
    FxPhaser  fx_phaser;
    FxFlanger fx_flanger;
    FxChorus  fx_chorus;
    FxBitcrusher fx_bitcrusher;
    FxOverdrive  fx_overdrive;
    uint8_t  last_fx_type;
};

// Expects fm_init_sine_tab(), osc_init_sine() and env_dx_init_tables() to
// have run (shared global tables).
inline void fm_render_init(FmRenderState &s) {
    for (uint32_t p = 0; p < FM_PART_COUNT; p++) s.part[p] = PartReceiverT<FmCookedPatch>{};
    s.multi = multi_cooked_default<FM_PART_COUNT>();
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        s.voice_patch[v] = FmCookedPatch{};
        s.voice_part[v] = 0;
        s.voice_load[v] = 0;
        s.voice_started[v] = false;
        s.last_trigger[v] = 0;  // matches VoiceParams' default trigger -- a never-triggered voice must not look new
        s.gated[v] = false;
        // A zero-initialized EnvDX is not idle (stage 0 is a real stage).
        for (uint32_t i = 0; i < FM_NUM_OPS; i++) env_dx_reset(s.ops[v][i].eg);
        fm_pitch_eg_init(s.peg[v]);
        fm_lfo_init(s.lfo[v]);
    }
    s.active_mask = 0;
    s.fx_delay.init();
    s.fx_reverb.init();
    s.fx_phaser.init();
    s.fx_flanger.init();
    s.fx_chorus.init();
    s.fx_bitcrusher.init();
    s.fx_overdrive.init();
    s.last_fx_type = 0xFF;
}

// Take `part`'s newest cooked patch from its slot, if one was published:
// an edit reaches the live fields of the Part's notes on that Patch; a new
// Patch leaves the Part's sounding notes on the one they have. Call once
// per Part before each fm_render_buffer(), after latching the voice block.
inline void fm_render_take_patch(FmRenderState &s, uint32_t part, PartSlotT<FmCookedPatch> &slot) {
    if (s.part[part].take(slot) == PartTake::NONE) return;
    const PartReceiverT<FmCookedPatch> &rx = s.part[part];
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (s.voice_part[v] == part && s.voice_load[v] == rx.load_id()) {
            FmModule::copy_live(rx.current(), s.voice_patch[v]);
        }
    }
}

// Take the newest cooked Multi from its slot, if one was published. Call
// once before each fm_render_buffer().
inline void fm_render_take_multi(FmRenderState &s, PartSlotT<FmMultiCooked> &slot) {
    slot.consume(s.multi);
}

// Whether `r` has an operator writing the output bus. Without one a voice
// is silent; rendering it would mix the output bus another voice left.
inline bool fm_routing_has_carrier(const FmRouting &r) {
    for (uint32_t i = 0; i < FM_NUM_OPS; i++) {
        if (r.out_bus[i] == FM_TARGET_OUT) return true;
    }
    return false;
}

// Whether 16-bit counter `a` is later than `b`.
inline bool fm_seq_after(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }

// The cooked patch a note of Patch load `load_id`, played with cook
// `cook_seq`, starts on; null if that cook hasn't arrived yet.
inline const FmCookedPatch *fm_note_patch(const PartReceiverT<FmCookedPatch> &rx, uint16_t load_id,
                                          uint16_t cook_seq) {
    if (load_id == rx.load_id()) return fm_seq_after(cook_seq, rx.seq()) ? nullptr : &rx.current();
    if (load_id == rx.previous_load_id()) return &rx.previous();
    // A load Core 1 hasn't seen yet, or one replaced before it ever arrived.
    return fm_seq_after(load_id, rx.load_id()) ? nullptr : &rx.current();
}

// Render one buffer from `vp` and the Parts' patches into `out`
// (SAMPLES_PER_BUFFER interleaved stereo int16 frames).
inline void fm_render_buffer(FmRenderState &s, const VoiceParamBlock &vp, int16_t *out) {
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        s.dry_l[i] = 0;
        s.dry_r[i] = 0;
    }
    bool any_cut = false;
    const FmVoiceBuses bus{ { s.bus_mod[0], s.bus_mod[1], s.bus_mod[2], s.bus_mod[3], s.bus_mod[4], s.bus_mod[5] },
                            s.bus_out };

    uint32_t active_mask = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        const VoiceParams &p = vp.voices[v];

        if (p.trigger != s.last_trigger[v]) {
            const uint8_t part = p.part < FM_PART_COUNT ? p.part : 0;
            const FmCookedPatch *patch = fm_note_patch(s.part[part], p.load_id, p.cook_seq);
            if (!patch) continue;  // its cook arrives with a later buffer
            s.last_trigger[v] = p.trigger;
            s.voice_patch[v] = *patch;
            s.voice_part[v] = part;
            s.voice_load[v] = p.load_id;
            s.voice_started[v] = patch->routing.valid && fm_routing_has_carrier(patch->routing);
            if (s.voice_started[v]) {
                fm_voice_note_on<DxEnvGlue>(s.ops[v], s.voice_patch[v].patch, p.phase_inc, p.amplitude, p.note,
                                            &s.peg[v], &s.lfo[v]);
                // Released before it started (while waiting for its cook, or
                // within the batch that played it): release at once.
                if (!p.gate) fm_voice_note_off<DxEnvGlue>(s.ops[v], &s.peg[v]);
            }
            s.gated[v] = p.gate;
        } else if (!p.gate && s.gated[v]) {
            // Gate-off edge: release through the EGs.
            if (s.voice_started[v]) fm_voice_note_off<DxEnvGlue>(s.ops[v], &s.peg[v]);
            s.gated[v] = false;
        } else {
            s.gated[v] = p.gate;
        }
        if (!s.voice_started[v]) continue;
        const FmCookedPatch &c = s.voice_patch[v];

        // Keep rendering through release; stop once every carrier's EG has
        // gone idle, never on gate alone.
        if (!p.gate && !fm_voice_active(s.ops[v], c.routing)) continue;

        // The voice's stereo gains from its Part's pan and volume, and the
        // share of it not sent to the FX. At unity volume and full send the
        // gains are the pan gains exactly and nothing is cut.
        const PartMixCooked &mix = s.multi.part[s.voice_part[v]];
        int32_t gain_l, gain_r;
        pan_gains_q15((int16_t)mix.pan_q15, gain_l, gain_r);
        if (mix.volume_q15 != 32768) {
            gain_l = (gain_l * mix.volume_q15) >> 15;
            gain_r = (gain_r * mix.volume_q15) >> 15;
        }
        const int32_t cut_q15 = 32768 - mix.send_q15;
        if (cut_q15 && !any_cut) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                s.cut_l[i] = 0;
                s.cut_r[i] = 0;
            }
            any_cut = true;
        }

        fm_render_voice_gains<FM_TABLE_BITS, DxEnvGlue>(s.ops[v], c.patch, c.routing, bus, gain_l, gain_r,
                                                         s.dry_l, s.dry_r, SAMPLES_PER_BUFFER, &s.peg[v],
                                                         &s.lfo[v], p.phase_inc, p.mod_wheel,
                                                         cut_q15 ? s.cut_l : nullptr, cut_q15 ? s.cut_r : nullptr,
                                                         cut_q15);
        active_mask |= (1u << v);
    }
    s.active_mask = active_mask;

    // Post-mix effect, the Multi's FX TYPE. Mono send / stereo return:
    // downmix the sent share of the stereo dry mix to mono, run the effect
    // on it, then add its wet output to both channels. Clear the newly
    // selected effect's buffer on a type switch so a stale tail can't
    // resurface.
    const EffectParams &fx = s.multi.fx;
    bool has_fx = (fx.type == FX_DELAY   || fx.type == FX_REVERB ||
                   fx.type == FX_PHASER  || fx.type == FX_FLANGER ||
                   fx.type == FX_CHORUS  || fx.type == FX_BITCRUSHER ||
                   fx.type == FX_OVERDRIVE);
    if (fx.type != s.last_fx_type) {
        if (fx.type == FX_DELAY)        s.fx_delay.init();
        else if (fx.type == FX_REVERB)  s.fx_reverb.init();
        else if (fx.type == FX_PHASER)  s.fx_phaser.init();
        else if (fx.type == FX_FLANGER) s.fx_flanger.init();
        else if (fx.type == FX_CHORUS)  s.fx_chorus.init();
        else if (fx.type == FX_BITCRUSHER) s.fx_bitcrusher.init();
        else if (fx.type == FX_OVERDRIVE)  s.fx_overdrive.init();
        s.last_fx_type = fx.type;
    }
    if (has_fx) {
        // The sent share: the dry mix less what the Parts don't send.
        if (any_cut) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                s.dry_l[i] -= s.cut_l[i];
                s.dry_r[i] -= s.cut_r[i];
            }
        }
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            s.fx_buf[i] = (s.dry_l[i] + s.dry_r[i]) >> 1;
        }
        if (fx.type == FX_DELAY)        s.fx_delay.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_REVERB)  s.fx_reverb.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_PHASER)  s.fx_phaser.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_FLANGER) s.fx_flanger.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_CHORUS)  s.fx_chorus.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else if (fx.type == FX_BITCRUSHER) s.fx_bitcrusher.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
        else                                s.fx_overdrive.process(s.fx_buf, SAMPLES_PER_BUFFER, fx);
    }

    // Crossfade the sent share dry/wet by the mix knob (Q15) rather than
    // summing full dry underneath -- fx_buf above already carries mix as
    // its own gain, so FX MIX 127 is wet-only. The unsent share stays dry
    // at unity.
    const int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)fx.mix) * 258 : 32768;
    const bool add_cut = has_fx && any_cut;
    const int32_t master = s.multi.master_q15;
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        int32_t l, r;
        if (has_fx) {
            l = (int32_t)(((int64_t)dry_scale * s.dry_l[i]) >> 15) + s.fx_buf[i];
            r = (int32_t)(((int64_t)dry_scale * s.dry_r[i]) >> 15) + s.fx_buf[i];
            if (add_cut) {
                l += s.cut_l[i];
                r += s.cut_r[i];
            }
        } else {
            l = s.dry_l[i];
            r = s.dry_r[i];
        }
        if (master != 32768) {
            l = (int32_t)(((int64_t)l * master) >> 15);
            r = (int32_t)(((int64_t)r * master) >> 15);
        }
        *out++ = (int16_t)__ssat(l, 16);
        *out++ = (int16_t)__ssat(r, 16);
    }
}
