#pragma once

#include "engine.h"
#include "chip_module.h"
#include "note_freq.h"       // AY_CLOCK_ZX
#include "speaker_sim.h"
#include "chip/sid_voice.h"
#include "chip/sid_filter.h"
#include "chip/ay_osc.h"
#include "chip/ay_envelope.h"
#include "patch/multi_out.h"
#include "patch/receiver.h"
#include <arm_acle.h>
#include <cmath>
#include <cstdint>

// The chip engine's per-buffer render (module_chip.md §1), with no
// hardware access -- shared by Core 1's audio_engine_run() and the host
// tests, so both run the same code.
//
// A note copies its Part's cooked patch when it starts
// (PartReceiverT::note_patch()): the cook it was played with, or a later
// one of the same Patch, or the previous cook if the note belongs to the
// Patch that cook replaced. If the note's cook hasn't arrived yet the note
// waits a buffer, so a note gets every edit made before it. Every chip
// parameter is note-on, so the frame VM plays a sounding note's copy
// unchanged.
//
// The mix is mono: each voice is scaled by its Part's volume (a Part's pan
// doesn't apply), the Multi's FX run on the share the Parts send, and the
// speaker sim follows the FX.
//
// One SidVoice per MAX_VOICES slot, dispatched by VoiceType like the
// groovebox: VT_SILENT is skipped, VT_SID renders (render_mask also skips a
// VT_SID slot once it's both ungated and fully decayed, so CPU cost tracks
// currently-audible voices rather than every slot allocation has ever
// touched -- see the render_mask comment below).
//
// Retrigger uses env.hard_restart() (§4.3's default: instantaneous reset,
// not the 6581's 1-2 frame delay bug, which isn't modelled) rather than
// gate_on()'s "only attacks if not already gated" -- the dynamic allocator
// can steal a slot that's still mid-release from a *different* note (§8's
// three-tier policy), so the attack must restart even though the envelope
// is neither idle nor freshly gated-off.
//
// Filter buses (module_chip.md §5, §7.2). Rendering is sub-blocked so bus
// accumulators are sized FILTER_BUS_COUNT x sub-block, not FILTER_BUS_COUNT
// x SAMPLES_PER_BUFFER. CHIP_SUBBLOCK is also the frame-tick cut point (see
// below), not a separate sample-exact one.
//
// A bus with zero voices routed to it this sub-block skips its filter tick
// entirely, derived directly from the same per-voice scan the render loop
// already does. bus_was_active tracks the 0->nonzero wake transition so a
// just-woken bus's SidFilter state resets instead of inheriting a stale
// resonant tail from whatever voice last fed it.
//
// The frame table VM (module_chip.md §6) rides CHIP_SUBBLOCK rather than a
// separate sample-exact cut point: <=64 samples (1.45 ms on a 20 ms frame)
// of jitter against a true 50 Hz grid, which only affects wave/pulse/
// filter/vibrato state, not the audio-critical bus round trip §7.2 already
// sub-blocks for.
//
// FilterBusParams (engine_base.h) is unused by chip's own rendering: a
// bound bus's cutoff/resonance/mode come from the *feeding* voice's own
// patch (bus_feeder below).
//
// Not wired: §4.4's per-voice sub-oscillator (mod_acc/mod_inc/mod_mode)
// exists only in the rig and the CHIP_STRICT harness's adjacency topology,
// not in the real engine's per-voice state. WaveRow's sync/ring flag bits
// (instrument.h) are consequently reserved, not read -- wiring them needs
// the sub-oscillator built first. sync_reset/ring_msb_flip stay 0.
static constexpr uint32_t CHIP_SUBBLOCK = 64;
static constexpr uint32_t FRAME_PERIOD_SAMPLES = SAMPLE_RATE / 50;   // §6.2: 50 Hz PAL default

// Per-voice frame-VM state.
struct ChipVm {
    uint8_t  wave_pos, pulse_pos, filter_pos;
    uint8_t  pulse_hold, filter_hold;      // frames left in the current sweep row
    int16_t  pulse_delta, filter_delta;    // that row's delta, reapplied until hold expires
    int32_t  pulse_cur, filter_cutoff_cur; // current swept values
    int8_t   wave_note_offset;             // last wave-table row's semitone offset
    uint16_t vib_phase;
    uint16_t frames_since_trigger;
    bool     gate_off_fired;
};

