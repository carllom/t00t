#include "audio_engine.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "opl_voice.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

static volatile uint8_t s_load_pct = 0;

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// OPL engine: MAX_VOICES independent 2-operator voices, each
// driven straight from VoiceParams (phase_inc = bend-scaled note frequency,
// patch = the whole timbre, amplitude = velocity, gate = held/released) with
// the algorithm's routing resolved once per note-on from one of patch.h's
// two fixed literals (no runtime DAG resolution -- OPL2 has only two
// algorithms), envelopes stepped once per control block (opl_voice.h,
// env_opl.h), and rendered through the reused ../fm/op.h kernels. gate=false
// releases through each operator's EG rather than cutting the voice
// immediately, and a voice keeps rendering until its carriers' envelopes
// actually finish (opl_voice_active()), never on gate alone.

static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];

// `fx_buf` is the mono send/return scratch for the post-mix effect.
static int32_t fx_buf[SAMPLES_PER_BUFFER];

static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

// Per-voice render state (Core 1 only, never crosses ParamExchange).
static FmOp      voice_ops[MAX_VOICES][FM_NUM_OPS];
static EnvOpl     voice_env[MAX_VOICES][2];
static FmRouting  voice_routing[MAX_VOICES];
static OplVibrato voice_vib[MAX_VOICES];
static uint8_t    voice_last_trigger[MAX_VOICES];
static bool       voice_gated[MAX_VOICES];  // Core 1's own gate-edge tracking, for the release transition

// Shared bus scratch -- reused across every voice, sequentially, within a
// pass. Six-wide to match FmRouting's fixed FM_NUM_OPS width, even though
// only buses 0/1 (and OUT) ever carry real signal for a 2-operator voice.
static int32_t bus_mod0[FM_BLOCK], bus_mod1[FM_BLOCK], bus_mod2[FM_BLOCK];
static int32_t bus_mod3[FM_BLOCK], bus_mod4[FM_BLOCK], bus_mod5[FM_BLOCK];
static int32_t bus_out[FM_BLOCK];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    opl_init_waveforms();
    osc_init_sine();       // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // eg_to_gain()'s exp2 LUT -- reused from ../fm/env_dx.h, must run before any EG step
    fx_delay.init();
    fx_reverb.init();
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_last_trigger[v] = 0;  // matches VoiceParams' default trigger=0 -- a never-triggered voice must NOT look "changed"
        voice_gated[v] = false;
        opl_voice_init_inert(voice_ops[v]);
        voice_env[v][0].ix = 4;  // idle -- a zero-initialized EnvOpl is NOT idle (stage 0 is a real stage)
        voice_env[v][1].ix = 4;
        voice_env[v][0].down = false;
        voice_env[v][1].down = false;
        voice_vib[v].phase = 0;
    }

    FmVoiceBuses bus{ { bus_mod0, bus_mod1, bus_mod2, bus_mod3, bus_mod4, bus_mod5 }, bus_out };

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
            if (!p.patch) continue;

            if (p.trigger != voice_last_trigger[v]) {
                opl_voice_note_on(voice_ops[v], voice_env[v], voice_routing[v], *p.patch,
                                   p.phase_inc, p.amplitude, p.note, voice_vib[v]);
                if (!p.gate) {
                    // A note this short had its on and off both land in the
                    // same committed parameter block, so there is no later
                    // gate-off edge left to catch below -- release right
                    // away rather than leaving the voice stuck sustaining.
                    opl_voice_note_off(voice_env[v]);
                }
                voice_last_trigger[v] = p.trigger;
                voice_gated[v] = p.gate;
            } else if (!p.gate && voice_gated[v]) {
                // Gate-off edge: release through the EG instead of a hard cutoff.
                opl_voice_note_off(voice_env[v]);
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }

            // Keep rendering through release even after gate goes false --
            // stop only once every carrier's EG has actually gone idle
            // (opl_voice_active()), never on gate alone.
            if (!p.gate && !opl_voice_active(voice_env[v], voice_routing[v])) continue;

            opl_render_voice(voice_ops[v], voice_env[v], voice_routing[v], bus, p.pan,
                              dry_l, dry_r, SAMPLES_PER_BUFFER, voice_vib[v], p.mod_wheel);
            active_mask |= (1u << v);
        }

        // Post-mix effect (delay / reverb, selected by CC74) -- identical
        // shape to every other engine's chain. Mono send / stereo return.
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

        multicore_fifo_push_timeout_us(active_mask, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}
