#pragma once

#include "engine.h"
#include "patch.h"
#include "patch/receiver.h"
#include "osc/oscillator.h"
#include "osc/sample.h"
#include "envelope.h"
#include "filter.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "fx/phaser.h"
#include "fx/flanger.h"
#include "fx/chorus.h"
#include "fx/bitcrusher.h"
#include "fx/overdrive.h"
#include "pan.h"
#include <cmath>
#include <arm_acle.h>

// The subtractive engine's per-buffer render: every voice, the post-mix
// effect and the stereo interleave, with no hardware access -- shared by
// Core 1's audio_engine_run() and the host renderers, so both run the same
// code.

// Mod-wheel vibrato: dedicated LFO, independent of the Patch's LFO.
static constexpr float   MOD_VIBRATO_HZ = 5.0f;
static constexpr int16_t MOD_VIBRATO_MAX_Q15 = 960;  // ~±50 cents at full mod

// Sub-block rendering (see module_tracker.md's "Rendering Pipeline" for the
// general pattern): the LFO value, envelope level, and SVF filter coefficient
// are each computed once per SUBBLOCK-sized chunk and linearly ramped across
// it — a unit of parameter constancy, not a unit of output. Oscillator phase
// advance, PolyBLEP, and the filter's own two-pass state update stay per-sample.
// Requires SAMPLES_PER_BUFFER to divide evenly so every sub-block is full
// size; no partial-block handling exists.
static constexpr uint32_t SUBBLOCK = 64;
static_assert(SAMPLES_PER_BUFFER % SUBBLOCK == 0,
              "sub-block rendering assumes SAMPLES_PER_BUFFER divides evenly by SUBBLOCK");

// Render-side state, owned solely by whoever calls sub_render_buffer()
// (Core 1 on the device).
struct SubRenderState {
    // Each Part's cooked patch, refreshed before each buffer by
    // sub_render_take_patch(). A voice's note-on fields arrive in its own
    // VoiceParams; it reads the live fields from its Part every buffer while
    // the Part holds the Patch load the note belongs to. Once the Part holds
    // another Patch, the voice is detached onto a frozen copy of its old
    // Patch's live fields and finishes on them.
    PartReceiverT<SubCookedPatch> part[SUB_PART_COUNT];
    SubLiveCooked voice_live[MAX_VOICES];   // used while voice_detached
    uint8_t voice_part[MAX_VOICES];
    uint16_t voice_load[MAX_VOICES];
    bool voice_detached[MAX_VOICES];

    // Per-voice runtime state
    uint32_t voice_phase[MAX_VOICES];
    float    lfo_phase[MAX_VOICES];
    float    mod_lfo_phase[MAX_VOICES];
    float    lfo_val_state[MAX_VOICES];   // last ramped LFO sample (Q15, as float), carried across sub-blocks
    float    f_half_state[MAX_VOICES];    // last ramped SVF F_half coefficient, carried across sub-blocks
    uint16_t noise_lfsr[MAX_VOICES];
    uint8_t  last_trigger[MAX_VOICES];
    bool     voice_gated[MAX_VOICES];
    Envelope envelope[MAX_VOICES];
    SVFilter filter[MAX_VOICES];

    // One fixed ADSR shared by every voice, plus its exact n-sample-ahead
    // decay/release coefficients (coeff^SUBBLOCK) for sub-block-rate
    // envelope advance.
    EnvConfig env_cfg;
    float decay_coeff_sub;
    float release_coeff_sub;

    // The Multi's cooked mix and FX, refreshed before each buffer by
    // sub_render_take_multi(). A voice mixes by its Part's volume, pan and
    // FX send.
    SubMultiCooked multi;

