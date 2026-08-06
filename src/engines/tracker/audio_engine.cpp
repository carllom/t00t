#include "audio_engine.h"
#include "mixer.h"
#include "pan.h"
#include "osc/sine.h"
#include "pico/platform.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <cmath>

// Real Fairlight sample data (already resident in flash for the subtractive
// engine, see ../../samples.cpp) pulled in directly for its compile-time
// constants. This is the "hardcoded test configuration" #15 asks for --
// pattern data, the host converter, and the TickBlock ring don't exist yet
// (tracker.md build order steps 3-4), so the mixer is proven against a real,
// looped 8-bit instrument rather than a synthesized tone.
#include "../../../samples/sararr1.h"

// --- Telemetry for the Core 0 UI (published by Core 1) ---
static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// Sample data must live in SRAM, never XIP (tracker.md "Memory Strategy"):
// 32 voices reading scattered, non-integer-stride addresses would thrash the
// 8 KB XIP cache and evict Core 0's code with it. `sararr1_data` above is
// `const`, i.e. flash-resident -- this buffer is the SRAM copy the mixer
// actually reads from. +1 for the interpolator's guard sample.
static int8_t s_sample_ram[sararr1_NUM_SAMPLES + 1];

// Same underlying SRAM bytes, exposed two ways: a looped chorus voice never
// reads past loop_end (2176..15232, well short of the physical end here), so
// only the one-shot descriptor ever touches the guard sample at index
// sararr1_NUM_SAMPLES.
static const TrackerSample s_sample_looped = {
    s_sample_ram, sararr1_NUM_SAMPLES, sararr1_LOOP_START, sararr1_LOOP_END, true,
};
static const TrackerSample s_sample_oneshot = {
    s_sample_ram, sararr1_NUM_SAMPLES, 0, sararr1_NUM_SAMPLES, false,
};

static TrackerVoice s_voices[MAX_VOICES];
static TrackerTickState s_tick;

static void load_test_sample() {
    for (uint32_t i = 0; i < sararr1_NUM_SAMPLES; i++) s_sample_ram[i] = sararr1_data[i];
    // Guard sample: only the one-shot descriptor's voices ever reach it, so
    // it carries the "last value" form (tracker.md's rule for a non-looped
    // sample), not the loop-start form.
    s_sample_ram[sararr1_NUM_SAMPLES] = sararr1_data[sararr1_NUM_SAMPLES - 1];
}

// 32 voices at differing increments and volumes (#15 acceptance): the lower
// half loop, the upper half are one-shot, so both of samples_to_loop_end()'s
// hoisted-bound cases (loop wrap and end-of-sample) run on every buffer.
// Each half is a +-1-octave detuned chorus around the sample's native pitch,
// panned across the stereo field.
static void setup_test_voices() {
    const float base_ratio = (float)sararr1_SAMPLE_RATE / (float)SAMPLE_RATE;
    // Pre-pan per-voice amplitude. 32 of these can never sum past full scale
    // even in the (acoustically implausible) worst case of every voice
    // peaking in phase at once: 32 * 1024 == 32768.
    constexpr int32_t VOICE_AMP_Q15 = 1024;
    constexpr uint32_t HALF = MAX_VOICES / 2;

    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        TrackerVoice &v = s_voices[i];
        bool looped_group = (i < HALF);
        uint32_t group_i = looped_group ? i : (i - HALF);

        // Worst realistic increment per tracker.md is ~16-32x; a +-1 octave
        // spread here stays well inside Q8.24's 8 integer bits while giving
        // each 16-voice group an audibly detuned unison.
        float octaves = ((float)group_i - 7.5f) / 15.0f * 2.0f;
        float ratio = base_ratio * exp2f(octaves);
        uint32_t inc_q8_24 = (uint32_t)(ratio * (float)(1u << TRACKER_INC_FRAC_BITS));

        // Spread hard-left to hard-right across all 32 voices.
        int16_t pan = (int16_t)(((int32_t)i * 65535 / (MAX_VOICES - 1)) - 32768);
        int32_t gain_l, gain_r;
        pan_gains_q15(pan, gain_l, gain_r);

        v.active = true;
        v.sample = looped_group ? &s_sample_looped : &s_sample_oneshot;
        v.pos = 0;
        // Q8.24 -> Q18.14, pre-shifted once here at latch time rather than
        // per-sample or per-sub-block.
        v.inc = tracker_latch_inc(inc_q8_24);
        // Ramp in from silence over the first sub-block instead of stepping
        // straight to target -- avoids a startup click on the very first
        // buffer, exactly the case tracker.md's ramping rule exists for.
        v.cur_volL = 0;
        v.cur_volR = 0;
        v.tgt_volL = (VOICE_AMP_Q15 * gain_l) >> 15;
        v.tgt_volR = (VOICE_AMP_Q15 * gain_r) >> 15;
    }

    // Stub 20 ms tick: no player/TickBlock ring yet (tracker.md build order
    // steps 3-5), but sub-blocks still get cut short at tick boundaries so
    // that path is exercised now rather than left unproven.
    s_tick.samples_per_tick = SAMPLE_RATE / 50;
    s_tick.remaining = s_tick.samples_per_tick;
}

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;  // no player, no IPC yet (#15) -- see mixer.h

    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // pan_gains_q15() (used below by setup_test_voices()) reads osc_sine()'s
    // wavetable; without this it's silently all-zero, so every voice's
    // target volume computes to zero -- full 32-voice interpolation cost,
    // dead silent output. (No oscillator/tone code left in this engine to
    // have called it for us, unlike #13's test tone.)
    osc_init_sine();

    load_test_sample();
    setup_test_voices();

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        tracker_render_buffer(s_voices, MAX_VOICES, out, SAMPLES_PER_BUFFER, s_tick);

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Active-voice bitmap to Core 0 (non-blocking): one-shot voices
        // clear their bit once they finish, unlike #13's always-on test tone.
        uint32_t bitmap = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            if (s_voices[v].active) bitmap |= (1u << v);
        }
        multicore_fifo_push_timeout_us(bitmap, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
