#include "audio_engine.h"
#include "core1_park.h"
#include "rig.h"
#include "chip_render.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "fx/phaser.h"
#include "fx/flanger.h"
#include "fx/chorus.h"
#include "fx/bitcrusher.h"
#include "fx/overdrive.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>
#include <atomic>
#include <cmath>

// Chip module Core 1 render (module_chip.md §1).
//
// Two builds live in this file, same idiom as the speech engine's
// SPEECH_PROFILE flag: the normal MIDI-driven engine (below, default) and
// a measurement rig (rig.h) behind T00T_CHIP_PROFILE=1 --
// `make ENGINE=chip CHIP_PROFILE=1`.

static volatile uint8_t s_load_pct = 0;
uint8_t audio_engine_load() { return s_load_pct; }

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

#if defined(T00T_CHIP_PROFILE) && T00T_CHIP_PROFILE

// Display telemetry (engine.h): display.cpp calls this regardless of
// CHIP_PROFILE, so the getter must link either way. The rig has no
// MIDI/display, so it reports silence.
void chip_voice_ui_state(uint32_t, ChipVoiceUiState *out) { *out = ChipVoiceUiState{VT_SILENT, false, 0, 0}; }

// --- Measurement rig (module_chip.md §1) --------------------------------
//
// A self-cycling, pin-only rig: no MIDI, no ParamExchange, no display.
// PROFILE_PIN (GPIO 22) is high for exactly the render, so the duty cycle
// read on a scope or logic analyser is the number, and the phase table
// steps through configurations on a fixed hold so one capture covers
// several of them.
//
// The levers themselves are compile-time (rig.h), because a runtime switch
// would put a branch inside the loop being measured. What varies at
// runtime is only how many of the built voices are rendered -- the
// per-voice cost is the slope across that sweep, and comparing two
// builds' intercepts instead would fold their code layout differences
// into it.

static ChipRig rig;
static int32_t dry[CHIP_RIG_SUBBLOCK];
#if CHIP_RIG_FX != 0 || CHIP_RIG_SPEAKER
static int32_t dry_full[SAMPLES_PER_BUFFER];
#endif

#if CHIP_RIG_FX == 1
static FxDelay fx_delay;
#elif CHIP_RIG_FX == 2
static FxReverb fx_reverb;
#elif CHIP_RIG_FX == 3
static FxPhaser fx_phaser;
#elif CHIP_RIG_FX == 4
static FxFlanger fx_flanger;
#elif CHIP_RIG_FX == 5
static FxChorus fx_chorus;
#elif CHIP_RIG_FX == 6
static FxBitcrusher fx_bitcrusher;
#elif CHIP_RIG_FX == 7
static FxOverdrive fx_overdrive;
#endif
#if CHIP_RIG_FX != 0
static int32_t fx_buf[SAMPLES_PER_BUFFER];
// Fixed mid-range settings -- every fx/*.h effect does the same work
// regardless of p1/p2/mix, so these values don't affect what's being
// measured.
static constexpr EffectType CHIP_RIG_FX_TYPE[] = { FX_OFF, FX_DELAY, FX_REVERB, FX_PHASER, FX_FLANGER, FX_CHORUS, FX_BITCRUSHER, FX_OVERDRIVE };
static constexpr EffectParams CHIP_RIG_FX_PARAMS = {
    (uint8_t)CHIP_RIG_FX_TYPE[CHIP_RIG_FX], 100, 64, 64
};
#endif

#if CHIP_RIG_SPEAKER
static SidSpeakerStage speaker;
#endif

// Voice counts to step through. The slope across these is the per-voice
// cost; the intercept at 0 is the fixed per-buffer overhead.
static constexpr uint32_t PHASE_VOICES[] = { 0, 1, 4, 8, 16, CHIP_RIG_VOICES };
static constexpr uint32_t PHASE_COUNT = sizeof(PHASE_VOICES) / sizeof(PHASE_VOICES[0]);

// ~4 s per phase: long enough for a stable reading and short enough that
// one capture covers the whole sweep.
static constexpr uint32_t PHASE_HOLD_BUFFERS = (4000000u / BUF_PERIOD_US) + 1;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;   // the rig drives its voices directly, by design

    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    rig.init();
#if CHIP_RIG_FX == 1
    fx_delay.init();
#elif CHIP_RIG_FX == 2
    fx_reverb.init();
#elif CHIP_RIG_FX == 3
    fx_phaser.init();
#elif CHIP_RIG_FX == 4
    fx_flanger.init();
