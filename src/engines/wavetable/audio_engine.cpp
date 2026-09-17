#include "audio_engine.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "fx/phaser.h"
#include "fx/flanger.h"
#include "fx/chorus.h"
#include "fx/bitcrusher.h"
#include "fx/overdrive.h"
#include "pan.h"
#include "osc/sine.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Wavetable engine Core 1 render (module_wavetable.md).
//
// Two builds live in this file, same idiom as the chip/speech engines'
// CHIP_PROFILE/SPEECH_PROFILE flags: the normal MIDI-driven engine skeleton
// (below, default) and the measurement rig (rig.h) behind
// T00T_WT_PROFILE=1 -- `make ENGINE=wavetable WT_PROFILE=1`.

static volatile uint8_t s_load_pct = 0;
uint8_t audio_engine_load() { return s_load_pct; }

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

#if defined(T00T_WT_PROFILE) && T00T_WT_PROFILE

#include "rig.h"

// --- Measurement rig (module_wavetable.md) ------------------------------
//
// A self-cycling, pin-only rig: no MIDI, no ParamExchange, no display.
// PROFILE_PIN (GPIO 22) is high for exactly the render, so the duty cycle
// read on a scope or logic analyser is the number, and the phase table
// steps through partial counts on a fixed hold so one capture covers
// several of them -- same shape as the chip module's own rig
// (src/engines/chip/audio_engine.cpp).

static WtRigPartial rig_partials[WT_RIG_PARTIALS];
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];

// Partial counts to step through. The slope across these is the per-partial
// cost; the intercept at 0 is the fixed per-buffer overhead.
static constexpr uint32_t PHASE_PARTIALS[] = { 0, 1, 4, 8, 16, 32, WT_RIG_PARTIALS };
static constexpr uint32_t PHASE_COUNT = sizeof(PHASE_PARTIALS) / sizeof(PHASE_PARTIALS[0]);

// ~4 s per phase: long enough for a stable reading and short enough that
// one capture covers the whole sweep.
static constexpr uint32_t PHASE_HOLD_BUFFERS = (4000000u / BUF_PERIOD_US) + 1;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;  // the rig drives its own partials directly, by design

    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    wt_rig_init_tables();
    for (uint32_t p = 0; p < WT_RIG_PARTIALS; p++) {
        float freq = 55.0f * (float)(1u << ((p / 8) % 4));
        uint8_t wave_pos = (uint8_t)((p * (WT_INDEX_SIZE - 1)) / (WT_RIG_PARTIALS - 1));
        float grain_hz = 20.0f + (float)(p % 8) * 5.0f;
        wt_rig_init_partial(rig_partials[p], freq, wave_pos, grain_hz);
    }

    uint32_t phase_idx = 0;
    uint32_t phase_buf_count = 0;

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);

        uint32_t n_partials = PHASE_PARTIALS[phase_idx];
        wt_rig_render_buffer(rig_partials, n_partials, dry_l, dry_r, SAMPLES_PER_BUFFER);

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            *out++ = (int16_t)__ssat(dry_l[i], 16);
            *out++ = (int16_t)__ssat(dry_r[i], 16);
        }

        gpio_put(PROFILE_PIN, 0);

        multicore_fifo_push_timeout_us(0, 0);

        phase_buf_count++;
        if (phase_buf_count >= PHASE_HOLD_BUFFERS) {
            phase_buf_count = 0;
            phase_idx = (phase_idx + 1) % PHASE_COUNT;
        }
    }
}

#else  // !T00T_WT_PROFILE -- the real engine skeleton

#include "osc/wavetable.h"
#include "envelope.h"

// One wavetable partial per voice, an ADSR envelope, no filter, no LFO, no
// partial pool -- see module_wavetable.md for what's deferred and why. The
// amplitude chain and FX chain otherwise match every other engine's shape
// (module_subtractive.md's Architecture section; src/engines/opl/
// audio_engine.cpp's FX block, reused unmodified below).

static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

static FxDelay      fx_delay;
static FxReverb     fx_reverb;
static FxPhaser     fx_phaser;
static FxFlanger    fx_flanger;
static FxChorus     fx_chorus;
static FxBitcrusher fx_bitcrusher;
static FxOverdrive  fx_overdrive;
static uint8_t      s_last_fx_type = 0xFF;

// Per-voice render state (Core 1 only, never crosses ParamExchange).
static uint32_t  voice_phase[MAX_VOICES];         // Q0.32, osc/wavetable.h's format
static Envelope  voice_env[MAX_VOICES];
static uint8_t   voice_last_trigger[MAX_VOICES];
static bool      voice_gated[MAX_VOICES];

