#include "audio_engine.h"
#include "osc/oscillator.h"
#include "osc/sample.h"
#include "envelope.h"
#include "filter.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

// Local voice state — only touched by Core 1
static uint32_t voice_phase[MAX_VOICES];
static uint32_t lfo_phase[MAX_VOICES];
static uint16_t noise_lfsr[MAX_VOICES];
static uint8_t  last_trigger[MAX_VOICES];
static bool     voice_gated[MAX_VOICES];
static Envelope envelope[MAX_VOICES];
static SVFilter filter[MAX_VOICES];

// Scratch buffer for mixing (int32_t to avoid overflow during summation)
static int32_t scratch[SAMPLES_PER_BUFFER];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // Configure envelope: 10ms attack, 100ms decay, 70% sustain, 800ms release
    EnvConfig env_cfg = env_config(10, 100, 70, 800);

    // Init local state
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        lfo_phase[v] = 0;
        noise_lfsr[v] = 0xACE1u;
        last_trigger[v] = 0;
        voice_gated[v] = false;
        envelope[v].init();
        filter[v].init();
    }

    // Generate wavetable
    osc_init_sine();

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);

        // Snapshot committed params
        const VoiceParamBlock &vp = params->active();

        // Clear scratch
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            scratch[i] = 0;
        }

        // Render all voices
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];

            // Detect new note (trigger changed)
            if (p.trigger != last_trigger[v]) {
                last_trigger[v] = p.trigger;
                voice_phase[v] = 0;
                lfo_phase[v] = 0;
                noise_lfsr[v] = 0xACE1u;
                envelope[v].trigger();
                filter[v].init();  // clean filter state on new note
                voice_gated[v] = true;
            }

            // Detect gate-off edge
            if (!p.gate && voice_gated[v]) {
                envelope[v].release();
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }

            // Skip silent voices
            if (!envelope[v].active()) continue;

            // Render with per-sample envelope
            uint32_t phase = voice_phase[v];
            uint32_t lfo_ph = lfo_phase[v];
            bool has_lfo = (p.lfo_rate != 0);
            bool has_filter = (p.filter_mode != FILTER_OFF);

            // Pre-compute filter Q (constant across buffer)
            int16_t filt_q = 0;
            if (has_filter) {
                filt_q = svf_compute_q(p.filter_resonance);
            }

            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                int32_t level = envelope[v].advance(env_cfg);
                if (level <= 0) break;

                // LFO: compute once, route to multiple destinations
                int32_t lfo_val = 0;
                if (has_lfo) {
                    lfo_val = osc_sine(lfo_ph);
                    lfo_ph += p.lfo_rate;
                }

                // LFO → pitch (vibrato): offset phase_inc by ±fraction
                uint32_t eff_phase_inc = p.phase_inc;
                if (p.lfo_pitch_depth > 0) {
                    int32_t pitch_mod = (lfo_val * p.lfo_pitch_depth) >> 15;
                    eff_phase_inc = (uint32_t)((int32_t)p.phase_inc +
                        (((int32_t)p.phase_inc * pitch_mod) >> 15));
                }

                // LFO → duty cycle (PWM): offset duty_cycle
                uint16_t eff_duty = p.duty_cycle;
                if (p.lfo_pwm_depth > 0) {
                    int32_t pwm_mod = (lfo_val * p.lfo_pwm_depth) >> 15;
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
                int32_t scaled = (sample * p.amplitude) >> 15;
                scaled = (scaled * level) >> 15;

                // LFO → amplitude (tremolo)
                if (p.lfo_depth > 0) {
                    int32_t mod = 32767 - p.lfo_depth + ((lfo_val * p.lfo_depth) >> 15);
                    scaled = (scaled * mod) >> 15;
                }

                // State-variable filter (fixed-point)
                if (has_filter) {
                    // Compute effective cutoff: base + envelope mod + LFO mod
                    int32_t cutoff = (int32_t)p.filter_cutoff;
                    if (p.filter_env_amount != 0) {
                        cutoff += ((int32_t)level * (int32_t)p.filter_env_amount) >> 15;
                    }
                    if (p.lfo_filter_depth != 0) {
                        cutoff += ((int32_t)lfo_val * (int32_t)p.lfo_filter_depth) >> 15;
                    }
                    if (cutoff < 20) cutoff = 20;
                    if (cutoff > 18000) cutoff = 18000;
                    int16_t filt_f = svf_compute_f_half(cutoff);
                    scaled = filter[v].tick(scaled, filt_f, filt_q, p.filter_mode);
                }

                scratch[i] += scaled;

                phase += eff_phase_inc;

                // Handle sample loop/end
                if (p.waveform == WAVE_SAMPLE && p.sample) {
                    if (!osc_sample_advance_phase(p.sample, phase)) {
                        break;  // non-looped sample ended
                    }
                }
            }
            voice_phase[v] = phase;
            lfo_phase[v] = lfo_ph;
        }

        // Clip and interleave into stereo int16_t buffer
        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t s = scratch[i];
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            int16_t val = (int16_t)s;
            *out++ = val;  // left
            *out++ = val;  // right (mono → both channels)
        }

        gpio_put(PROFILE_PIN, 0);

        // Send active-voice bitmap to Core 0 (non-blocking)
        uint32_t bitmap = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            if (envelope[v].active()) bitmap |= (1u << v);
        }
        multicore_fifo_push_timeout_us(bitmap, 0);
    }
}