#elif CHIP_RIG_FX == 5
    fx_chorus.init();
#elif CHIP_RIG_FX == 6
    fx_bitcrusher.init();
#elif CHIP_RIG_FX == 7
    fx_overdrive.init();
#endif
#if CHIP_RIG_SPEAKER
    speaker.init((float)SAMPLE_RATE);
#endif

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

#if CHIP_RIG_FX == 0 && CHIP_RIG_SPEAKER == 0
        // Unchanged fast path -- fused render+output, no full-buffer staging.
        // Kept byte-for-byte identical to the pre-FX/speaker rig so every
        // number already measured with this build stays comparable; the
        // staged path below only exists for builds that need it downstream.
        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_RIG_SUBBLOCK) {
            uint32_t n = SAMPLES_PER_BUFFER - base;
            if (n > CHIP_RIG_SUBBLOCK) n = CHIP_RIG_SUBBLOCK;

            for (uint32_t i = 0; i < n; i++) dry[i] = 0;
            rig.render_n(dry, n, voices);

            for (uint32_t i = 0; i < n; i++) {
                // Mono, duplicated -- the speaker stage is mono (module_chip.md
                // §10); a stereo pan here would measure a stage the module
                // does not have.
                int16_t s = (int16_t)__ssat(dry[i] >> SID_MIX_SHIFT, 16);
                *out++ = s;
                *out++ = s;
            }
        }
#else
        for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_RIG_SUBBLOCK) {
            uint32_t n = SAMPLES_PER_BUFFER - base;
            if (n > CHIP_RIG_SUBBLOCK) n = CHIP_RIG_SUBBLOCK;

            for (uint32_t i = 0; i < n; i++) dry[i] = 0;
            rig.render_n(dry, n, voices);
            for (uint32_t i = 0; i < n; i++) dry_full[base + i] = dry[i];
        }

        // The insert sits upstream of the speaker sim, not after it
        // (module_chip.md §10). Mono send / stereo return per every fx/*.h
        // effect's own contract, but this rig's output is already mono, so
        // the wet add-back is direct.
#if CHIP_RIG_FX != 0
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) fx_buf[i] = dry_full[i];
#if CHIP_RIG_FX == 1
        fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#elif CHIP_RIG_FX == 2
        fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#elif CHIP_RIG_FX == 3
        fx_phaser.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#elif CHIP_RIG_FX == 4
        fx_flanger.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#elif CHIP_RIG_FX == 5
        fx_chorus.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#elif CHIP_RIG_FX == 6
        fx_bitcrusher.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#else
        fx_overdrive.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#endif
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) dry_full[i] += fx_buf[i];
#endif

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t v = dry_full[i];
#if CHIP_RIG_SPEAKER
            v = (int32_t)speaker.tick((float)(v >> SID_MIX_SHIFT));
#else
            v >>= SID_MIX_SHIFT;
#endif
            // Mono, duplicated -- the speaker stage is mono (module_chip.md
            // §10); a stereo pan here would measure a stage the module
            // does not have.
            int16_t s = (int16_t)__ssat(v, 16);
            *out++ = s;
            *out++ = s;
        }
#endif  // CHIP_RIG_FX == 0 && CHIP_RIG_SPEAKER == 0

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

#else  // !T00T_CHIP_PROFILE -- the real engine

// Render state -- only touched by Core 1 (and read by the display's
// telemetry getter).
static ChipRenderState render_state;

void chip_voice_ui_state(uint32_t voice, ChipVoiceUiState *out) {
    *out = (voice < MAX_VOICES) ? render_state.voice_ui[voice] : ChipVoiceUiState{VT_SILENT, false, 0, 0};
}

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    res2p_init();   // module_chip.md §1: speaker sim's peak stage (res2p.h's LUT, must run once before any res2p_set())
    chip_render_init(render_state);

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
        // previous one, or waits for it (chip_render.h).
        const VoiceParamBlock &vp = params->active();
        std::atomic_thread_fence(std::memory_order_acquire);
        for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) {
            chip_render_take_patch(render_state, p, chip_part_slots[p]);
        }
        chip_render_take_multi(render_state, chip_multi_slot);

        chip_render_buffer(render_state, vp, i2s_buffer_ptr(buffers, buf_index));

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // Consumed by voice_alloc.cpp's three-tier steal policy on Core 0
        // (§8).
        multicore_fifo_push_timeout_us(render_state.active_mask, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#endif  // T00T_CHIP_PROFILE
