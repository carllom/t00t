#pragma once

#include "engine.h"
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

// Mod-wheel vibrato: dedicated LFO, independent of the preset LFO.
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

    // Stereo dry mix (int32_t to avoid overflow during summation); each voice's
    // contribution is split L/R by its pan coefficient. `fx_buf` is the mono
    // send/return scratch for the post-mix effect (mono send / stereo return).
    int32_t dry_l[SAMPLES_PER_BUFFER];
    int32_t dry_r[SAMPLES_PER_BUFFER];
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

    for (uint32_t v = 0; v < MAX_VOICES; v++) {
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

// Bitmap of voices whose envelope is still sounding.
inline uint32_t sub_render_active_mask(const SubRenderState &s) {
    uint32_t bitmap = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (s.envelope[v].active()) bitmap |= (1u << v);
    }
    return bitmap;
}

// Render one buffer from `vp` into `out` (SAMPLES_PER_BUFFER interleaved
// stereo int16 frames).
inline void sub_render_buffer(SubRenderState &s, const VoiceParamBlock &vp, int16_t *out) {
    // Clear the stereo dry mix
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        s.dry_l[i] = 0;
        s.dry_r[i] = 0;
    }

    // Render all voices
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        const VoiceParams &p = vp.voices[v];

        // Detect new note (trigger changed)
        if (p.trigger != s.last_trigger[v]) {
            s.last_trigger[v] = p.trigger;
            s.voice_phase[v] = 0;
            s.lfo_phase[v] = 0.0f;
            s.mod_lfo_phase[v] = 0.0f;
            s.lfo_val_state[v] = 0.0f;  // LFO restarts at phase 0 -> sine = 0
            // Filter coefficient seeded from the note's starting cutoff
            // (envelope and LFO are both 0 at trigger, so no mod applies
            // yet) so the first sub-block doesn't glide from a stale
            // coefficient left over from a previous note.
            s.f_half_state[v] = (p.filter_mode != FILTER_OFF)
                ? (float)svf_compute_f_half((int32_t)p.filter_cutoff) : 0.0f;
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
        bool has_lfo = (p.lfo_rate != 0.0f);
        bool has_mod = (p.mod_depth > 0);
        bool has_filter = (p.filter_mode != FILTER_OFF);

        // Pre-compute LFO phase increment per sample
        float lfo_phase_inc = 0.0f;
        if (has_lfo) {
            lfo_phase_inc = p.lfo_rate / (float)SAMPLE_RATE;
        }

        // Dedicated mod-wheel vibrato LFO increment
        const float mod_phase_inc = MOD_VIBRATO_HZ / (float)SAMPLE_RATE;

        // Pre-convert float LFO depths to Q15 integers for inner loop
        int16_t lfo_depth_q15 = (int16_t)(p.lfo_depth * 32767.0f);
        int16_t lfo_pitch_q15 = (int16_t)(p.lfo_pitch_depth * 32767.0f);
        int16_t lfo_pwm_q15 = (int16_t)(p.lfo_pwm_depth * 512.0f);
        int16_t lfo_filt_hz = (int16_t)(p.lfo_filter_depth);  // Hz, signed

        // Pre-compute filter Q (constant across buffer)
        int32_t filt_q = 0;
        if (has_filter) {
            filt_q = svf_compute_q(p.filter_resonance);
        }

        // Pre-compute this voice's stereo pan gains (constant across buffer)
        int32_t gain_l, gain_r;
        pan_gains_q15(p.pan, gain_l, gain_r);

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
                int32_t cutoff = (int32_t)p.filter_cutoff;
                if (p.filter_env_amount != 0) {
                    cutoff += (level_target_q15 * (int32_t)p.filter_env_amount) >> 15;
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
                uint16_t eff_duty = p.duty_cycle;
                if (lfo_pwm_q15 > 0) {
                    int32_t pwm_mod = (lfo_val * lfo_pwm_q15) >> 15;
                    int32_t d = (int32_t)p.duty_cycle + pwm_mod;
                    if (d < 1) d = 1;
                    if (d > 1022) d = 1022;
                    eff_duty = (uint16_t)d;
                }

                int32_t sample;
                if (p.waveform == WAVE_SAMPLE && p.sample) {
                    sample = osc_sample_play(p.sample, phase);
                } else {
                    sample = osc_sample(p.waveform, phase, eff_duty, s.noise_lfsr[v], eff_phase_inc);
                }

                // Amplitude chain: oscillator × velocity × envelope
                int32_t level = (int32_t)(env_ramp * 32767.0f);
                int32_t scaled = (sample * p.amplitude) >> 15;
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
                    scaled = s.filter[v].tick(scaled, filt_f, filt_q, p.filter_mode);
                }

                s.dry_l[blk + i] += (int32_t)(((int64_t)scaled * gain_l) >> 15);
                s.dry_r[blk + i] += (int32_t)(((int64_t)scaled * gain_r) >> 15);

                phase += eff_phase_inc;

                // Handle sample loop/end
                if (p.waveform == WAVE_SAMPLE && p.sample) {
                    if (!osc_sample_advance_phase(p.sample, phase)) {
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

    // Post-mix effect (delay, reverb, phaser, flanger, or chorus,
    // selected by CC74). Mono send / stereo return: downmix the stereo
    // dry mix to mono, run the (still-mono) effect on it, then add its
    // wet output identically to both channels — only the dry path
    // carries per-voice pan. Clear the newly selected effect's buffer on
    // a type switch so a stale tail can't resurface.
    bool has_fx = (vp.fx.type == FX_DELAY   || vp.fx.type == FX_REVERB ||
                   vp.fx.type == FX_PHASER  || vp.fx.type == FX_FLANGER ||
                   vp.fx.type == FX_CHORUS  || vp.fx.type == FX_BITCRUSHER ||
                   vp.fx.type == FX_OVERDRIVE);
    if (vp.fx.type != s.last_fx_type) {
        if (vp.fx.type == FX_DELAY)        s.fx_delay.init();
        else if (vp.fx.type == FX_REVERB)  s.fx_reverb.init();
        else if (vp.fx.type == FX_PHASER)  s.fx_phaser.init();
        else if (vp.fx.type == FX_FLANGER) s.fx_flanger.init();
        else if (vp.fx.type == FX_CHORUS)  s.fx_chorus.init();
        else if (vp.fx.type == FX_BITCRUSHER) s.fx_bitcrusher.init();
        else if (vp.fx.type == FX_OVERDRIVE)  s.fx_overdrive.init();
        s.last_fx_type = vp.fx.type;
    }
    if (has_fx) {
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            s.fx_buf[i] = (s.dry_l[i] + s.dry_r[i]) >> 1;
        }
        if (vp.fx.type == FX_DELAY)        s.fx_delay.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_REVERB)  s.fx_reverb.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_PHASER)  s.fx_phaser.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_FLANGER) s.fx_flanger.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_CHORUS)  s.fx_chorus.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else if (vp.fx.type == FX_BITCRUSHER) s.fx_bitcrusher.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        else                                   s.fx_overdrive.process(s.fx_buf, SAMPLES_PER_BUFFER, vp.fx);
    }

    // Crossfade dry/wet by the mix knob (Q15) instead of always summing
    // full dry underneath -- fx_buf above already carries mix baked in
    // as its own gain, so CC73=127 now means wet-only, not dry+wet
    // (dry+near-unity-gain wet was clipping constantly at full mix,
    // worst on the phaser's zero-delay allpass).
    int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)vp.fx.mix) * 258 : 32768;

    // Clip, add the wet return to both channels, and interleave into stereo int16_t.
    for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
        int32_t l, r;
        if (has_fx) {
            l = (int32_t)(((int64_t)dry_scale * s.dry_l[i]) >> 15) + s.fx_buf[i];
            r = (int32_t)(((int64_t)dry_scale * s.dry_r[i]) >> 15) + s.fx_buf[i];
        } else {
            l = s.dry_l[i];
            r = s.dry_r[i];
        }
        *out++ = (int16_t)__ssat(l, 16);
        *out++ = (int16_t)__ssat(r, 16);
    }
}
