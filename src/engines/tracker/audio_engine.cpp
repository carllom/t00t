#include "audio_engine.h"
#include "mixer.h"
#include "player_task.h"
#include "pico/platform.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"

// Core 1: the real-time mixer (#18, module_tracker.md "Core Split"). Consumes
// TickBlocks from g_tracker_tick_ring (produced on Core 0 by player_task.cpp)
// and renders them via mixer.h's tracker_render_buffer(), cut short at
// whatever tick boundary falls inside a DMA buffer. Never touches
// SongHeader/flash: tracker_apply_tick() (player.h) resolves note triggers
// purely from g_tracker_resident_samples, the SRAM table Core 0 built once
// at song load -- exactly the "keeps pattern-data flash reads off Core 1"
// split module_tracker.md calls for.

// --- Telemetry for the Core 0 UI (published by Core 1) ---
static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

static TrackerVoice s_voices[MAX_VOICES];
static TrackerSample s_voice_sample[MAX_VOICES];
static uint32_t s_tick_remaining = 0;
static uint32_t s_samples_per_tick = 0;

// Fills exactly `frames` stereo frames of `out`, crossing as many tick
// boundaries as needed. Ring-empty at a tick boundary renders silence
// (module_tracker.md "Startup and underrun": "a visible dropout on the profiling
// pin beats a subtle timing glitch") rather than replaying stale state --
// also doubles as the transport-stop mechanism (player_task.cpp stops
// producing; the ring drains over its last 0-2 ticks, then this takes over).
static void __not_in_flash_func(tracker_fill_buffer)(int16_t *out, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        if (s_tick_remaining == 0) {
            if (g_tracker_tick_ring.empty()) {
                for (uint32_t i = done; i < frames; i++) {
                    out[i * 2 + 0] = 0;
                    out[i * 2 + 1] = 0;
                }
                return;
            }
            TickBlock &tb = g_tracker_tick_ring.read_slot();
            tracker_apply_tick(tb, g_tracker_resident_samples, s_voices, s_voice_sample, g_tracker_num_channels);
            s_samples_per_tick = tb.samples_per_tick;
            s_tick_remaining = s_samples_per_tick;
            g_tracker_tick_ring.pop();
            // Non-blocking doorbell ack (module_tracker.md: "Core 1 never stalls") --
            // Core 0's tracker_player_task() drains this and refills the ring.
            multicore_fifo_push_timeout_us(1, 0);
        }

        uint32_t n = frames - done;
        if (s_tick_remaining < n) n = s_tick_remaining;

        TrackerTickState local{n, n};
        tracker_render_buffer(s_voices, g_tracker_num_channels, out + done * 2, n, local);

        done += n;
        s_tick_remaining -= n;
    }
}

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;  // module_tracker.md: latest-wins ParamExchange isn't used here -- see the ordered TickBlock ring instead

    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    for (uint32_t v = 0; v < MAX_VOICES; v++) s_voices[v] = TrackerVoice{};

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        tracker_fill_buffer(out, SAMPLES_PER_BUFFER);

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
