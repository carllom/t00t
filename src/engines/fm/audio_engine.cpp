#include "audio_engine.h"
#include "render.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Build/boot smoke test (#41): no operator kernel, no patch struct, no
// envelope, no algorithm table yet -- fm.md's P0 measurement gate is the
// very next slice, and it needs this build target to measure on. Proves the
// build seam, MAX_VOICES=16 (provisional), the FM-specific 4096-entry sine
// table (sine_tab.h/render.h), and that delay/reverb stay linked (fm.md §2:
// FM's whole working set is ~12 KB, so the 128 KB delay line costs it
// nothing it needs). Voice 0 is a hardcoded, always-on test tone rather than
// MIDI-driven; voices 1..15 are silent placeholders.
static constexpr float TEST_TONE_HZ = 440.0f;  // matches the other full-rate skeletons (subtractive/groovebox/tracker)

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

static uint32_t voice0_phase;

// Stereo dry mix, same shape as the other engines' dry_l/dry_r. `fx_buf` is
// the mono send/return scratch for the post-mix effect (mono send / stereo
// return).
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Post-mix effects (Core 1 only). Linked unconditionally -- fm.md §2: no
// sample-RAM pressure to protect, unlike the tracker's skeleton (#13).
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    voice0_phase = 0;
    fm_init_sine_tab();
    osc_init_sine();  // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    fx_delay.init();
    fx_reverb.init();

    const uint32_t inc = fm_phase_inc(TEST_TONE_HZ);

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Snapshot committed params — unused until the operator kernel
        // lands, but read every pass so the double-buffer handoff is
        // exercised.
        const VoiceParamBlock &vp = params->active();

        fm_render_test_tone(voice0_phase, inc, /*pan=*/0, dry_l, dry_r, SAMPLES_PER_BUFFER);

        // Post-mix effect (delay / reverb, selected by CC74) — identical
        // shape to the other engines' chain. Mono send / stereo return:
        // downmix the stereo dry mix to mono, run the (still-mono) effect on
        // it, then add its wet output identically to both channels. Clear
        // the newly selected effect's buffer on a type switch so a stale
        // tail can't leak.
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

        // Active-voice bitmap to Core 0 (non-blocking): voice 0 always sounding.
        multicore_fifo_push_timeout_us(1u, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
