#include "audio_engine.h"
#include "render.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Phoneme keyboard (#28, speech.md P1 "SPEECH_HOLD"): MAX_VOICES=4
// independent formant-cascade voices, each driven straight from
// VoiceParams (phase_inc = glottal pitch, phoneme = sustained vowel, gate =
// held/released) with no segment sequencer -- one MIDI note is one
// sustained phoneme. render.h's speech_render_voice() is the shared
// device/host render core (tools/host_render/render_speech.cpp calls the
// same function to render each vowel to WAV).

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// ZOH x2 (render.h): every native sample fills exactly two output frames, so
// the output-rate buffer must divide evenly by two. Real hardware always
// satisfies this (SAMPLES_PER_BUFFER defaults to 256, override is 512 --
// audio_common.h), but the assumption is load-bearing enough to assert.
static constexpr uint32_t NATIVE_SAMPLES_PER_BUFFER = SAMPLES_PER_BUFFER / 2;
static_assert(SAMPLES_PER_BUFFER % 2 == 0,
              "speech engine's ZOH x2 resample requires an even output buffer size");

// Stereo dry mix at output rate (44.1 kHz, post-ZOH). Unlike the #27
// skeleton's single always-on test tone, speech_render_voice() accumulates
// (+=) so up to MAX_VOICES can be mixed -- callers must clear both buffers
// first. `fx_buf` is the mono send/return scratch for the post-mix effect
// (mono send / stereo return).
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Per-voice render state (Core 1 only, never crosses ParamExchange).
static SpeechVoice voices[MAX_VOICES];

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

    res2p_init();    // must run before any res2p_radius()/res2p_set() call
    osc_init_sine();  // pan_gains_q15() (speech_render_voice) reads this table --
                       // an easy drop when #28 replaced the #27 skeleton's own
                       // osc_init_sine() call with res2p_init(): every sample
                       // was getting multiplied by gain 0 from an all-zero
                       // wavetable, silent output despite correct DSP upstream.
    for (uint32_t v = 0; v < MAX_VOICES; v++) voices[v] = SpeechVoice{};
    fx_delay.init();
    fx_reverb.init();

    while (true) {
        // Wait for DMA ISR to tell us which buffer to fill
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        // Snapshot committed params
        const VoiceParamBlock &vp = params->active();

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            dry_l[i] = 0;
            dry_r[i] = 0;
        }

        uint32_t active_mask = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];
            speech_render_voice(voices[v], p.phase_inc, (float)SPEECH_RATE, p.trigger,
                                 p.amplitude, p.gate, p.phoneme, p.pan,
                                 dry_l, dry_r, NATIVE_SAMPLES_PER_BUFFER);
            // speech.md: "hold the bit set until the phoneme sequence
            // completes, regardless of gate" -- P1 has no utterance to
            // outlast the gate, so active == gate is exactly that rule
            // applied to a single sustained phoneme.
            if (p.gate) active_mask |= (1u << v);
        }

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

        // Active-voice bitmap to Core 0 (non-blocking).
        multicore_fifo_push_timeout_us(active_mask, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