    // Stereo dry mix (int32_t to avoid overflow during summation); each voice's
    // contribution is split L/R by its Part's pan and scaled by its volume.
    // `cut_l`/`cut_r` collect the share of it the Parts don't send to the
    // FX -- only written in a buffer where a sounding Part's send is below
    // full (`any_cut`). `fx_buf` is the mono send/return scratch for the
    // post-mix effect (mono send / stereo return).
    int32_t dry_l[SAMPLES_PER_BUFFER];
    int32_t dry_r[SAMPLES_PER_BUFFER];
    int32_t cut_l[SAMPLES_PER_BUFFER];
    int32_t cut_r[SAMPLES_PER_BUFFER];
    int32_t fx_buf[SAMPLES_PER_BUFFER];

    // Post-mix effect state
    FxDelay   fx_delay;
    FxReverb  fx_reverb;
    FxPhaser  fx_phaser;
    FxFlanger fx_flanger;
    FxChorus  fx_chorus;
    FxBitcrusher fx_bitcrusher;
    FxOverdrive  fx_overdrive;
    uint8_t  last_fx_type;  // detect type switch to clear buffers
};

// Expects osc_init_sine() to have run (the sine wavetable is shared global state).
inline void sub_render_init(SubRenderState &s) {
    // 10ms attack, 100ms decay, 70% sustain, 800ms release
    s.env_cfg = env_config(10, 100, 70, 800);
    s.decay_coeff_sub   = powf(s.env_cfg.decay_coeff,   (float)SUBBLOCK);
    s.release_coeff_sub = powf(s.env_cfg.release_coeff, (float)SUBBLOCK);

    for (uint32_t p = 0; p < SUB_PART_COUNT; p++) s.part[p] = PartReceiverT<SubCookedPatch>{};
    s.multi = multi_cooked_default<SUB_PART_COUNT>();
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        s.voice_live[v] = SubLiveCooked{};
        s.voice_part[v] = 0;
        s.voice_load[v] = 0;
        s.voice_detached[v] = false;
        s.voice_phase[v] = 0;
        s.lfo_phase[v] = 0.0f;
        s.mod_lfo_phase[v] = 0.0f;
        s.lfo_val_state[v] = 0.0f;
        s.f_half_state[v] = 0.0f;
        s.noise_lfsr[v] = 0xACE1u;
        s.last_trigger[v] = 0;
        s.voice_gated[v] = false;
        s.envelope[v].init();
        s.filter[v].init();
    }

    s.fx_delay.init();
    s.fx_reverb.init();
    s.fx_phaser.init();
    s.fx_flanger.init();
    s.fx_chorus.init();
    s.fx_bitcrusher.init();
    s.fx_overdrive.init();
    s.last_fx_type = 0xFF;
}

// Take `part`'s newest cooked patch from its slot, if one was published.
// Call once per Part before each sub_render_buffer(), after latching the
// voice block.
inline void sub_render_take_patch(SubRenderState &s, uint32_t part, PartSlotT<SubCookedPatch> &slot) {
    if (s.part[part].take(slot) != PartTake::LOAD) return;
    const PartReceiverT<SubCookedPatch> &rx = s.part[part];
    const SubLiveCooked &old_live = rx.previous().live;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        // A note on the newly arrived load (started while it was still
        // pending) stays attached.
        if (s.voice_part[v] == part && !s.voice_detached[v] && s.envelope[v].active() &&
            s.voice_load[v] != rx.load_id()) {
            s.voice_live[v] = old_live;
            s.voice_detached[v] = true;
        }
    }
}

// Take the newest cooked Multi from its slot, if one was published. Call
// once before each sub_render_buffer().
inline void sub_render_take_multi(SubRenderState &s, PartSlotT<SubMultiCooked> &slot) {
    slot.consume(s.multi);
}

// Bitmap of voices whose envelope is still sounding.
inline uint32_t sub_render_active_mask(const SubRenderState &s) {
    uint32_t bitmap = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (s.envelope[v].active()) bitmap |= (1u << v);
    }
    return bitmap;
}

