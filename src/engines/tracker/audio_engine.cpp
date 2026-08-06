#include "audio_engine.h"
#include "osc/oscillator.h"
#include "pan.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Build/boot smoke test (#13): no XM/pattern logic yet. Proves the build
// seam, MAX_VOICES=32, and that voice_alloc + fx/delay + fx/reverb can be
// left out of the link (see CMakeLists.txt) before any real mixer lands.
// Voice 0 is a hardcoded, always-on test tone rather than MIDI- or
// pattern-driven; voices 1..31 are silent placeholders.
static constexpr float TEST_TONE_HZ = 440.0f;
static constexpr int16_t TEST_TONE_AMPLITUDE = 16384;  // fixed headroom, no envelope yet

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

static uint32_t voice0_phase;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    voice0_phase = 0;
    osc_init_sine();

    // Centered pan — audible on both channels through the shared stereo tail.
    int32_t gain_l, gain_r;
    pan_gains_q15(0, gain_l, gain_r);
    const uint32_t inc = osc_phase_inc(TEST_TONE_HZ);

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Snapshot committed params — unused until pattern playback lands,
        // but read every pass so the double-buffer handoff is exercised.
        const VoiceParamBlock &vp = params->active();
        (void)vp;

        uint32_t phase = voice0_phase;
        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t sample = (osc_sine(phase) * TEST_TONE_AMPLITUDE) >> 15;
            int32_t l = ((int64_t)sample * gain_l) >> 15;
            int32_t r = ((int64_t)sample * gain_r) >> 15;
            *out++ = (int16_t)__ssat(l, 16);
            *out++ = (int16_t)__ssat(r, 16);
            phase += inc;
        }
        voice0_phase = phase;

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Active-voice bitmap to Core 0 (non-blocking): voice 0 always sounding.
        multicore_fifo_push_timeout_us(1u, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