// Per-voice AY frame-VM state (module_chip.md §12.3), same role ChipVm
// plays for SID.
struct AyVm {
    uint8_t  tone_pos, volume_pos;
    uint8_t  volume_hold;         // frames left in the current volume-sweep row
    int16_t  volume_delta;        // that row's delta, reapplied until hold expires
    int32_t  volume_cur;          // current software-envelope level, 0-15
    int8_t   tone_note_offset;    // last tone-table row's semitone offset
    uint16_t vib_phase;
    uint16_t frames_since_trigger;
    bool     gate_off_fired;
};

struct ChipRenderState {
    PartReceiverT<ChipCookedPatch> part[CHIP_PART_COUNT];

    // Per-voice state
    ChipCookedPatch voice_patch[MAX_VOICES];  // the note's own copy of its Patch
    uint8_t voice_part[MAX_VOICES];
    VoiceType voice_type[MAX_VOICES];         // the chip that copy names: what the voice plays
    SidVoice voice[MAX_VOICES];
    EnvSidRates rates;
    uint8_t last_trigger[MAX_VOICES];
    uint32_t acc_scale;
    ChipVm vm[MAX_VOICES];
    uint32_t frame_sample_pos;   // global frame-tick counter, sample-driven not buffer-driven

    // x/12 semitone ratios in Q16, -24..+24, computed once at init (powf
    // isn't constexpr).
    uint32_t semitone_ratio_q16[49];

    SidFilter bus_filter[FILTER_BUS_COUNT];
    bool      bus_was_active[FILTER_BUS_COUNT];
    int32_t   bus_acc[FILTER_BUS_COUNT][CHIP_SUBBLOCK];

    // The Multi's cooked mix and FX, the Parts' mix (mono: dry_l and
    // cut_l), the shared FX, and the mix they leave for the speaker sim.
    ChipMultiCooked multi;
    MultiOut out;
    int32_t mix[SAMPLES_PER_BUFFER];
    SidSpeakerStage speaker;   // module_chip.md §10

    // AY-3-8910/YM2149 voices (module_chip.md §12.1). One AyTone/AyNoise/
    // AyEnvelope trio per slot, independent -- not the real chip's shared-
    // noise/shared-envelope-across-3-channels topology. VT_AY reuses the
    // same MAX_VOICES pool and voice_alloc as VT_SID; a slot holds one or
    // the other, never both, decided at note-on.
    AyTone     ay_tone[MAX_VOICES];
    AyNoise    ay_noise[MAX_VOICES];
    AyEnvelope ay_env[MAX_VOICES];
    uint32_t   ay_tick_scale;   // Q16 ticks/sample, clock/8 internal rate (ay_osc.h)
    uint32_t   ay_tick_phase;   // shared across every AY voice -- same clock,
                                 // same sample rate, so the tick-count sequence
                                 // for a given sample is identical for all of them
    AyVm ay_vm[MAX_VOICES];

    // Display telemetry (engine.h) and the active-voice bitmap for Core 0.
    ChipVoiceUiState voice_ui[MAX_VOICES];
    uint32_t active_mask;
};

// Expects res2p_init() to have run (the speaker sim's peak stage).
inline void chip_render_init(ChipRenderState &s) {
    for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) s.part[p] = PartReceiverT<ChipCookedPatch>{};
    s.multi = multi_cooked_default<CHIP_PART_COUNT>();
    s.acc_scale = sid_acc_scale_q8(SID_CLOCK_PAL, SAMPLE_RATE);
    s.rates = env_sid_make_rates(SID_CLOCK_PAL, SAMPLE_RATE);
    for (int i = -24; i <= 24; i++)
        s.semitone_ratio_q16[i + 24] = (uint32_t)(powf(2.0f, (float)i / 12.0f) * 65536.0f + 0.5f);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        s.voice[v].init(SID_MODEL_6581);   // 6581 only -- 8580 not modelled yet
        s.voice_patch[v] = ChipCookedPatch{};
        s.voice_part[v] = 0;
        s.voice_type[v] = VT_SILENT;
        s.last_trigger[v] = 0;  // matches VoiceParams' default trigger -- a never-triggered voice must not look new
        s.vm[v] = ChipVm{};
        s.ay_vm[v] = AyVm{};
        s.voice_ui[v] = ChipVoiceUiState{ VT_SILENT, false, 0, 0 };
    }
    s.frame_sample_pos = 0;
    for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++) {
        s.bus_filter[b].init();
        s.bus_was_active[b] = false;
    }
    s.out.init();
    s.speaker.init((float)SAMPLE_RATE);
    s.ay_tick_scale = ay_tick_scale_q16(AY_CLOCK_ZX, (double)SAMPLE_RATE);
    s.ay_tick_phase = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        s.ay_tone[v].init();
        s.ay_noise[v].init();
        s.ay_env[v].init();
    }
    s.active_mask = 0;
}

