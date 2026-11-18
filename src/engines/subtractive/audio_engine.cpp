#include "audio_engine.h"
#include "render.h"
#include "core1_park.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <atomic>

// --- Telemetry for the Core 0 UI (published by Core 1) ---
static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// Render state — only touched by Core 1
static SubRenderState render_state;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // Generate wavetable
    osc_init_sine();
    sub_render_init(render_state);

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
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
        // so every note seen here finds its patch as the Part's current or
        // previous one (patch/receiver.h).
        const VoiceParamBlock &vp = params->active();
        std::atomic_thread_fence(std::memory_order_acquire);
        for (uint32_t p = 0; p < SUB_PART_COUNT; p++) {
            sub_render_take_patch(render_state, p, sub_part_slots[p]);
        }

        sub_render_buffer(render_state, vp, i2s_buffer_ptr(buffers, buf_index));

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Send active-voice bitmap to Core 0 (non-blocking). The allocator drains
        // this each pass; the UI reads it via voice_alloc_active_mask().
        multicore_fifo_push_timeout_us(sub_render_active_mask(render_state), 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
