#include "audio_engine.h"
#include "osc/oscillator.h"
#include "osc/sample.h"
#include "envelope.h"
#include "filter.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "pan.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <cmath>
#include <arm_acle.h>

// --- Telemetry for the Core 0 UI (published by Core 1) ---
static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

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

// Local voice state — only touched by Core 1
static uint32_t voice_phase[MAX_VOICES];
static float    lfo_phase[MAX_VOICES];
static float    mod_lfo_phase[MAX_VOICES];
static float    lfo_val_state[MAX_VOICES];   // last ramped LFO sample (Q15, as float), carried across sub-blocks
static float    f_half_state[MAX_VOICES];    // last ramped SVF F_half coefficient, carried across sub-blocks
static uint16_t noise_lfsr[MAX_VOICES];
static uint8_t  last_trigger[MAX_VOICES];
static bool     voice_gated[MAX_VOICES];
static Envelope envelope[MAX_VOICES];
static SVFilter filter[MAX_VOICES];

// Stereo dry mix (int32_t to avoid overflow during summation); each voice's
// contribution is split L/R by its pan coefficient. `fx_buf` is the mono
// send/return scratch for the post-mix effect (mono send / stereo return).
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Post-mix effect state (Core 1 only)
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;  // detect type switch to clear buffers

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // Configure envelope: 10ms attack, 100ms decay, 70% sustain, 800ms release
    EnvConfig env_cfg = env_config(10, 100, 70, 800);

    // Sub-block-rate envelope advance needs the exact n-sample-ahead decay/
    // release coefficient (coeff^SUBBLOCK). env_cfg is one fixed ADSR shared
    // by every voice, so this is computed once here, never at runtime.
    const float decay_coeff_sub   = powf(env_cfg.decay_coeff,   (float)SUBBLOCK);
    const float release_coeff_sub = powf(env_cfg.release_coeff, (float)SUBBLOCK);

    // Init local state
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        lfo_phase[v] = 0.0f;
        mod_lfo_phase[v] = 0.0f;
        lfo_val_state[v] = 0.0f;
        f_half_state[v] = 0.0f;
        noise_lfsr[v] = 0xACE1u;
        last_trigger[v] = 0;
        voice_gated[v] = false;
        envelope[v].init();
        filter[v].init();
    }

    // Generate wavetable
    osc_init_sine();

    // Clear effect buffers
    fx_delay.init();
    fx_reverb.init();

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Snapshot committed params
        const VoiceParamBlock &vp = params->active();

        // Clear the stereo dry mix
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            dry_l[i] = 0;
            dry_r[i] = 0;
        }

        // Render all voices
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];

            // Detect new note (trigger changed)
            if (p.trigger != last_trigger[v]) {
                last_trigger[v] = p.trigger;
                voice_phase[v] = 0;
                lfo_phase[v] = 0.0f;
                mod_lfo_phase[v] = 0.0f;
                lfo_val_state[v] = 0.0f;  // LFO restarts at phase 0 -> sine = 0
                // Filter coefficient seeded from the note's starting cutoff
                // (envelope and LFO are both 0 at trigger, so no mod applies
                // yet) so the first sub-block doesn't glide from a stale
                // coefficient left over from a previous note.
                f_half_state[v] = (p.filter_mode != FILTER_OFF)
                    ? (float)svf_compute_f_half((int32_t)p.filter_cutoff) : 0.0f;
                noise_lfsr[v] = 0xACE1u;
                envelope[v].trigger();
                filter[v].init();  // clean filter state on new note
                voice_gated[v] = true;
            }

            // Detect gate-off edge
            if (!p.gate && voice_gated[v]) {
                envelope[v].release(env_cfg);
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }

            // Skip silent voices
            if (!envelope[v].active()) continue;

            // Render in sub-blocks (see the SUBBLOCK comment above): LFO
            // value, envelope level, and the SVF F_half coefficient are each
            // computed once per sub-block and linearly ramped across it.
            // Filter Q stays a per-buffer constant below — resonance is
            // never modulated, so there is nothing to ramp. Pitch does not
            // ramp: eff_phase_inc is recomputed once per sub-block, from the
            // sub-block's target LFO/vibrato values (not the ramped ones),
            // and held fixed across it — a stepped frequency change, not a
            // click-prone amplitude one.
            uint32_t phase = voice_phase[v];
            float lfo_ph = lfo_phase[v];
            float mod_ph = mod_lfo_phase[v];
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

            float lfo_cur = lfo_val_state[v];
            float fhalf_cur = f_half_state[v];
            bool sample_ended = false;

            for (uint32_t blk = 0; blk < SAMPLES_PER_BUFFER && !sample_ended; blk += SUBBLOCK) {
                if (!envelope[v].active()) break;
                uint32_t n = SUBBLOCK;

                // --- Envelope: exact O(1) advance by n samples, ramped within the block ---
                float env_start = envelope[v].level;
                float env_target = envelope[v].advance_block(env_cfg, decay_coeff_sub, release_coeff_sub, n);
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
                        sample = osc_sample(p.waveform, phase, eff_duty, noise_lfsr[v], eff_phase_inc);
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
                        scaled = filter[v].tick(scaled, filt_f, filt_q, p.filter_mode);
                    }

                    dry_l[blk + i] += (int32_t)(((int64_t)scaled * gain_l) >> 15);
                    dry_r[blk + i] += (int32_t)(((int64_t)scaled * gain_r) >> 15);

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
            voice_phase[v] = phase;
            lfo_phase[v] = lfo_ph;
            mod_lfo_phase[v] = mod_ph;
            lfo_val_state[v] = lfo_cur;
            f_half_state[v] = fhalf_cur;
        }

        // Post-mix effect (delay or reverb, selected by CC74). Mono send /
        // stereo return: downmix the stereo dry mix to mono, run the
        // (still-mono) effect on it, then add its wet output identically to
        // both channels — only the dry path carries per-voice pan. Clear the
        // newly selected effect's buffer on a type switch so a stale tail
        // can't resurface.
        bool has_fx = (vp.fx.type == FX_DELAY || vp.fx.type == FX_REVERB);
        if (vp.fx.type != s_last_fx_type) {
            if (vp.fx.type == FX_DELAY)       fx_delay.init();
            else if (vp.fx.type == FX_REVERB) fx_reverb.init();
            s_last_fx_type = vp.fx.type;
        }
        if (has_fx) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                fx_buf[i] = (dry_l[i] + dry_r[i]) >> 1;
            }
            if (vp.fx.type == FX_DELAY) fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else                        fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        }

        // Clip, add the wet return to both channels, and interleave into stereo int16_t.
        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t l = dry_l[i];
            int32_t r = dry_r[i];
            if (has_fx) {
                l += fx_buf[i];
                r += fx_buf[i];
            }
            *out++ = (int16_t)__ssat(l, 16);
            *out++ = (int16_t)__ssat(r, 16);
        }

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Send active-voice bitmap to Core 0 (non-blocking). The allocator drains
        // this each pass; the UI reads it via voice_alloc_active_mask().
        uint32_t bitmap = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            if (envelope[v].active()) bitmap |= (1u << v);
        }
        multicore_fifo_push_timeout_us(bitmap, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