// Take `part`'s newest cooked patch from its slot, if one was published.
// Call once per Part before each chip_render_buffer(), after latching the
// voice block.
inline void chip_render_take_patch(ChipRenderState &s, uint32_t part, PartSlotT<ChipCookedPatch> &slot) {
    s.part[part].take(slot);
}

// Take the newest cooked Multi from its slot, if one was published. Call
// once before each chip_render_buffer().
inline void chip_render_take_multi(ChipRenderState &s, PartSlotT<ChipMultiCooked> &slot) {
    slot.consume(s.multi);
}

// Add sample `x` of a voice (or a filter bus) with Part gains `g` to the
// mix at `i`: scaled by the Part's volume, and its unsent share also into
// the cut.
inline void chip_mix_scaled(MultiOut &out, uint32_t i, int32_t x, const PartGains &g) {
    const int32_t y = (int32_t)(((int64_t)x * g.l) >> 15);
    out.dry_l[i] += y;
    if (g.cut_q15) out.cut_l[i] += (int32_t)(((int64_t)y * g.cut_q15) >> 15);
}

// Whether gains `g` leave a voice as it is: unity volume, all of it sent.
inline bool chip_gains_unity(const PartGains &g) { return g.l == 32768 && g.cut_q15 == 0; }

// Fresh AY instrument: tables start from row 0, volume seeded from the
// instrument's initial_volume (ignored if use_envelope -- the hardware
// envelope owns level entirely then). Mirrors chip_vm_reset()'s shape.
inline void chip_ay_vm_reset(ChipRenderState &st, uint32_t v, const ChipCookedPatch &ins) {
    AyVm &s = st.ay_vm[v];
    s.tone_pos = 0; s.volume_pos = 0;
    s.volume_hold = 0;
    s.volume_delta = 0;
    s.volume_cur = ins.ay_initial_volume;
    s.tone_note_offset = 0;
    s.vib_phase = 16384;   // center, not trough -- same reasoning as chip_vm_reset()'s
                            // own comment: tri(16384) = 0, rising, so vibrato eases
                            // out from zero deviation instead of snapping to one edge
    s.frames_since_trigger = 0;
    s.gate_off_fired = false;
}

