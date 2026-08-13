#include "audio_engine.h"
#include "rig.h"
#include "speaker_sim.h"
#include "chip/sid_voice.h"
#include "chip/sid_filter.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Chip module Core 1 render (sid.md §1 P1, §14 item 2).
//
// Two builds live in this file, same idiom as the speech engine's
// SPEECH_PROFILE flag: the normal MIDI-driven engine (below, default) and
// the P0 measurement rig (rig.h, preserved unchanged behind
// T00T_CHIP_PROFILE=1 -- `make ENGINE=chip CHIP_PROFILE=1` -- so the
// hardware-verified cost numbers in sid.md §9/§14a.9 stay re-measurable
// against later changes without deleting the rig that produced them).

static volatile uint8_t s_load_pct = 0;
uint8_t audio_engine_load() { return s_load_pct; }

static constexpr uint32_t BUF_PERIOD_US = 1000000u * SAMPLES_PER_BUFFER / SAMPLE_RATE;

#if defined(T00T_CHIP_PROFILE) && T00T_CHIP_PROFILE

// --- P0 measurement rig (sid.md §1 P0, §14 item 1) -------------------------
//
// A self-cycling, pin-only rig: no MIDI, no ParamExchange, no display.
// PROFILE_PIN (GPIO 22) is high for exactly the render, so the duty cycle
// read on a scope or logic analyser is the number, and the phase table steps
// through configurations on a fixed hold so one capture covers several of
// them. Same hands-off shape as the tracker's #16 rig and the speech
// engine's #31 profiling build.
//
// The levers themselves are compile-time (rig.h), because a runtime switch
// would put a branch inside the loop being measured. What varies at runtime is
// only how many of the built voices are rendered -- which is the one axis that
// must be swept within a single build, since the per-voice cost is the
// *slope*, and comparing two builds' intercepts would fold their code layout
// differences into it. That is the #43 lesson from the FM rig: inlining and
// flash placement move the fixed cost enough to swamp a per-voice figure.

static ChipRig rig;
static int32_t dry[CHIP_RIG_SUBBLOCK];
#if CHIP_RIG_FX != 0 || CHIP_RIG_SPEAKER
static int32_t dry_full[SAMPLES_PER_BUFFER];
#endif

#if CHIP_RIG_FX == 1
static FxDelay fx_delay;
#elif CHIP_RIG_FX == 2
static FxReverb fx_reverb;
#endif
#if CHIP_RIG_FX != 0
static int32_t fx_buf[SAMPLES_PER_BUFFER];
// Fixed mid-range settings -- sid.md §9's FX cost lines are the fixed
// per-sample cost of the effect running, not a function of these knobs
// (fx/delay.h, fx/reverb.h: both do the same work regardless of p1/p2/mix).
static constexpr EffectParams CHIP_RIG_FX_PARAMS = {
    (uint8_t)(CHIP_RIG_FX == 1 ? FX_DELAY : FX_REVERB), 100, 64, 64
};
#endif

#if CHIP_RIG_SPEAKER
static SidSpeakerStage speaker;
#endif

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
#if CHIP_RIG_FX == 1
    fx_delay.init();
#elif CHIP_RIG_FX == 2
    fx_reverb.init();
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
                // Mono, duplicated -- sid.md §10 says the speaker stage is
                // mono and "more authentic and half the price", and a stereo
                // pan here would measure a stage the module does not have.
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

        // §10: "you want delay -> speaker, or reverb -> speaker" -- the
        // insert sits upstream of the speaker sim, not after it. Mono send /
        // stereo return per fx/delay.h and fx/reverb.h's own contract, but
        // this rig's output is already mono, so the wet add-back is direct.
#if CHIP_RIG_FX != 0
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) fx_buf[i] = dry_full[i];
#if CHIP_RIG_FX == 1
        fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
#else
        fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, CHIP_RIG_FX_PARAMS);
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
            // Mono, duplicated -- sid.md §10 says the speaker stage is
            // mono and "more authentic and half the price", and a stereo
            // pan here would measure a stage the module does not have.
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

