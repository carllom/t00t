#include "audio_engine.h"
#include "osc/oscillator.h"
#include "envelope.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

// Local voice state — only touched by Core 1
static uint32_t voice_phase[MAX_VOICES];
static uint32_t lfo_phase[MAX_VOICES];
static uint8_t  last_trigger[MAX_VOICES];
static bool     voice_gated[MAX_VOICES];
static Envelope envelope[MAX_VOICES];

// Scratch buffer for mixing (int32_t to avoid overflow during summation)
static int32_t scratch[SAMPLES_PER_BUFFER];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // Configure envelope: 10ms attack, 100ms decay, 70% sustain, 200ms release
    EnvConfig env_cfg = env_config(10, 100, 70, 200);

    // Init local state
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        lfo_phase[v] = 0;
        last_trigger[v] = 0;
        voice_gated[v] = false;
        envelope[v].init();
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
                envelope[v].trigger();
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

            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                int32_t level = envelope[v].advance(env_cfg);
                if (level <= 0) break;

                int32_t sample = osc_sample(p.waveform, phase);

                // Amplitude chain: oscillator × velocity × envelope
                int32_t scaled = (sample * p.amplitude) >> 15;
                scaled = (scaled * level) >> 15;

                // LFO → amplitude (tremolo)
                if (p.lfo_depth > 0) {
                    // LFO output: sine in [-32767..32767]
                    int32_t lfo_val = osc_sine(lfo_ph);
                    // Map to modulation: 32767 = full volume, -(depth) = reduced
                    // mod = 32767 - depth + (lfo_val * depth) >> 15
                    // When lfo_val=+32767: mod=32767 (unity)
                    // When lfo_val=-32767: mod=32767-2*depth (minimum)
                    int32_t mod = 32767 - p.lfo_depth + ((lfo_val * p.lfo_depth) >> 15);
                    scaled = (scaled * mod) >> 15;
                    lfo_ph += p.lfo_rate;
                }

                scratch[i] += scaled;

                phase += p.phase_inc;
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
    }
}