// One frame boundary's worth of table stepping for one AY voice, mirroring
// chip_vm_frame_tick()'s shape. base_period is the trigger-time register
// value (p.ay_period); arpeggio/vibrato both divide it rather than adding, since
// AY's period is inversely proportional to pitch (note_freq.h's
// ay_hz_to_period) -- an additive delta that works for SID's
// roughly-linear freq_reg would wobble asymmetrically here, sharper on one
// side than the other.
inline void chip_ay_vm_frame_tick(ChipRenderState &st, uint32_t v, const ChipCookedPatch &ins, uint16_t base_period) {
    AyVm &s = st.ay_vm[v];
    s.frames_since_trigger++;

    // The wave table's notes are the arpeggio; its waveform bits don't apply.
    if (ins.wave.len > 0) {
        const WaveRow &r = ins.wave.rows[s.tone_pos];
        s.tone_note_offset = r.note;
        s.tone_pos++;
        if (s.tone_pos >= ins.wave.len)
            s.tone_pos = (ins.wave.loop < ins.wave.len) ? ins.wave.loop : (uint8_t)(ins.wave.len - 1);
    }

    // The pulse table is the software volume envelope.
    if (!ins.ay_use_envelope && ins.pulse.len > 0) {
        if (s.volume_hold == 0) {
            const SweepRow &r = ins.pulse.rows[s.volume_pos];
            s.volume_delta = r.delta;
            s.volume_hold = r.duration;
            s.volume_pos++;
            if (s.volume_pos >= ins.pulse.len)
                s.volume_pos = (ins.pulse.loop < ins.pulse.len) ? ins.pulse.loop : (uint8_t)(ins.pulse.len - 1);
        }
        s.volume_cur += s.volume_delta;
        if (s.volume_cur < 0) s.volume_cur = 0;
        if (s.volume_cur > 15) s.volume_cur = 15;
        if (s.volume_hold > 0) s.volume_hold--;
    }

    // Arpeggio: exact, via division by the same Q16 semitone-ratio table
    // SID's own vibrato/arpeggio multiply by -- multiplying Hz by a ratio is
    // dividing period by the same ratio, so this reuses the table as-is
    // rather than needing an inverted copy of it.
    uint32_t period = base_period;
    if (s.tone_note_offset != 0) {
        int off = s.tone_note_offset;
        if (off < -24) off = -24;
        if (off > 24) off = 24;
        period = (uint32_t)(((uint64_t)period << 16) / st.semitone_ratio_q16[off + 24]);
    }
    // Vibrato: proportional to the *current* period, not a fixed offset on
    // it -- period is inversely related to pitch, so a fixed absolute
    // delta would be a tiny relative pitch shift at a large (low-pitch)
    // period and a huge one at a small (high-pitch) period. Scaling the
    // delta by `period` itself keeps the fractional deviation constant
    // across the whole pitch range instead, same as arpeggio's own
    // divide-by-ratio above. Still an uncalibrated raw wobble scale, not
    // cents.
    if (ins.vibrato_depth > 0 && s.frames_since_trigger >= ins.vibrato_delay) {
        s.vib_phase = (uint16_t)(s.vib_phase + ins.vibrato_speed * 64u);
        int32_t tri = (s.vib_phase < 32768)
                          ? (int32_t)s.vib_phase - 16384
                          : 49152 - (int32_t)s.vib_phase;
        int32_t delta = (int32_t)(((int64_t)period * tri * ins.vibrato_depth) >> 24);
        int32_t signed_period = (int32_t)period - delta;
        period = (uint32_t)(signed_period < 1 ? 1 : signed_period);
    }
    if (period > 4095) period = 4095;
    if (period < 1) period = 1;
    st.ay_tone[v].set_period((uint16_t)period);

    if (ins.gate_off_timer > 0 && !s.gate_off_fired && s.frames_since_trigger >= ins.gate_off_timer) {
        s.gate_off_fired = true;
    }
}

// Fresh instrument: first-frame waveform immediately (before any tick),
// filter cutoff seeded from the instrument's init value, everything else
// zeroed so the tables start from row 0.
inline void chip_vm_reset(ChipRenderState &st, uint32_t v, const ChipCookedPatch &ins) {
    ChipVm &s = st.vm[v];
    s.wave_pos = 0; s.pulse_pos = 0; s.filter_pos = 0;
    s.pulse_hold = 0; s.filter_hold = 0;
    s.pulse_delta = 0; s.filter_delta = 0;
    s.pulse_cur = ins.pulse_init;
    s.filter_cutoff_cur = ins.filter_cutoff_init;
    s.wave_note_offset = 0;
    // 16384, not 0: the triangle mapping below has tri(phase=0) = -16384,
    // the *trough*, not the center -- starting there means vibrato jumps
    // straight to the bottom of its swing the instant the delay expires,
    // instead of easing out from zero deviation. tri(16384) = 0, rising.
    s.vib_phase = 16384;
    s.frames_since_trigger = 0;
    s.gate_off_fired = false;
    st.voice[v].osc.waveform = ins.first_wave;
    st.voice[v].osc.pw = (uint16_t)s.pulse_cur;
}

