#include "audio_engine.h"
#include "core1_park.h"
#include "hardware/gpio.h"
#include "opl_render.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <atomic>

static volatile uint8_t s_load_pct = 0;

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// OPL engine: MAX_VOICES independent voices, each 2-op or 4-op depending on
// its Patch's Algorithm, each started from its Part's cooked patch
// (opl_render.h) and driven from VoiceParams (phase_inc = bend-scaled note
// frequency, amplitude = velocity, gate = held/released), mixed by the
// Multi's Parts and FX.

// Render state -- only touched by Core 1.
static OplRenderState render_state;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    opl_init_waveforms();
    osc_init_sine();       // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // eg_to_gain()'s exp2 LUT -- reused from ../fm/env_dx.h, must run before any EG step
    opl_render_init(render_state);

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        // Parked while Core 0 wrote flash: playback ran on silenced buffers
        // meanwhile, so drop the fill requests queued up and resume with the
        // next one, in step with playback again.
        if (core1_park_point()) {
            multicore_fifo_drain();
            continue;
        }

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Latch the voice block before taking new cooked patches: Core 0
        // publishes a Part's patch before committing the notes that use it,
        // so every note seen here finds its cook as the Part's current or
        // previous one, or waits for it (opl_render.h).
        const VoiceParamBlock &vp = params->active();
        std::atomic_thread_fence(std::memory_order_acquire);
        for (uint32_t p = 0; p < OPL_PART_COUNT; p++) {
            opl_render_take_patch(render_state, p, opl_part_slots[p]);
        }
        opl_render_take_multi(render_state, opl_multi_slot);

        opl_render_buffer(render_state, vp, i2s_buffer_ptr(buffers, buf_index));

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Active-voice bitmap to Core 0 (non-blocking).
        multicore_fifo_push_timeout_us(render_state.active_mask, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
