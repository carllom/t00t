#include "audio_engine.h"
#include "voice.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

// Local voice state — only touched by Core 1
static uint32_t voice_phase[MAX_VOICES];
static bool voice_active[MAX_VOICES];

// Scratch buffer for mixing (int32_t to avoid overflow during summation)
static int32_t scratch[SAMPLES_PER_BUFFER];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // Init local state
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        voice_active[v] = false;
    }

    // Generate wavetable
    voice_init_tables();

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

        // Render all active voices
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];

            if (!p.active) {
                voice_active[v] = false;
                continue;
            }

            // Reset phase on note-on edge
            if (!voice_active[v]) {
                voice_phase[v] = 0;
                voice_active[v] = true;
            }

            // Wavetable render with linear interpolation
            uint32_t phase = voice_phase[v];
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
                uint32_t frac = phase & ((1 << PHASE_FRAC_BITS) - 1);

                int16_t s0 = sine_table[idx];
                int16_t s1 = sine_table[(idx + 1) & WAVETABLE_MASK];
                int32_t sample = s0 + (((int32_t)(s1 - s0) * (int32_t)frac) >> PHASE_FRAC_BITS);

                scratch[i] += (sample * p.amplitude) >> 15;

                phase += p.phase_inc;
            }
            voice_phase[v] = phase;
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