// One frame boundary's worth of table stepping for one voice (module_chip.md §6).
inline void chip_vm_frame_tick(ChipRenderState &st, uint32_t v, const ChipCookedPatch &ins, uint32_t base_freq) {
    ChipVm &s = st.vm[v];
    SidVoice &voice = st.voice[v];
    s.frames_since_trigger++;

    if (ins.wave.len > 0) {
        const WaveRow &r = ins.wave.rows[s.wave_pos];
        if (r.waveform != WAVE_HOLD) voice.osc.waveform = r.waveform;
        s.wave_note_offset = r.note;
        s.wave_pos++;
        if (s.wave_pos >= ins.wave.len)
            s.wave_pos = (ins.wave.loop < ins.wave.len) ? ins.wave.loop : (uint8_t)(ins.wave.len - 1);
    }

    if (ins.pulse.len > 0) {
        if (s.pulse_hold == 0) {
            const SweepRow &r = ins.pulse.rows[s.pulse_pos];
            s.pulse_delta = r.delta;
            s.pulse_hold = r.duration;
            s.pulse_pos++;
            if (s.pulse_pos >= ins.pulse.len)
                s.pulse_pos = (ins.pulse.loop < ins.pulse.len) ? ins.pulse.loop : (uint8_t)(ins.pulse.len - 1);
        }
        s.pulse_cur += s.pulse_delta;
        if (s.pulse_cur < 0) s.pulse_cur = 0;
        if (s.pulse_cur > 4095) s.pulse_cur = 4095;
        if (s.pulse_hold > 0) s.pulse_hold--;
        voice.osc.pw = (uint16_t)s.pulse_cur;
    }

    if (ins.uses_filter && ins.filter.len > 0) {
        if (s.filter_hold == 0) {
            const SweepRow &r = ins.filter.rows[s.filter_pos];
            s.filter_delta = r.delta;
            s.filter_hold = r.duration;
            s.filter_pos++;
            if (s.filter_pos >= ins.filter.len)
                s.filter_pos = (ins.filter.loop < ins.filter.len) ? ins.filter.loop : (uint8_t)(ins.filter.len - 1);
        }
        s.filter_cutoff_cur += s.filter_delta;
        if (s.filter_cutoff_cur < 0) s.filter_cutoff_cur = 0;
        if (s.filter_cutoff_cur > 2047) s.filter_cutoff_cur = 2047;
        if (s.filter_hold > 0) s.filter_hold--;
    }

    // Arpeggio (wave-table note offset) and vibrato both apply as ratios on
    // the base register rather than recomputing from a note number, so
    // they compose for free with whatever pitch bend Core 0 already baked
    // into base_freq. Vibrato is a frame-stepped triangle LFO, not
    // smoothed. vibrato_depth is a raw register-wobble scale, not
    // calibrated to cents/semitones -- a by-ear tuning item, not a claim
    // of correctness.
    uint32_t freq = base_freq;
    if (s.wave_note_offset != 0) {
        int off = s.wave_note_offset;
        if (off < -24) off = -24;
        if (off > 24) off = 24;
        freq = (uint32_t)(((uint64_t)freq * st.semitone_ratio_q16[off + 24]) >> 16);
    }
    if (ins.vibrato_depth > 0 && s.frames_since_trigger >= ins.vibrato_delay) {
        s.vib_phase = (uint16_t)(s.vib_phase + ins.vibrato_speed * 64u);
        int32_t tri = (s.vib_phase < 32768)
                          ? (int32_t)s.vib_phase - 16384
                          : 49152 - (int32_t)s.vib_phase;
        int32_t delta = (tri * (int32_t)ins.vibrato_depth) >> 12;
        int32_t signed_freq = (int32_t)freq + delta;
        freq = (uint32_t)(signed_freq < 0 ? 0 : signed_freq);
    }
    if (freq > 65535) freq = 65535;
    voice.osc.inc = sid_freq_to_inc((uint16_t)freq, st.acc_scale);

    if (ins.gate_off_timer > 0 && !s.gate_off_fired && s.frames_since_trigger >= ins.gate_off_timer) {
        voice.env.gate_off();
        s.gate_off_fired = true;
    }
}

// The filter bus voice `v` feeds, or BUS_NONE: the one Core 0 bound for
// its note, if the voice's own Patch copy filters.
inline uint8_t chip_voice_bus(const ChipRenderState &s, const VoiceParams &p, uint32_t v) {
    return s.voice_patch[v].uses_filter ? p.filter_bus : BUS_NONE;
}

