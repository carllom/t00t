#include "audio_engine.h"
#include "rig.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Chip module F0 measurement build (sid.md §1 P0, §14 item 1).
//
// A self-cycling, pin-only rig: no MIDI, no ParamExchange, no display, no
// effects. PROFILE_PIN (GPIO 22) is high for exactly the render, so the duty
// cycle read on a scope or logic analyser is the number, and the phase table
// steps through configurations on a fixed hold so one capture covers several
// of them. Same hands-off shape as the tracker's #16 rig and the speech
// engine's #31 profiling build.
//
// The levers themselves are compile-time (rig.h), because a runtime switch
// would put a branch inside the loop being measured. What varies at runtime is
// only how many of the built voices are rendered -- which is the one axis that
// must be swept within a single build, since the per-voice cost is the
// *slope*, and comparing two builds' intercepts would fold their code layout
// differences into it. That is the #43 lesson from the FM rig: inlining and
// flash placement move the fixed cost enough to swamp a per-voice figure.

static volatile uint8_t s_load_pct = 0;
uint8_t audio_engine_load() { return s_load_pct; }

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

static ChipRig rig;
static int32_t dry[CHIP_RIG_SUBBLOCK];

// Voice counts to step through. The slope across these is the per-voice cost;
// the intercept at 0 is the fixed per-buffer overhead, which sid.md §9's
// "idle ~0.6%" line is the comparison for.
static constexpr uint32_t PHASE_VOICES[] = { 0, 1, 4, 8, 16, CHIP_RIG_VOICES };
static constexpr uint32_t PHASE_COUNT = sizeof(PHASE_VOICES) / sizeof(PHASE_VOICES[0]);

// ~4 s per phase, matching #16 and #31: long enough for a stable reading and
// short enough that one capture covers the whole sweep.
static constexpr uint32_t PHASE_HOLD_BUFFERS = (4000000u / BUF_PERIOD_US) + 1;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;   // the rig drives its voices directly, by design

    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    rig.init();

    uint32_t phase = 0;
    uint32_t held = 0;

    for (;;) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        if (++held >= PHASE_HOLD_BUFFERS) {
            held = 0;
            phase = (phase + 1) % PHASE_COUNT;
        }
        const uint32_t voices = PHASE_VOICES[phase];

        uint32_t t_start = time_us_32();
        gpio_put(PROFILE_PIN, 1);

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_RIG_SUBBLOCK) {
            uint32_t n = SAMPLES_PER_BUFFER - base;
            if (n > CHIP_RIG_SUBBLOCK) n = CHIP_RIG_SUBBLOCK;

            for (uint32_t i = 0; i < n; i++) dry[i] = 0;
            rig.render_n(dry, n, voices);

            for (uint32_t i = 0; i < n; i++) {
                // Mono, duplicated -- sid.md §10 says the speaker stage is
                // mono and "more authentic and half the price", and a stereo
                // pan here would measure a stage the module does not have.
                int16_t s = (int16_t)__ssat(dry[i] >> SID_MIX_SHIFT, 16);
                *out++ = s;
                *out++ = s;
            }
        }

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Report the phase's voice count in the bitmap Core 0 already reads,
        // so a capture can be lined up with the phase without a second pin.
        multicore_fifo_push_timeout_us((1u << voices) - 1u, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