// --- P1 engine skeleton (sid.md §1 P1, §14 item 2) --------------------------
//
// One SidVoice per MAX_VOICES slot, dispatched by VoiceType exactly like the
// groovebox (§7.3) -- VT_SILENT is skipped, VT_SID renders. No filter buses
// yet (P2: straight to the dry mix), no frame table VM yet (P3: a note is
// freq/pw/waveform/ADSR held static for its duration, no vibrato/arpeggio/
// mod_inc sweep). §4.4's per-voice sub-oscillator isn't wired to anything
// yet either -- sync_reset/ring_msb_flip are 0 until the frame VM can drive
// mod_inc. This is the "prove a SID voice sounds right" milestone, not the
// expressive one.
//
// Retrigger uses env.hard_restart() (§4.3's default: instantaneous reset,
// not the 6581's 1-2 frame delay bug, which isn't modelled) rather than
// gate_on()'s "only attacks if not already gated" -- a static MIDI-channel
// map means the same voice slot retriggers on every repeated note on that
// channel, and it must restart the attack even if the previous note's
// release hasn't finished.
//
// P2 (sid.md §5, §7.2): filter buses. Rendering is sub-blocked -- not the
// whole-buffer pass P1 used -- because §7.2's two-phase pass needs bus
// accumulators sized FILTER_BUS_COUNT x sub-block, not FILTER_BUS_COUNT x
// SAMPLES_PER_BUFFER, and because P3's frame VM will need a sub-block-sized
// cut point for its own tick boundary anyway. CHIP_SUBBLOCK reuses P0's
// proven value (rig.h's CHIP_RIG_SUBBLOCK default).
//
// Per-bus idle skip is sid.md §5.2's "P2 TODO": a bus with zero voices
// routed to it this sub-block skips its filter tick entirely (the ~80 c/f
// per sample §14a.9 measured for an idle-but-bound bus), derived directly
// from the same per-voice scan the render loop already does -- no separate
// bookkeeping needed from the Core 0 binding policy. bus_was_active tracks
// the 0->nonzero wake transition so a just-woken bus's SidFilter state
// resets instead of inheriting a stale resonant tail from whatever voice
// last fed it.
static constexpr uint32_t CHIP_SUBBLOCK = 64;

static SidVoice voice[MAX_VOICES];
static EnvSidRates rates;
static uint8_t last_trigger[MAX_VOICES];

static SidFilter bus_filter[FILTER_BUS_COUNT];
static bool      bus_was_active[FILTER_BUS_COUNT];
static int32_t   bus_acc[FILTER_BUS_COUNT][CHIP_SUBBLOCK];