// Render one buffer from `vp` and the Parts' patches into `out`
// (SAMPLES_PER_BUFFER interleaved stereo int16 frames, the mono mix
// duplicated).
inline void chip_render_buffer(ChipRenderState &s, const VoiceParamBlock &vp, int16_t *out) {
    s.out.begin();
    int32_t *dry = s.out.dry_l;

    // Per voice, once per buffer: start a new note on its own copy of its
    // Part's patch, apply the gate, and decide whether it renders. Its
    // Part's gains apply to every sample it renders this buffer.
    // Instrument-driven waveform/pw/filter come from chip_vm_reset()/
    // chip_vm_frame_tick(), not from VoiceParams -- there is no raw
    // waveform/pw/ad/sr in VoiceParams (§6).
    uint32_t active_mask = 0;
    uint32_t render_mask = 0;
    PartGains gains[MAX_VOICES];
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        const VoiceParams &p = vp.voices[v];
        if (p.type == VT_SILENT) continue;  // never played

        const bool start = p.trigger != s.last_trigger[v];
        if (start) {
            const uint8_t part = p.part < CHIP_PART_COUNT ? p.part : 0;
            const ChipCookedPatch *patch = s.part[part].note_patch(p.load_id, p.cook_seq);
            if (!patch) continue;  // its cook arrives with a later buffer
            s.last_trigger[v] = p.trigger;
            s.voice_patch[v] = *patch;
            s.voice_part[v] = part;
            s.voice_type[v] = patch->type;
        }
        const ChipCookedPatch &ins = s.voice_patch[v];

        if (s.voice_type[v] == VT_AY) {
            // Apply the instrument once on a fresh trigger, raw base period
            // immediately -- chip_ay_vm_frame_tick() is the ongoing
            // authority on ay_tone[v]'s period from the next frame tick on,
            // so setting it unconditionally every buffer here would stomp
            // arpeggio/vibrato on every buffer that doesn't also contain a
            // frame tick.
            if (start) {
                s.ay_tone[v].init();
                s.ay_noise[v].init();
                s.ay_env[v].init();
                s.ay_tone[v].set_period(p.ay_period);
                if (ins.ay_noise_on) s.ay_noise[v].set_period(ins.ay_noise_period);
                if (ins.ay_use_envelope) {
                    s.ay_env[v].set_period(ins.ay_envelope_period);
                    s.ay_env[v].set_shape(ins.ay_envelope_shape);
                }
                chip_ay_vm_reset(s, v, ins);
            }

            // Real AY-3-8910 hardware has no gate/release concept at the
            // chip level (ay_instrument.h's header note) -- gate_off_timer
            // is software's own answer, same guard shape as SID's
            // vm[v].gate_off_fired (below): without it, the next buffer
            // would still see p.gate true and never re-arm renders.
            bool renders = p.gate && !s.ay_vm[v].gate_off_fired;
            if (renders) {
                active_mask |= (1u << v);
                render_mask |= (1u << v);
                gains[v] = s.out.part_gains_mono(s.multi.part[s.voice_part[v]]);
            }
            // AY voices report telemetry through the same field -- tone_pos
            // doubles as wave_pos (ChipVoiceUiState's own comment).
            s.voice_ui[v] = renders ? ChipVoiceUiState{VT_AY, p.gate, s.voice_part[v], s.ay_vm[v].tone_pos}
                                    : ChipVoiceUiState{VT_SILENT, false, 0, 0};
            continue;
        }

        SidVoice &voice = s.voice[v];
        voice.env.set_adsr(ins.ad, ins.sr);
        voice.velocity = p.velocity;

        if (start) {
            voice.env.hard_restart();
            // Raw base pitch, immediately -- chip_vm_frame_tick() is the
            // ongoing authority on osc.inc from here on (arpeggio/vibrato
            // included). Reassigning it unconditionally every buffer here
            // would stomp whatever chip_vm_frame_tick() had just computed
            // back to the unmodulated base on every buffer that doesn't
            // also contain a frame tick.
            voice.osc.inc = sid_freq_to_inc(p.sid_freq, s.acc_scale);
            chip_vm_reset(s, v, ins);
        }
        // vm[v].gate_off_fired guards against re-triggering: gate_off_timer
        // (in chip_vm_frame_tick) calls env.gate_off() directly, which
        // clears the envelope's own internal gate flag -- but the MIDI gate
        // (p.gate) is still true for as long as the key is held, so without
        // this guard the very next buffer would see p.gate=true, find the
        // envelope no longer gated, and call gate_on() right back -- a
        // spurious full re-attack immediately after every timed
        // auto-release. chip_vm_reset() clears the flag on a genuine new
        // trigger, so a fresh note's own gate-off timer still works.
        if (p.gate && !s.vm[v].gate_off_fired) voice.env.gate_on();
        else if (!p.gate)                      voice.env.gate_off();

        // voice_alloc's three-tier steal policy (§8) needs this bit to mean
        // "still audible" (counter > 0), not "this slot has a VT_SID note
        // nominally assigned" -- the latter never clears, so every voice
        // would read as permanently active and priority 1/2 (silent/
        // released steals) would never fire, forcing every allocation to
        // fall through to stealing the oldest held note.
        if (voice.env.counter > 0) active_mask |= (1u << v);

        // render_mask (module_chip.md §8): p.type is never reset to
        // VT_SILENT on note-off -- dynamic allocation only ever flips
        // p.gate, reusing the same slot on its next note. Without this,
        // every voice, bus feed and frame-VM table included, that was ever
        // played this session would keep ticking -- CPU cost would track
        // the high-water mark of voice slots used, not currently audible
        // voices. `|| p.gate` (not just counter > 0, unlike active_mask
        // above) matters for the sample right after a hard_restart: counter
        // is reset to 0 there and only ramps up as tick() runs through the
        // attack, so counter alone would skip a note's own opening samples.
        bool renders = p.gate || voice.env.counter > 0;
        if (renders) {
            render_mask |= (1u << v);
            gains[v] = s.out.part_gains_mono(s.multi.part[s.voice_part[v]]);
        }

        s.voice_ui[v] = renders ? ChipVoiceUiState{VT_SID, p.gate, s.voice_part[v], s.vm[v].wave_pos}
                                : ChipVoiceUiState{VT_SILENT, false, 0, 0};
    }

    for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_SUBBLOCK) {
        uint32_t n = SAMPLES_PER_BUFFER - base;
        if (n > CHIP_SUBBLOCK) n = CHIP_SUBBLOCK;

        // Frame tick: sample-counted, not buffer-edge-quantised (§6.1),
        // riding CHIP_SUBBLOCK's granularity rather than a separate
        // sample-exact cut.
        s.frame_sample_pos += n;
        bool frame_tick = false;
        while (s.frame_sample_pos >= FRAME_PERIOD_SAMPLES) {
            s.frame_sample_pos -= FRAME_PERIOD_SAMPLES;
            frame_tick = true;
        }
        if (frame_tick) {
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                if (!(render_mask & (1u << v))) continue;
                const VoiceParams &p = vp.voices[v];
                if (s.voice_type[v] == VT_SID) chip_vm_frame_tick(s, v, s.voice_patch[v], p.sid_freq);
                else chip_ay_vm_frame_tick(s, v, s.voice_patch[v], p.ay_period);
            }
        }

        for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++)
            for (uint32_t i = 0; i < n; i++) s.bus_acc[b][i] = 0;

        uint32_t bus_hits[FILTER_BUS_COUNT] = {0};
        uint32_t bus_feeder[FILTER_BUS_COUNT];   // valid only where bus_hits[b] > 0
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            // SID only -- an AY voice would fall into the dry-mix branch
            // below and call SidVoice::tick() on a slot that was never a
            // SidVoice this trigger.
            if (!(render_mask & (1u << v)) || s.voice_type[v] != VT_SID) continue;

            uint8_t bus = chip_voice_bus(s, vp.voices[v], v);
            if (bus < FILTER_BUS_COUNT) {
                for (uint32_t i = 0; i < n; i++) s.bus_acc[bus][i] += s.voice[v].tick(s.rates, false, 0);
                bus_hits[bus]++;
                bus_feeder[bus] = v;
            } else if (chip_gains_unity(gains[v])) {
                for (uint32_t i = 0; i < n; i++) dry[base + i] += s.voice[v].tick(s.rates, false, 0);
            } else {
                for (uint32_t i = 0; i < n; i++) chip_mix_scaled(s.out, base + i, s.voice[v].tick(s.rates, false, 0), gains[v]);
            }
        }

        for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++) {
            if (bus_hits[b] == 0) { s.bus_was_active[b] = false; continue; }
            if (!s.bus_was_active[b]) { s.bus_filter[b].init(); s.bus_was_active[b] = true; }

            // A bus is bound to one Part (module_chip.md §5.2): sharing only
            // applies to that Part's own notes, so the feeding voice's own
            // patch is this bus's tonal source, and its Part's gains its
            // gains.
            const uint32_t feeder = bus_feeder[b];
            const ChipCookedPatch &ins = s.voice_patch[feeder];
            int16_t f_half = sid_filter_f_half(SID_MODEL_6581, (uint16_t)s.vm[feeder].filter_cutoff_cur);
            int32_t q = sid_filter_q(SID_MODEL_6581, ins.filter_resonance);
            if (chip_gains_unity(gains[feeder])) {
                for (uint32_t i = 0; i < n; i++)
                    dry[base + i] += s.bus_filter[b].tick(s.bus_acc[b][i], f_half, q, ins.filter_mode_mask, /*saturate=*/true);
            } else {
                for (uint32_t i = 0; i < n; i++)
                    chip_mix_scaled(s.out, base + i,
                                    s.bus_filter[b].tick(s.bus_acc[b][i], f_half, q, ins.filter_mode_mask, /*saturate=*/true),
                                    gains[feeder]);
            }
        }

        // AY voices (module_chip.md §12.1): no filter bus, straight to
        // the mix. Tick-phase is shared across every AY voice -- same
        // clock, same sample rate, so the tick-count sequence for a given
        // sample is identical for all of them.
        uint32_t ay_ticks[CHIP_SUBBLOCK];
        bool any_ay = false;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            if ((render_mask & (1u << v)) && s.voice_type[v] == VT_AY) { any_ay = true; break; }
        }
        if (any_ay) {
            uint32_t phase = s.ay_tick_phase;
            for (uint32_t i = 0; i < n; i++) {
                phase += s.ay_tick_scale;
                ay_ticks[i] = phase >> 16;
                phase &= 0xffffu;
            }
            s.ay_tick_phase = phase;

            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                if (!(render_mask & (1u << v)) || s.voice_type[v] != VT_AY) continue;
                const ChipCookedPatch &ins = s.voice_patch[v];
                const bool unity = chip_gains_unity(gains[v]);

                for (uint32_t i = 0; i < n; i++) {
                    uint32_t ticks = ay_ticks[i];
                    s.ay_tone[v].advance(ticks);
                    if (ins.ay_noise_on) s.ay_noise[v].advance(ticks);
                    if (ins.ay_use_envelope) s.ay_env[v].advance(ticks);

                    // ay_mix()'s gate bit toggles 0/1 for every real
                    // tone/noise combination (only the documented "both
                    // disabled" quirk makes it a true constant) -- bipolar-
                    // izing it here (0/1 -> -1/+1) recovers the AC content
                    // the mix needs to sum sensibly with SID's own bipolar
                    // (w - wave_zero()) voices, at the cost of that one
                    // quirk becoming a half-scale residual DC bias instead
                    // of a full one. AY_STRICT's render_ay.cpp (host tool)
                    // does not do this -- it compares against ayumi's own
                    // raw (unipolar, DC-filtered separately) output, so it
                    // must not apply a correction the reference doesn't.
                    uint8_t tone_out = ins.ay_tone_on ? s.ay_tone[v].out : 0;
                    uint8_t t_off = ins.ay_tone_on ? 0 : 1;
                    uint8_t noise_bit = ins.ay_noise_on ? s.ay_noise[v].out() : 0;
                    uint8_t n_off_bit = ins.ay_noise_on ? 0 : 1;
                    uint8_t gate = (uint8_t)((tone_out | t_off) & (noise_bit | n_off_bit));
                    // ay_vm[v].volume_cur is the live level either way:
                    // seeded from the initial volume at trigger
                    // (chip_ay_vm_reset) and, if the patch has a volume
                    // table, stepped by chip_ay_vm_frame_tick() every frame
                    // -- the static case (no table) just never advances it
                    // past its starting value, so this one read covers both.
                    int8_t level = ins.ay_use_envelope ? s.ay_env[v].level
                                                        : (int8_t)(s.ay_vm[v].volume_cur * 2 + 1);
                    float amp = ay_dac_table(ins.ay_model)[level];   // module_chip.md §12.4: per-patch model
                    float bipolar = gate ? 1.0f : -1.0f;

                    // Scale to roughly a SID voice's own raw peak magnitude
                    // (~2-3e5 typical, sid_voice.h's (w - wave_zero()) *
                    // amp) so AY and SID voices sum into one mix without one
                    // silently dominating -- a first guess, not a
                    // calibrated one.
                    constexpr float AY_MIX_SCALE = 700000.0f;
                    const int32_t x = (int32_t)(bipolar * amp * AY_MIX_SCALE);
                    if (unity) dry[base + i] += x;
                    else chip_mix_scaled(s.out, base + i, x, gains[v]);
                }
            }
        }
    }

    // The Multi's FX on the share the Parts send, and its master. The
    // speaker sim (module_chip.md §10) follows, upstream of the final
    // safety clamp below -- its own soft clip is the cone-breakup
    // character, not that clamp. Its preset is a single global choice,
    // replicated identically across every voice by input_subsystem.cpp on
    // CC17, read once here from voice 0.
    s.out.finish_mono(s.multi.fx, s.multi.master_q15, s.mix);
    s.speaker.apply_preset(vp.voices[0].speaker_preset);

    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        // Mono, duplicated -- module_chip.md §10: the module is
        // authentically mono.
        int32_t v = (int32_t)s.speaker.tick((float)(s.mix[i] >> SID_MIX_SHIFT));
        int16_t smp = (int16_t)__ssat(v, 16);
        *out++ = smp;
        *out++ = smp;
    }

    s.active_mask = active_mask;
}