// Render one buffer from `vp` and `s.part` into `out`
// (SAMPLES_PER_BUFFER interleaved stereo int16 frames).
inline void sub_render_buffer(SubRenderState &s, const VoiceParamBlock &vp, int16_t *out) {
    // Clear the stereo dry mix
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        s.dry_l[i] = 0;
        s.dry_r[i] = 0;
    }
    bool any_cut = false;

    // Render all voices
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        const VoiceParams &p = vp.voices[v];
        const uint8_t part_index = p.part < SUB_PART_COUNT ? p.part : 0;
        const PartReceiverT<SubCookedPatch> &part = s.part[part_index];
        const SubNoteOnCooked &note_on = p.note_on;

        // Detect new note (trigger changed)
        if (p.trigger != s.last_trigger[v]) {
            s.last_trigger[v] = p.trigger;
            // A note committed just before its Part took another Patch
            // belongs to the previous load: it starts detached on that
            // Patch's live fields. Otherwise it follows the Part.
            const bool on_previous = p.load_id != part.load_id() && p.load_id == part.previous_load_id();
            const SubLiveCooked &start_live = on_previous ? part.previous().live : part.current().live;
            s.voice_part[v] = part_index;
            s.voice_load[v] = p.load_id;
            s.voice_detached[v] = on_previous;
            s.voice_live[v] = start_live;
            s.voice_phase[v] = 0;
            s.lfo_phase[v] = 0.0f;
            s.mod_lfo_phase[v] = 0.0f;
            s.lfo_val_state[v] = 0.0f;  // LFO restarts at phase 0 -> sine = 0
            // Filter coefficient seeded from the note's starting cutoff
            // (envelope and LFO are both 0 at trigger, so no mod applies
            // yet) so the first sub-block doesn't glide from a stale
            // coefficient left over from a previous note.
            s.f_half_state[v] = (note_on.filter_mode != FILTER_OFF) ? start_live.f_half_seed : 0.0f;
            s.noise_lfsr[v] = 0xACE1u;
            s.envelope[v].trigger();
            s.filter[v].init();  // clean filter state on new note
            s.voice_gated[v] = true;
        }

        // Detect gate-off edge
        if (!p.gate && s.voice_gated[v]) {
            s.envelope[v].release(s.env_cfg);
            s.voice_gated[v] = false;
        } else {
            s.voice_gated[v] = p.gate;
        }

        // Skip silent voices
        if (!s.envelope[v].active()) continue;

        const SubLiveCooked &c = s.voice_detached[v] ? s.voice_live[v] : part.current().live;

        // Render in sub-blocks (see the SUBBLOCK comment above): LFO
        // value, envelope level, and the SVF F_half coefficient are each
        // computed once per sub-block and linearly ramped across it.
        // Filter Q stays a per-buffer constant below — resonance is
        // never modulated, so there is nothing to ramp. Pitch does not
        // ramp: eff_phase_inc is recomputed once per sub-block, from the
        // sub-block's target LFO/vibrato values (not the ramped ones),
        // and held fixed across it — a stepped frequency change, not a
        // click-prone amplitude one.
        uint32_t phase = s.voice_phase[v];
        float lfo_ph = s.lfo_phase[v];
        float mod_ph = s.mod_lfo_phase[v];
        bool has_lfo = c.has_lfo;
        bool has_mod = (p.mod_depth > 0);
        bool has_filter = (note_on.filter_mode != FILTER_OFF);

        // Patch-derived constants come cooked (patch.h), live ones from the Part
        const float lfo_phase_inc = c.lfo_phase_inc;
        const int16_t lfo_depth_q15 = c.lfo_depth_q15;
        const int16_t lfo_pitch_q15 = c.lfo_pitch_q15;
        const int16_t lfo_pwm_q15 = c.lfo_pwm_q15;
        const int16_t lfo_filt_hz = c.lfo_filt_hz;  // Hz, signed

        // Per-voice constants the per-sample loop reads, copied to locals:
        // read through `p`/`note_on`/`c` there, they cost a pointer reload
        // from the stack plus the field load on every sample.
        const Waveform waveform = note_on.waveform;
        const SampleDef *const sample_def = (waveform == WAVE_SAMPLE) ? note_on.sample : nullptr;
        const FilterMode filter_mode = note_on.filter_mode;
        const int32_t amplitude = p.amplitude;
        const uint16_t duty_cycle = c.duty_cycle;
        const int32_t filt_q = c.filter_q;

        // Dedicated mod-wheel vibrato LFO increment
        const float mod_phase_inc = MOD_VIBRATO_HZ / (float)SAMPLE_RATE;

        // This voice's stereo gains from its Part's pan and volume, and the
        // share of it not sent to the FX (constant across the buffer). At
        // unity volume and full send the gains are the pan gains exactly
        // and nothing is cut.
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

        float lfo_cur = s.lfo_val_state[v];
        float fhalf_cur = s.f_half_state[v];
        bool sample_ended = false;

        for (uint32_t blk = 0; blk < SAMPLES_PER_BUFFER && !sample_ended; blk += SUBBLOCK) {
            if (!s.envelope[v].active()) break;
            uint32_t n = SUBBLOCK;

            // --- Envelope: exact O(1) advance by n samples, ramped within the block ---
            float env_start = s.envelope[v].level;
            float env_target = s.envelope[v].advance_block(s.env_cfg, s.decay_coeff_sub, s.release_coeff_sub, n);
            float env_delta = (env_target - env_start) / (float)n;
            float env_ramp = env_start;

            // --- LFO: sample the value n samples ahead, ramp toward it ---
            float lfo_target = 0.0f;
            if (has_lfo) {
                float lfo_ph_end = lfo_ph + lfo_phase_inc * (float)n;
                if (lfo_ph_end >= 1.0f) lfo_ph_end -= 1.0f;
                lfo_target = (float)osc_sine((uint32_t)(lfo_ph_end * (float)PHASE_CYCLE));
                lfo_ph = lfo_ph_end;
            }
            float lfo_delta = (lfo_target - lfo_cur) / (float)n;
            float lfo_ramp = lfo_cur;
            int32_t lfo_target_i = (int32_t)lfo_target;

            // --- Pitch: stepped once per sub-block, never ramped ---
            uint32_t eff_phase_inc = p.phase_inc;
            if (lfo_pitch_q15 > 0) {
                int32_t pitch_mod = (lfo_target_i * lfo_pitch_q15) >> 15;
                eff_phase_inc = (uint32_t)((int32_t)p.phase_inc +
                    (((int32_t)p.phase_inc * pitch_mod) >> 15));
            }
            if (has_mod) {
                float mod_ph_end = mod_ph + mod_phase_inc * (float)n;
                if (mod_ph_end >= 1.0f) mod_ph_end -= 1.0f;
                int32_t mod_sine = osc_sine((uint32_t)(mod_ph_end * (float)PHASE_CYCLE));
                mod_ph = mod_ph_end;
                int32_t vib = (mod_sine * p.mod_depth) >> 15;       // scale by wheel
                int32_t frac = (vib * MOD_VIBRATO_MAX_Q15) >> 15;   // ±max depth
                eff_phase_inc = (uint32_t)((int32_t)eff_phase_inc +
                    (((int32_t)eff_phase_inc * frac) >> 15));
            }

            // --- SVF F_half: recomputed from ramped cutoff/resonance
            // parameters (never interpolated coefficient-to-coefficient)
            // at each sub-block boundary, then ramped like LFO/envelope ---
            float fhalf_target = fhalf_cur;
            if (has_filter) {
                int32_t level_target_q15 = (int32_t)(env_target * 32767.0f);
                int32_t cutoff = (int32_t)c.filter_cutoff;
                if (c.filter_env_amount != 0) {
                    cutoff += (level_target_q15 * (int32_t)c.filter_env_amount) >> 15;
                }
                if (lfo_filt_hz != 0) {
                    cutoff += (lfo_target_i * lfo_filt_hz) >> 15;
                }
                if (cutoff < 20) cutoff = 20;
                if (cutoff > 18000) cutoff = 18000;
                fhalf_target = (float)svf_compute_f_half(cutoff);
            }
            float fhalf_delta = (fhalf_target - fhalf_cur) / (float)n;
            float fhalf_ramp = fhalf_cur;

            for (uint32_t i = 0; i < n; i++) {
                int32_t lfo_val = (int32_t)lfo_ramp;

                // LFO → duty cycle (PWM): offset duty_cycle
                uint16_t eff_duty = duty_cycle;
                if (lfo_pwm_q15 > 0) {
                    int32_t pwm_mod = (lfo_val * lfo_pwm_q15) >> 15;
                    int32_t d = (int32_t)duty_cycle + pwm_mod;
                    if (d < 1) d = 1;
                    if (d > 1022) d = 1022;
                    eff_duty = (uint16_t)d;
                }

                int32_t sample;
                if (sample_def) {
                    sample = osc_sample_play(sample_def, phase);
                } else {
                    sample = osc_sample(waveform, phase, eff_duty, s.noise_lfsr[v], eff_phase_inc);
                }

                // Amplitude chain: oscillator × velocity × envelope
                int32_t level = (int32_t)(env_ramp * 32767.0f);
                int32_t scaled = (sample * amplitude) >> 15;
                scaled = (scaled * level) >> 15;

                // LFO → amplitude (tremolo)
                if (lfo_depth_q15 > 0) {
                    int32_t mod = 32767 - lfo_depth_q15 + ((lfo_val * lfo_depth_q15) >> 15);
                    scaled = (scaled * mod) >> 15;
                }

                // State-variable filter (fixed-point) — F_half ramped, Q
                // constant, the two-pass integration itself per-sample
                if (has_filter) {
                    int16_t filt_f = (int16_t)fhalf_ramp;
                    scaled = s.filter[v].tick(scaled, filt_f, filt_q, filter_mode);
                }

                const int32_t out_l = (int32_t)(((int64_t)scaled * gain_l) >> 15);
                const int32_t out_r = (int32_t)(((int64_t)scaled * gain_r) >> 15);
                s.dry_l[blk + i] += out_l;
                s.dry_r[blk + i] += out_r;
                if (cut_q15) {
                    s.cut_l[blk + i] += (int32_t)(((int64_t)out_l * cut_q15) >> 15);
                    s.cut_r[blk + i] += (int32_t)(((int64_t)out_r * cut_q15) >> 15);
                }

                phase += eff_phase_inc;

                // Handle sample loop/end
                if (sample_def) {
                    if (!osc_sample_advance_phase(sample_def, phase)) {
                        sample_ended = true;  // non-looped sample ended
                        break;
                    }
                }

                env_ramp += env_delta;
                lfo_ramp += lfo_delta;
                fhalf_ramp += fhalf_delta;
            }

            lfo_cur = lfo_target;
            fhalf_cur = fhalf_target;
        }
        s.voice_phase[v] = phase;
        s.lfo_phase[v] = lfo_ph;
        s.mod_lfo_phase[v] = mod_ph;
        s.lfo_val_state[v] = lfo_cur;
        s.f_half_state[v] = fhalf_cur;
    }

    // Post-mix effect (delay, reverb, phaser, flanger, or chorus, the
    // Multi's FX TYPE). Mono send / stereo return: downmix the sent share
    // of the stereo dry mix to mono, run the (still-mono) effect on it,
    // then add its wet output identically to both channels — only the dry
    // path carries pan. Clear the newly selected effect's buffer on a type
    // switch so a stale tail can't resurface.
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
    // its own gain, so FX MIX 127 is wet-only; dry plus near-unity wet would
    // clip at full mix. The unsent share stays dry at unity.
    int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)fx.mix) * 258 : 32768;
    const bool add_cut = has_fx && any_cut;
    const int32_t master = s.multi.master_q15;

    // Clip, add the wet return to both channels, and interleave into stereo int16_t.
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