static int32_t dry[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    uint32_t acc_scale = sid_acc_scale_q8(SID_CLOCK_PAL, SAMPLE_RATE);
    rates = env_sid_make_rates(SID_CLOCK_PAL, SAMPLE_RATE);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice[v].init(SID_MODEL_6581);   // §13 item 6: 6581 first, 8580 is P6
        last_trigger[v] = 0;
    }
    for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++) {
        bus_filter[b].init();
        bus_was_active[b] = false;
    }
    fx_delay.init();
    fx_reverb.init();

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        const VoiceParamBlock &vp = params->active();

        // Params applied once per buffer -- unchanged from P1. filter_bus
        // now flows through too, read fresh from vp per sub-block below (a
        // bus rebind mid-buffer, e.g. a new note stealing the last free
        // bus, takes effect on the very next sub-block rather than waiting
        // a whole buffer).
        uint32_t active_mask = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];
            if (p.type != VT_SID) continue;

            voice[v].osc.inc = sid_freq_to_inc(p.freq, acc_scale);
            voice[v].osc.pw = p.pw;
            voice[v].osc.waveform = p.waveform;
            voice[v].env.set_adsr(p.ad, p.sr);
            voice[v].velocity = p.velocity;

            if (p.trigger != last_trigger[v]) {
                last_trigger[v] = p.trigger;
                voice[v].env.hard_restart();
            }
            if (p.gate) voice[v].env.gate_on();
            else        voice[v].env.gate_off();

            active_mask |= (1u << v);
        }

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) dry[i] = 0;

        for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_SUBBLOCK) {
            uint32_t n = SAMPLES_PER_BUFFER - base;
            if (n > CHIP_SUBBLOCK) n = CHIP_SUBBLOCK;

            for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++)
                for (uint32_t i = 0; i < n; i++) bus_acc[b][i] = 0;

            uint32_t bus_hits[FILTER_BUS_COUNT] = {0};
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                const VoiceParams &p = vp.voices[v];
                if (p.type != VT_SID) continue;

                uint8_t bus = p.filter_bus;
                if (bus < FILTER_BUS_COUNT) {
                    for (uint32_t i = 0; i < n; i++) bus_acc[bus][i] += voice[v].tick(rates, false, 0);
                    bus_hits[bus]++;
                } else {
                    for (uint32_t i = 0; i < n; i++) dry[base + i] += voice[v].tick(rates, false, 0);
                }
            }

            for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++) {
                if (bus_hits[b] == 0) { bus_was_active[b] = false; continue; }
                if (!bus_was_active[b]) { bus_filter[b].init(); bus_was_active[b] = true; }

                const FilterBusParams &fb = vp.bus[b];
                uint8_t sid_model = (fb.model == FB_8580) ? SID_MODEL_8580 : SID_MODEL_6581;
                int16_t f_half = sid_filter_f_half(sid_model, fb.cutoff);
                int32_t q = sid_filter_q(sid_model, (uint8_t)fb.resonance);
                for (uint32_t i = 0; i < n; i++)
                    dry[base + i] += bus_filter[b].tick(bus_acc[b][i], f_half, q, fb.mode_mask, /*saturate=*/true);
            }
        }

        // Post-mix effect (delay / reverb), selected by CC74 -- same shape as
        // every other engine's insert (engine_base.h's EffectParams). Chip's
        // render is mono already (sid.md has no per-voice pan), so there is
        // no stereo downmix step before the send, unlike the stereo engines.
        bool has_fx = (vp.fx.type == FX_DELAY || vp.fx.type == FX_REVERB);
        if (vp.fx.type != s_last_fx_type) {
            if (vp.fx.type == FX_DELAY)       fx_delay.init();
            else if (vp.fx.type == FX_REVERB) fx_reverb.init();
            s_last_fx_type = vp.fx.type;
        }
        if (has_fx) {
            for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) fx_buf[i] = dry[i];
            if (vp.fx.type == FX_DELAY) fx_delay.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
            else                        fx_reverb.process(fx_buf, SAMPLES_PER_BUFFER, vp.fx);
        }

        int16_t *out = i2s_buffer_ptr(buffers, buf_index);
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
            int32_t v = dry[i];
            if (has_fx) v += fx_buf[i];
            // Mono, duplicated -- sid.md §10: the module is authentically
            // mono (no VoiceParams.pan); the speaker sim that would sit
            // downstream of this is P5, not built yet.
            int16_t s = (int16_t)__ssat(v >> SID_MIX_SHIFT, 16);
            *out++ = s;
            *out++ = s;
        }

        uint32_t busy_us = time_us_32() - t_start;
        gpio_put(PROFILE_PIN, 0);

        // No consumer yet (chip skips voice_alloc.cpp -- CMakeLists.txt
        // HAS_VOICE_ALLOC=0, static channel->voice map, §8). Pushed anyway,
        // same as the tracker engine's equivalent push: harmless, and ready
        // for a P5 LCD "active voices" telemetry read.
        multicore_fifo_push_timeout_us(active_mask, 0);

        uint32_t inst = busy_us * 100u / BUF_PERIOD_US;
        if (inst > 100) inst = 100;
        s_load_pct = (uint8_t)((uint32_t)s_load_pct - (s_load_pct >> 3) + (inst >> 3));
    }
}

#endif  // T00T_CHIP_PROFILE
