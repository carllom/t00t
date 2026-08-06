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
// constants -- the "hardcoded test configuration" #15/#16 ask for; pattern
// data, the host converter, and the TickBlock ring don't exist yet
// (tracker.md build order steps 3-4).
#include "../../../samples/sararr1.h"

// --- Telemetry for the Core 0 UI (published by Core 1) ---
static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// #16: the 32-voice profiling measurement gate. Self-cycling rig -- no
// display, no serial stdio on this build (USB is MIDI-only), so the only
// readout is the profiling pin (GPIO 22) on a scope/logic analyzer. Each
// phase holds for PHASE_HOLD_FRAMES (a fixed wall-clock duration regardless
// of DMA_BUFFER_SIZE) so a listener can read every number off one continuous
// trace, or reflash with DMA_BUFFER_SIZE=512 and re-read the same sequence
// for the buffer-size comparison. See engine.md for the recorded results.
enum Phase : uint8_t {
    PHASE_IDLE,        // 0 voices -- isolates fixed per-buffer/IRQ overhead
    PHASE_8,
    PHASE_16,
    PHASE_24,
    PHASE_32_LINEAR,
    PHASE_32_NEAREST,  // same 32 voices, mix_voice_nearest() instead of mix_voice()
    PHASE_COUNT
};
static constexpr uint32_t PHASE_HOLD_FRAMES = SAMPLE_RATE * 4;  // 4s per phase, ~24s per cycle

static void phase_config(uint8_t phase, uint32_t &num_voices, bool &nearest) {
    switch (phase) {
        case PHASE_IDLE:       num_voices = 0;  nearest = false; break;
        case PHASE_8:          num_voices = 8;  nearest = false; break;
        case PHASE_16:         num_voices = 16; nearest = false; break;
        case PHASE_24:         num_voices = 24; nearest = false; break;
        case PHASE_32_LINEAR:  num_voices = 32; nearest = false; break;
        case PHASE_32_NEAREST: num_voices = 32; nearest = true;  break;
        default:               num_voices = 0;  nearest = false; break;
    }
}

// Sample data must live in SRAM, never XIP (tracker.md "Memory Strategy"):
// 32 voices reading scattered, non-integer-stride addresses would thrash the
// 8 KB XIP cache and evict Core 0's code with it. `sararr1_data` above is
// `const`, i.e. flash-resident -- this buffer is the SRAM copy the mixer
// actually reads from. +1 for the interpolator's guard sample.
static int8_t s_chorus_ram[sararr1_NUM_SAMPLES + 1];
static const TrackerSample s_sample_chorus = {
    s_chorus_ram, sararr1_NUM_SAMPLES, sararr1_LOOP_START, sararr1_LOOP_END, true,
};

// A deliberately tight 4-sample loop (#16 acceptance: "one voice per short
// loop, to catch samples_to_loop_end() overhead on tight loops"). At the
// increments in use here (~0.16-0.65x) this wraps roughly every 6-25 output
// samples -- samples_to_loop_end()/wrap_loop() run many times per sub-block
// instead of once, the worst case the hoisted-bound design has to pay for.
static int8_t s_tight_ram[5];  // 4 samples + guard (loop-start value)
static const TrackerSample s_sample_tight = { s_tight_ram, 4, 0, 4, true };

static TrackerVoice s_voices[MAX_VOICES];
static TrackerTickState s_tick;

static void load_test_samples() {
    for (uint32_t i = 0; i < sararr1_NUM_SAMPLES; i++) s_chorus_ram[i] = sararr1_data[i];
    s_chorus_ram[sararr1_NUM_SAMPLES] = sararr1_data[sararr1_LOOP_START];  // looped: guard = loop-start value

    static const int8_t tight[4] = { 100, 60, -60, -100 };
    for (uint32_t i = 0; i < 4; i++) s_tight_ram[i] = tight[i];
    s_tight_ram[4] = tight[0];  // looped: guard = loop-start value
}

// All 32 slots are configured once at boot and never go inactive (every
// sample here loops forever) -- which slots actually get mixed each buffer
// is purely a function of `num_voices` passed to tracker_render_buffer(),
// so voice count stays exactly stable for the whole hold time of each phase
// instead of decaying as one-shots finish (#15's rig did that; not wanted
// for a duty-cycle measurement). Slot 0 is always the tight loop; slots
// 1..31 are a chorus spread +-1 octave around the sample's native pitch,
// panned across the stereo field.
static void setup_test_voices() {
    const float base_ratio = (float)sararr1_SAMPLE_RATE / (float)SAMPLE_RATE;
    // Pre-pan per-voice amplitude. 32 of these can never sum past full scale
    // even in the (acoustically implausible) worst case of every voice
    // peaking in phase at once: 32 * 1024 == 32768.
    constexpr int32_t VOICE_AMP_Q15 = 1024;

    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        TrackerVoice &v = s_voices[i];

        float ratio;
        if (i == 0) {
            ratio = base_ratio;  // tight-loop voice: same pitch family, no detune needed
        } else {
            // Worst realistic increment per tracker.md is ~16-32x; a +-1
            // octave spread across the 31 chorus slots stays well inside
            // Q8.24's 8 integer bits while giving an audibly detuned unison.
            uint32_t g = i - 1;
            float octaves = ((float)g - 15.0f) / 31.0f * 2.0f;
            ratio = base_ratio * exp2f(octaves);
        }
        uint32_t inc_q8_24 = (uint32_t)(ratio * (float)(1u << TRACKER_INC_FRAC_BITS));

        // Spread hard-left to hard-right across all 32 voices.
        int16_t pan = (int16_t)(((int32_t)i * 65535 / (MAX_VOICES - 1)) - 32768);
        int32_t gain_l, gain_r;
        pan_gains_q15(pan, gain_l, gain_r);

        v.active = true;
        v.sample = (i == 0) ? &s_sample_tight : &s_sample_chorus;
        v.pos = 0;
        // Q8.24 -> Q18.14, pre-shifted once here at latch time rather than
        // per-sample or per-sub-block.
        v.inc = tracker_latch_inc(inc_q8_24);
        // Ramp in from silence over the first sub-block instead of stepping
        // straight to target -- avoids a click whenever a phase transition
        // brings a previously-excluded voice back into the mix.
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
    (void)params;  // no player, no IPC yet (#15/#16) -- see mixer.h

    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    // pan_gains_q15() (used below by setup_test_voices()) reads osc_sine()'s
    // wavetable; without this it's silently all-zero, so every voice's
    // target volume computes to zero -- full mixing cost, dead silent output.
    osc_init_sine();

    load_test_samples();
    setup_test_voices();

    uint8_t phase = PHASE_IDLE;
    uint32_t phase_frames = 0;

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        uint32_t num_voices;
        bool nearest;
        phase_config(phase, num_voices, nearest);

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        tracker_render_buffer(s_voices, num_voices, out, SAMPLES_PER_BUFFER, s_tick, nearest);

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        phase_frames += SAMPLES_PER_BUFFER;
        if (phase_frames >= PHASE_HOLD_FRAMES) {
            phase_frames = 0;
            phase = (uint8_t)((phase + 1) % PHASE_COUNT);
        }

        // Active-voice bitmap to Core 0 (non-blocking).
        uint32_t bitmap = 0;
        for (uint32_t v = 0; v < num_voices; v++) {
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
