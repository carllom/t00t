#pragma once

#include "engine.h"
#include "fm_module.h"
#include "op.h"
#include "patch/multi_out.h"
#include "patch/receiver.h"
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

    // The Parts' mix, the shared FX and the output.
    MultiOut out;
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
    s.out.init();
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

// Render one buffer from `vp` and the Parts' patches into `out`
// (SAMPLES_PER_BUFFER interleaved stereo int16 frames).
inline void fm_render_buffer(FmRenderState &s, const VoiceParamBlock &vp, int16_t *out) {
    s.out.begin();
    const FmVoiceBuses bus{ { s.bus_mod[0], s.bus_mod[1], s.bus_mod[2], s.bus_mod[3], s.bus_mod[4], s.bus_mod[5] },
                            s.bus_out };

    uint32_t active_mask = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        const VoiceParams &p = vp.voices[v];

        if (p.trigger != s.last_trigger[v]) {
            const uint8_t part = p.part < FM_PART_COUNT ? p.part : 0;
            const FmCookedPatch *patch = s.part[part].note_patch(p.load_id, p.cook_seq);
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

        const PartGains g = s.out.part_gains(s.multi.part[s.voice_part[v]]);
        fm_render_voice_gains<FM_TABLE_BITS, DxEnvGlue>(s.ops[v], c.patch, c.routing, bus, g.l, g.r,
                                                         s.out.dry_l, s.out.dry_r, SAMPLES_PER_BUFFER, &s.peg[v],
                                                         &s.lfo[v], p.phase_inc, p.mod_wheel,
                                                         g.cut_q15 ? s.out.cut_l : nullptr,
                                                         g.cut_q15 ? s.out.cut_r : nullptr, g.cut_q15);
        active_mask |= (1u << v);
    }
    s.active_mask = active_mask;

    s.out.finish(s.multi.fx, s.multi.master_q15, out);
}
