#include "audio_engine.h"
#include "render.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Build/boot smoke test (#27): no segment sequencer, no tract filter yet.
// Proves the build seam, MAX_VOICES=4, the 22.05 kHz native / ZOH x2 resample
// seam (render.h), and that delay/reverb stay linked (speech.md: unlike the
// tracker, this engine has no sample-RAM pressure to protect). Voice 0 is a
// hardcoded, always-on test tone rather than MIDI- or segment-driven; voices
// 1..3 are silent placeholders reachable once the sequencer lands.
static constexpr float TEST_TONE_HZ = 220.0f;  // one octave below the other skeletons' 440 Hz

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

static uint32_t voice0_phase;

// ZOH x2 (render.h): every native sample fills exactly two output frames, so
// the output-rate buffer must divide evenly by two. Real hardware always
// satisfies this (SAMPLES_PER_BUFFER defaults to 256, override is 512 --
// audio_common.h), but the assumption is load-bearing enough to assert.
static constexpr uint32_t NATIVE_SAMPLES_PER_BUFFER = SAMPLES_PER_BUFFER / 2;
static_assert(SAMPLES_PER_BUFFER % 2 == 0,
              "speech engine's ZOH x2 resample requires an even output buffer size");

// Stereo dry mix at output rate (44.1 kHz, post-ZOH), same shape as the
// other engines' dry_l/dry_r -- render.h fills every slot via the ZOH, so no
// separate clear pass is needed here. `fx_buf` is the mono send/return
// scratch for the post-mix effect (mono send / stereo return).
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Post-mix effects (Core 1 only). Linked unconditionally, unlike the
// tracker's skeleton -- speech.md: "Delay/reverb stay linked ... speech has
// no sample-RAM pressure".
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    voice0_phase = 0;
    osc_init_sine();
    fx_delay.init();
    fx_reverb.init();

    // Phase increment at SPEECH_RATE, not the shared osc_phase_inc()'s
    // baked-in SAMPLE_RATE (44.1 kHz) -- this oscillator runs at half that.
    const uint32_t inc = (uint32_t)((TEST_TONE_HZ / (float)SPEECH_RATE)
        * (float)WAVETABLE_SIZE * (float)(1 << PHASE_FRAC_BITS));

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Snapshot committed params — unused until the sequencer lands, but
        // read every pass so the double-buffer handoff is exercised.
        const VoiceParamBlock &vp = params->active();

        speech_render_test_tone(voice0_phase, inc, /*pan=*/0, dry_l, dry_r, NATIVE_SAMPLES_PER_BUFFER);

        // Post-mix effect (delay / reverb, selected by CC74) — identical
        // shape to the subtractive/groovebox chain. Mono send / stereo
        // return: downmix the stereo dry mix to mono, run the (still-mono)
        // effect on it, then add its wet output identically to both
        // channels. Clear the newly selected effect's buffer on a type
        // switch so a stale tail can't leak.
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