// Fixed envelope shape for this skeleton -- no per-preset ADSR yet, same
// deferral as filter/LFO above.
static EnvConfig s_env_cfg;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    wavetable_bank_init();
    osc_init_sine();  // pan_gains_q15()'s quadrature source
    fx_delay.init();
    fx_reverb.init();
    fx_phaser.init();
    fx_flanger.init();
    fx_chorus.init();
    fx_bitcrusher.init();
    fx_overdrive.init();
    s_env_cfg = env_config(5, 200, 70, 400);

    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_phase[v] = 0;
        voice_env[v].init();
        voice_last_trigger[v] = 0;  // matches VoiceParams' default trigger=0
        voice_gated[v] = false;
    }

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        const VoiceParamBlock &vp = params->active();

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            dry_l[i] = 0;
            dry_r[i] = 0;
        }

        uint32_t active_mask = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];
            if (!p.table) continue;

            if (p.trigger != voice_last_trigger[v]) {
                voice_phase[v] = 0;
                voice_env[v].trigger();
                voice_last_trigger[v] = p.trigger;
                voice_gated[v] = p.gate;
                if (!p.gate) {
                    // A note this short had its on and off both land in the
                    // same committed parameter block -- release right away
                    // rather than leaving the voice stuck sustaining.
                    voice_env[v].release(s_env_cfg);
                }
            } else if (!p.gate && voice_gated[v]) {
                voice_env[v].release(s_env_cfg);
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }

            if (!voice_env[v].active()) continue;

            int32_t gain_l, gain_r;
            pan_gains_q15(p.pan, gain_l, gain_r);

            uint32_t phase = voice_phase[v];
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                float env_f = voice_env[v].advance(s_env_cfg);
                int32_t level = (int32_t)(env_f * 32767.0f);
                int32_t raw = wavetable_read_bilinear(*p.table, phase, p.wave_pos);
                int32_t scaled = (raw * p.amplitude) >> 15;
                scaled = (scaled * level) >> 15;
                dry_l[i] += (scaled * gain_l) >> 15;
                dry_r[i] += (scaled * gain_r) >> 15;
                phase += p.phase_inc;
            }
            voice_phase[v] = phase;
            active_mask |= (1u << v);
        }

        // Post-mix effect (selected by CC74) -- identical shape to every
        // other engine's chain. Mono send / stereo return.
        bool has_fx = (vp.fx.type == FX_DELAY   || vp.fx.type == FX_REVERB ||
                       vp.fx.type == FX_PHASER  || vp.fx.type == FX_FLANGER ||
                       vp.fx.type == FX_CHORUS  || vp.fx.type == FX_BITCRUSHER ||
                       vp.fx.type == FX_OVERDRIVE);
        if (vp.fx.type != s_last_fx_type) {
            if (vp.fx.type == FX_DELAY)        fx_delay.init();
            else if (vp.fx.type == FX_REVERB)  fx_reverb.init();
            else if (vp.fx.type == FX_PHASER)  fx_phaser.init();
            else if (vp.fx.type == FX_FLANGER) fx_flanger.init();
            else if (vp.fx.type == FX_CHORUS)  fx_chorus.init();
            else if (vp.fx.type == FX_BITCRUSHER) fx_bitcrusher.init();
            else if (vp.fx.type == FX_OVERDRIVE)  fx_overdrive.init();
            s_last_fx_type = vp.fx.type;
        }
        if (has_fx) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
                fx_buf[i] = (dry_l[i] + dry_r[i]) >> 1;
            }
            if (vp.fx.type == FX_DELAY)        fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else if (vp.fx.type == FX_REVERB)  fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else if (vp.fx.type == FX_PHASER)  fx_phaser.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else if (vp.fx.type == FX_FLANGER) fx_flanger.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else if (vp.fx.type == FX_CHORUS)  fx_chorus.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else if (vp.fx.type == FX_BITCRUSHER) fx_bitcrusher.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else                                   fx_overdrive.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        }

        int32_t dry_scale = has_fx ? (int32_t)(127 - (int32_t)vp.fx.mix) * 258 : 32768;

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t l, r;
            if (has_fx) {
                l = (int32_t)(((int64_t)dry_scale * dry_l[i]) >> 15) + fx_buf[i];
                r = (int32_t)(((int64_t)dry_scale * dry_r[i]) >> 15) + fx_buf[i];
            } else {
                l = dry_l[i];
                r = dry_r[i];
            }
            *out++ = (int16_t)__ssat(l, 16);
            *out++ = (int16_t)__ssat(r, 16);
        }

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        multicore_fifo_push_timeout_us(active_mask, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#endif  // T00T_WT_PROFILE
