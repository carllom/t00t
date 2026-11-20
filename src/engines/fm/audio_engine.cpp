#include "audio_engine.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

#if defined(T00T_FM_PROFILE) && T00T_FM_PROFILE

// Stereo dry mix, same shape as the other engines' dry_l/dry_r.
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];

#include "rig.h"

// #42 P0 rig: replaces the normal test-tone loop below with the stripped
// N-voice x 6-operator mixer (rig.h) -- no MIDI, no patch logic, fixed
// increments/gains, same "hands-off, self-cycling" shape the tracker's #16
// and speech's #31 rigs used. Every module_fm.md §3.6 lever is a build-time switch
// (rig.h's FM_RIG_* macros, set via the Makefile's FM_RIG_* variables), so
// this file doesn't itself choose between them -- the bench session
// (blocked on this issue, not part of it) does, by reflashing with a
// different combination each time.
static FmRigOp s_voices[FM_RIG_VOICES][6];
static int32_t s_bus0[FM_RIG_BLOCK], s_bus1[FM_RIG_BLOCK], s_bus2[FM_RIG_BLOCK];
static int32_t s_bus3[FM_RIG_BLOCK], s_bus4[FM_RIG_BLOCK], s_bus5[FM_RIG_BLOCK];
static int32_t s_bus_out[FM_RIG_BLOCK];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;  // fixed synthetic content only -- MIDI input is ignored in this build

    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    fm_rig_init_table();
    for (uint32_t v = 0; v < FM_RIG_VOICES; v++) {
        // One octave per 4 voices so FM_RIG_VOICES voices don't all beat
        // against each other at the exact same fundamental -- cost is
        // identical either way (fixed increments), this only matters for
        // the host WAV sounding like something other than a single dense
        // drone when the author listens to the sanity render.
        float base_hz = 55.0f * (float)(1u << (v / 4));
        fm_rig_init_voice(s_voices[v], base_hz, (float)SAMPLE_RATE);
    }
    FmRigBuses bus{ { s_bus0, s_bus1, s_bus2, s_bus3, s_bus4, s_bus5 }, s_bus_out };

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill -- outside the
        // profiling bracket below, so the pin reflects only the render
        // pass, not time spent waiting on the DMA IRQ.
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        fm_rig_render_buffer(s_voices, bus, FM_RIG_VOICES, dry_l, dry_r, SAMPLES_PER_BUFFER);

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            *out++ = (int16_t)__ssat(dry_l[i], 16);
            *out++ = (int16_t)__ssat(dry_r[i], 16);
        }

        multicore_fifo_push_timeout_us(0, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#else

#include "fm_render.h"
#include "core1_park.h"
#include <atomic>

// FM engine: MAX_VOICES independent 6-operator voices, each started from
// its Part's cooked patch (fm_render.h) and driven from VoiceParams
// (phase_inc = bend-scaled note frequency, amplitude = velocity, gate =
// held/released), mixed by the Multi's Parts and FX.

// Render state -- only touched by Core 1.
static FmRenderState render_state;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    fm_init_sine_tab();
    osc_init_sine();  // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // EG exp2 table -- must run before any EG step
    fm_render_init(render_state);

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
        // so every note seen here finds its cook as the Part's current or
        // previous one, or waits for it (fm_render.h).
        const VoiceParamBlock &vp = params->active();
        std::atomic_thread_fence(std::memory_order_acquire);
        for (uint32_t p = 0; p < FM_PART_COUNT; p++) {
            fm_render_take_patch(render_state, p, fm_part_slots[p]);
        }
        fm_render_take_multi(render_state, fm_multi_slot);

        fm_render_buffer(render_state, vp, i2s_buffer_ptr(buffers, buf_index));

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Active-voice bitmap to Core 0 (non-blocking).
        multicore_fifo_push_timeout_us(render_state.active_mask, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#endif  // T00T_FM_PROFILE
