#include "audio_engine.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

static volatile uint8_t s_load_pct = 0;

// Audio buffer period in microseconds — the deadline for rendering one buffer.
static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

uint8_t audio_engine_load() { return s_load_pct; }

// Stereo dry mix, same shape as the other engines' dry_l/dry_r.
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];

#if defined(T00T_FM_PROFILE) && T00T_FM_PROFILE

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

#include "op.h"
#include "fx/delay.h"
#include "fx/reverb.h"

// FM engine (#44/#45, module_fm.md P1/P2): MAX_VOICES independent 6-operator
// voices, each driven straight from VoiceParams (phase_inc = bend-scaled
// note frequency, patch = the whole timbre, amplitude = velocity, gate =
// held/released) with routing resolved once per note-on (patch.h's
// fm_resolve_routing()), envelopes stepped once per control block (op.h's
// fm_voice_step_envelopes(), env_dx.h), and rendered through op.h's
// kernels. #45 supersedes #44's fixed-gain/hard-cutoff behavior: gate=false
// now releases through each operator's EG instead of cutting the voice
// immediately, and a voice keeps rendering (and keeps its active_mask bit)
// until its carriers' envelopes actually finish (fm_voice_active()) --
// mirroring the subtractive engine's trigger/gate/envelope-active idiom.

// `fx_buf` is the mono send/return scratch for the post-mix effect (mono
// send / stereo return).
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Post-mix effects (Core 1 only). Linked unconditionally -- module_fm.md §2: no
// sample-RAM pressure to protect, unlike the tracker's skeleton (#13).
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

// Per-voice render state (Core 1 only, never crosses ParamExchange).
static FmOp     voice_ops[MAX_VOICES][FM_NUM_OPS];
static FmRouting voice_routing[MAX_VOICES];
static uint8_t  voice_last_trigger[MAX_VOICES];
static bool     voice_routing_valid[MAX_VOICES];
static bool     voice_gated[MAX_VOICES];  // Core 1's own gate-edge tracking, for the release transition
static FmPitchEg voice_peg[MAX_VOICES];   // #49: one pitch EG per voice (not per operator -- pitch_eg.h)
static FmLfo    voice_lfo[MAX_VOICES];    // #49: one LFO per voice (not per operator -- lfo.h)

// Shared bus scratch (module_fm.md §4.3: "one shared scratch for the whole engine,
// not per-voice" -- reused across every voice, sequentially, within a pass).
static int32_t bus_mod0[FM_BLOCK], bus_mod1[FM_BLOCK], bus_mod2[FM_BLOCK];
static int32_t bus_mod3[FM_BLOCK], bus_mod4[FM_BLOCK], bus_mod5[FM_BLOCK];
static int32_t bus_out[FM_BLOCK];

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    fm_init_sine_tab();
    osc_init_sine();  // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // EG exp2 table -- must run before any EG step
    fx_delay.init();
    fx_reverb.init();
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_last_trigger[v] = 0;  // matches VoiceParams' default trigger=0 -- a never-triggered voice must NOT look "changed"
        voice_routing_valid[v] = false;
        voice_gated[v] = false;
        // A zero-initialized EnvDX is NOT idle (stage 0 is a real stage) --
        // every voice's EGs need an explicit env_dx_reset() so
        // fm_voice_active() doesn't report a never-triggered voice as active.
        for (uint32_t i = 0; i < FM_NUM_OPS; i++) env_dx_reset(voice_ops[v][i].eg);
        fm_pitch_eg_init(voice_peg[v]);  // #49
        fm_lfo_init(voice_lfo[v]);       // #49
    }

    FmVoiceBuses bus{ { bus_mod0, bus_mod1, bus_mod2, bus_mod3, bus_mod4, bus_mod5 }, bus_out };

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
            if (!p.patch) continue;

            // Routing (order/bus pointers/kernel selection/first-writer
            // flags) is resolved once per note-on, from patch data alone
            // (module_fm.md §5.6) -- a new `trigger` value is exactly that event.
            if (p.trigger != voice_last_trigger[v]) {
                voice_routing_valid[v] = fm_resolve_routing(*p.patch, voice_routing[v]);
                if (voice_routing_valid[v]) {
                    fm_voice_note_on(voice_ops[v], *p.patch, p.phase_inc, p.amplitude, p.note,
                                      &voice_peg[v], &voice_lfo[v]);
                }
                voice_last_trigger[v] = p.trigger;
                voice_gated[v] = p.gate;
            } else if (!p.gate && voice_gated[v]) {
                // Gate-off edge: release through the EG (#45) instead of
                // #44's hard cutoff -- mirrors the subtractive engine's
                // "Detect gate-off edge" (Trigger/Gate Signaling, engine.md).
                if (voice_routing_valid[v]) fm_voice_note_off(voice_ops[v], &voice_peg[v]);
                voice_gated[v] = false;
            } else {
                voice_gated[v] = p.gate;
            }
            if (!voice_routing_valid[v]) continue;  // patch failed DAG validation (module_fm.md §4.2) -- skip, don't mis-render

            // Keep rendering through release even after gate goes false --
            // stop only once every carrier's EG has actually gone idle
            // (fm_voice_active()), never on gate alone. This is what makes
            // "a voice reports itself free only when its carriers have
            // actually decayed" true instead of aspirational (module_fm.md #45,
            // avoiding the tracker's #21 "key-off never frees a voice" bug).
            if (!p.gate && !fm_voice_active(voice_ops[v], voice_routing[v])) continue;

            // #49: pitch bend, pitch EG, and LFO increment recompute all
            // happen inside fm_render_voice() now, once per control block
            // (finer-grained than #44's old once-per-buffer
            // fm_voice_update_pitch() call).
            fm_render_voice(voice_ops[v], *p.patch, voice_routing[v], bus, p.pan, dry_l, dry_r, SAMPLES_PER_BUFFER,
                             &voice_peg[v], &voice_lfo[v], p.phase_inc, p.mod_wheel);
            active_mask |= (1u << v);
        }

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

        // Active-voice bitmap to Core 0 (non-blocking).
        multicore_fifo_push_timeout_us(active_mask, 0);

        // Publish render load for the UI. EMA (alpha 1/8) of the per-buffer render
        // time as a fraction of the buffer deadline.
        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#endif  // T00T_FM_PROFILE
