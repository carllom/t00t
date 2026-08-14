#include "audio_engine.h"
#include "rig.h"
#include "speaker_sim.h"
#include "instruments.h"
#include "chip/sid_voice.h"
#include "chip/sid_filter.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>
#include <cmath>

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

// --- Engine (sid.md §1 P1/P2/P3, §14 item 2) --------------------------------
//
// One SidVoice per MAX_VOICES slot, dispatched by VoiceType exactly like the
// groovebox (§7.3) -- VT_SILENT is skipped, VT_SID renders.
//
// Retrigger uses env.hard_restart() (§4.3's default: instantaneous reset,
// not the 6581's 1-2 frame delay bug, which isn't modelled) rather than
// gate_on()'s "only attacks if not already gated" -- a static MIDI-channel
// map means the same voice slot retriggers on every repeated note on that
// channel, and it must restart the attack even if the previous note's
// release hasn't finished.
//
// P2 (sid.md §5, §7.2): filter buses. Rendering is sub-blocked so bus
// accumulators are sized FILTER_BUS_COUNT x sub-block, not FILTER_BUS_COUNT
// x SAMPLES_PER_BUFFER. CHIP_SUBBLOCK reuses P0's proven value (rig.h's
// CHIP_RIG_SUBBLOCK default) -- and per open question 1, this is also
// P3's frame-tick cut point (see below), not a separate sample-exact one.
//
// Per-bus idle skip is sid.md §5.2's "P2 TODO": a bus with zero voices
// routed to it this sub-block skips its filter tick entirely (the ~80 c/f
// per sample §14a.9 measured for an idle-but-bound bus), derived directly
// from the same per-voice scan the render loop already does. bus_was_active
// tracks the 0->nonzero wake transition so a just-woken bus's SidFilter
// state resets instead of inheriting a stale resonant tail from whatever
// voice last fed it.
//
// P3 (sid.md §6): the frame table VM. Resolves open question 1 ("does the
// VM tick get its own cut point, or ride the existing SUBBLOCK boundary")
// as the doc's own lean predicted: rides CHIP_SUBBLOCK. ≤64 samples (1.45 ms
// on a 20 ms frame, 7%) of jitter against a true 50 Hz grid -- "likely fine,
// unlike Core 0's 29%" -- against building a second, finer cut point for a
// tick that only ever changes wave/pulse/filter/vibrato state, not the
// audio-critical bus round trip §7.2 already sub-blocks for.
//
// A note's instrument index is the *only* synthesis data Core 0 sends
// (§7.1: "the VM lives on Core 1 and Core 0 only supplies note/CC/
// instrument-select") -- INSTRUMENTS[] is const, compiled into flash, and
// identically reachable from both cores, so there is nothing to shuttle
// through ParamExchange beyond the index and the note itself. FilterBusParams
// (engine_base.h, P2) is consequently unused by chip's own rendering now:
// a bound bus's cutoff/resonance/mode come from the *feeding* voice's own
// instrument (bus_feeder_voice below), same source Core 1 already has
// locally. The struct stays in engine_base.h for engines that do want a
// live Core-0-set bus preset; chip just doesn't need it once instruments
// own their own filter data.
//
// Not wired: §4.4's per-voice sub-oscillator (mod_acc/mod_inc/mod_mode) was
// never added to the real engine's per-voice state at P1 or P2, only to the
// P0 rig and the CHIP_STRICT harness's adjacency topology. WaveRow's sync/
// ring flag bits (instrument.h) are consequently reserved, not read --
// wiring them needs the sub-oscillator built first, a real chunk of work in
// its own right, not a P3 afterthought. sync_reset/ring_msb_flip stay 0.
static constexpr uint32_t CHIP_SUBBLOCK = 64;
static constexpr uint32_t FRAME_PERIOD_SAMPLES = SAMPLE_RATE / 50;   // §6.2: 50 Hz PAL default

static SidVoice voice[MAX_VOICES];
static EnvSidRates rates;
static uint8_t last_trigger[MAX_VOICES];
static uint32_t acc_scale_g;

// Per-voice frame-VM state (Core 1 only, never crosses ParamExchange).
struct ChipVm {
    uint8_t  wave_pos, pulse_pos, filter_pos;
    uint8_t  pulse_hold, filter_hold;      // frames left in the current sweep row
    int16_t  pulse_delta, filter_delta;    // that row's delta, reapplied until hold expires
    int32_t  pulse_cur, filter_cutoff_cur; // current swept values
    int8_t   wave_note_offset;             // last wave-table row's semitone offset
    uint16_t vib_phase;
    uint16_t frames_since_trigger;
    bool     gate_off_fired;
};
static ChipVm vm[MAX_VOICES];
static uint32_t frame_sample_pos = 0;   // global frame-tick counter, sample-driven not buffer-driven

// x/12 semitone ratios in Q16, -24..+24. Computed once at boot (same "small
// runtime init table" pattern as env_sid_make_rates -- not constexpr because
// powf isn't, and this runs once, nowhere near the render loop).
static uint32_t semitone_ratio_q16[49];
static void init_semitone_ratios() {
    for (int i = -24; i <= 24; i++)
        semitone_ratio_q16[i + 24] = (uint32_t)(powf(2.0f, (float)i / 12.0f) * 65536.0f + 0.5f);
}

static SidFilter bus_filter[FILTER_BUS_COUNT];
static bool      bus_was_active[FILTER_BUS_COUNT];
static int32_t   bus_acc[FILTER_BUS_COUNT][CHIP_SUBBLOCK];

static int32_t dry[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

// Fresh instrument: first-frame waveform immediately (before any tick),
// filter cutoff seeded from the instrument's init value, everything else
// zeroed so the tables start from row 0.
static void vm_reset(uint32_t v, const Instrument &ins) {
    ChipVm &s = vm[v];
    s.wave_pos = 0; s.pulse_pos = 0; s.filter_pos = 0;
    s.pulse_hold = 0; s.filter_hold = 0;
    s.pulse_delta = 0; s.filter_delta = 0;
    s.pulse_cur = ins.pulse_init;
    s.filter_cutoff_cur = ins.filter_cutoff_init;
    s.wave_note_offset = 0;
    // 16384, not 0: the triangle mapping below has tri(phase=0) = -16384,
    // the *trough*, not the center -- starting there means vibrato jumps
    // straight to the bottom of its swing the instant the delay expires,
    // instead of easing out from zero deviation. tri(16384) = 0, rising.
    s.vib_phase = 16384;
    s.frames_since_trigger = 0;
    s.gate_off_fired = false;
    voice[v].osc.waveform = ins.first_wave;
    voice[v].osc.pw = (uint16_t)s.pulse_cur;
}

// One frame boundary's worth of table stepping for one voice (sid.md §6).
static void vm_frame_tick(uint32_t v, const Instrument &ins, uint32_t base_freq) {
    ChipVm &s = vm[v];
    s.frames_since_trigger++;

    if (ins.wave.len > 0) {
        const WaveRow &r = ins.wave.rows[s.wave_pos];
        if (r.waveform != WAVE_HOLD) voice[v].osc.waveform = r.waveform;
        s.wave_note_offset = r.note;
        s.wave_pos++;
        if (s.wave_pos >= ins.wave.len)
            s.wave_pos = (ins.wave.loop < ins.wave.len) ? ins.wave.loop : (uint8_t)(ins.wave.len - 1);
    }

    if (ins.pulse.len > 0) {
        if (s.pulse_hold == 0) {
            const SweepRow &r = ins.pulse.rows[s.pulse_pos];
            s.pulse_delta = r.delta;
            s.pulse_hold = r.duration;
            s.pulse_pos++;
            if (s.pulse_pos >= ins.pulse.len)
                s.pulse_pos = (ins.pulse.loop < ins.pulse.len) ? ins.pulse.loop : (uint8_t)(ins.pulse.len - 1);
        }
        s.pulse_cur += s.pulse_delta;
        if (s.pulse_cur < 0) s.pulse_cur = 0;
        if (s.pulse_cur > 4095) s.pulse_cur = 4095;
        if (s.pulse_hold > 0) s.pulse_hold--;
        voice[v].osc.pw = (uint16_t)s.pulse_cur;
    }

    if (ins.uses_filter && ins.filter.len > 0) {
        if (s.filter_hold == 0) {
            const SweepRow &r = ins.filter.rows[s.filter_pos];
            s.filter_delta = r.delta;
            s.filter_hold = r.duration;
            s.filter_pos++;
            if (s.filter_pos >= ins.filter.len)
                s.filter_pos = (ins.filter.loop < ins.filter.len) ? ins.filter.loop : (uint8_t)(ins.filter.len - 1);
        }
        s.filter_cutoff_cur += s.filter_delta;
        if (s.filter_cutoff_cur < 0) s.filter_cutoff_cur = 0;
        if (s.filter_cutoff_cur > 2047) s.filter_cutoff_cur = 2047;
        if (s.filter_hold > 0) s.filter_hold--;
    }

    // Arpeggio (wave-table note offset) and vibrato both apply as ratios on
    // the base register rather than recomputing from a note number, so they
    // compose for free with whatever pitch bend Core 0 already baked into
    // base_freq. Vibrato is a frame-stepped triangle LFO, not smoothed --
    // §6's "the quantisation is the sound" applies here too, same as every
    // other table. vibrato_depth is a raw register-wobble scale, not
    // calibrated to cents -- >>12 (not the original >>8, off by ~4 bits)
    // puts depth 15/40 at roughly 1.3%/3.6% deviation on a mid-range note,
    // vs. the original's 22%/58% -- a siren, not vibrato, and the likely
    // dominant cause of an early "rough/grainy" by-ear report on instruments
    // with no arpeggio at all to blame instead. Still a by-ear tuning item,
    // not a claim of correctness -- this is a better first guess, not a
    // calibrated one.
    uint32_t freq = base_freq;
    if (s.wave_note_offset != 0) {
        int off = s.wave_note_offset;
        if (off < -24) off = -24;
        if (off > 24) off = 24;
        freq = (uint32_t)(((uint64_t)freq * semitone_ratio_q16[off + 24]) >> 16);
    }
    if (ins.vibrato_depth > 0 && s.frames_since_trigger >= ins.vibrato_delay) {
        s.vib_phase = (uint16_t)(s.vib_phase + ins.vibrato_speed * 64u);
        int32_t tri = (s.vib_phase < 32768)
                          ? (int32_t)s.vib_phase - 16384
                          : 49152 - (int32_t)s.vib_phase;
        int32_t delta = (tri * (int32_t)ins.vibrato_depth) >> 12;
        int32_t signed_freq = (int32_t)freq + delta;
        freq = (uint32_t)(signed_freq < 0 ? 0 : signed_freq);
    }
    if (freq > 65535) freq = 65535;
    voice[v].osc.inc = sid_freq_to_inc((uint16_t)freq, acc_scale_g);

    if (ins.gate_off_timer > 0 && !s.gate_off_fired && s.frames_since_trigger >= ins.gate_off_timer) {
        voice[v].env.gate_off();
        s.gate_off_fired = true;
    }
}

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    acc_scale_g = sid_acc_scale_q8(SID_CLOCK_PAL, SAMPLE_RATE);
    rates = env_sid_make_rates(SID_CLOCK_PAL, SAMPLE_RATE);
    init_semitone_ratios();
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

        // Params applied once per buffer. filter_bus flows through fresh
        // per sub-block below (a bus rebind mid-buffer takes effect on the
        // next sub-block, not a whole buffer later). Instrument-driven
        // waveform/pw/filter come from vm_reset()/vm_frame_tick() now, not
        // read from VoiceParams directly -- there is no raw waveform/pw/ad/
        // sr in VoiceParams any more (§6 supersedes P1's fixed-patch fields).
        uint32_t active_mask = 0;
        for (uint32_t v = 0; v < MAX_VOICES; v++) {
            const VoiceParams &p = vp.voices[v];
            if (p.type != VT_SID) continue;

            uint8_t ins_idx = p.instrument < INSTRUMENT_COUNT ? p.instrument : 0;
            const Instrument &ins = INSTRUMENTS[ins_idx];

            voice[v].env.set_adsr(ins.ad, ins.sr);
            voice[v].velocity = p.velocity;

            if (p.trigger != last_trigger[v]) {
                last_trigger[v] = p.trigger;
                voice[v].env.hard_restart();
                // Raw base pitch, immediately -- vm_frame_tick() is the
                // ongoing authority on osc.inc from here on (arpeggio/
                // vibrato included). Reassigning it unconditionally every
                // buffer here, as an earlier version did, stomped whatever
                // vm_frame_tick() had just computed back to the unmodulated
                // base on every buffer that *didn't* also contain a frame
                // tick (roughly 2 of every 3, since a ~882-sample frame
                // period spans ~3.4 256-sample buffers) -- a buffer-rate
                // (172 Hz) snap between the true pitch and the root that
                // read as "notes too close together" and "weird waveform"
                // on ARP_LEAD, and was simply too small to notice on
                // FILTER_PAD's much subtler vibrato.
                voice[v].osc.inc = sid_freq_to_inc(p.freq, acc_scale_g);
                vm_reset(v, ins);
            }
            // vm[v].gate_off_fired guards against re-triggering: gate_off_timer
            // (below, in vm_frame_tick) calls env.gate_off() directly, which
            // clears the envelope's own internal gate flag -- but the MIDI
            // gate (p.gate) is still true for as long as the key is held, so
            // without this guard the very next buffer would see p.gate=true,
            // find the envelope no longer gated, and call gate_on() right
            // back -- a spurious full re-attack immediately after every
            // timed auto-release. vm_reset() clears the flag on a genuine
            // new trigger, so a fresh note's own gate-off timer still works.
            if (p.gate && !vm[v].gate_off_fired) voice[v].env.gate_on();
            else if (!p.gate)                    voice[v].env.gate_off();

            active_mask |= (1u << v);
        }

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) dry[i] = 0;

        for (uint32_t base = 0; base < SAMPLES_PER_BUFFER; base += CHIP_SUBBLOCK) {
            uint32_t n = SAMPLES_PER_BUFFER - base;
            if (n > CHIP_SUBBLOCK) n = CHIP_SUBBLOCK;

            // Frame tick: sample-counted, not buffer-edge-quantised (§6.1),
            // just riding CHIP_SUBBLOCK's granularity rather than a separate
            // sample-exact cut (open question 1, resolved above).
            frame_sample_pos += n;
            bool frame_tick = false;
            while (frame_sample_pos >= FRAME_PERIOD_SAMPLES) {
                frame_sample_pos -= FRAME_PERIOD_SAMPLES;
                frame_tick = true;
            }
            if (frame_tick) {
                for (uint32_t v = 0; v < MAX_VOICES; v++) {
                    const VoiceParams &p = vp.voices[v];
                    if (p.type != VT_SID) continue;
                    uint8_t ins_idx = p.instrument < INSTRUMENT_COUNT ? p.instrument : 0;
                    vm_frame_tick(v, INSTRUMENTS[ins_idx], p.freq);
                }
            }

            for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++)
                for (uint32_t i = 0; i < n; i++) bus_acc[b][i] = 0;

            uint32_t bus_hits[FILTER_BUS_COUNT] = {0};
            uint32_t bus_feeder[FILTER_BUS_COUNT];   // valid only where bus_hits[b] > 0
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                const VoiceParams &p = vp.voices[v];
                if (p.type != VT_SID) continue;

                uint8_t bus = p.filter_bus;
                if (bus < FILTER_BUS_COUNT) {
                    for (uint32_t i = 0; i < n; i++) bus_acc[bus][i] += voice[v].tick(rates, false, 0);
                    bus_hits[bus]++;
                    bus_feeder[bus] = v;
                } else {
                    for (uint32_t i = 0; i < n; i++) dry[base + i] += voice[v].tick(rates, false, 0);
                }
            }

            for (uint32_t b = 0; b < FILTER_BUS_COUNT; b++) {
                if (bus_hits[b] == 0) { bus_was_active[b] = false; continue; }
                if (!bus_was_active[b]) { bus_filter[b].init(); bus_was_active[b] = true; }

                // Bus binding is 1:1 (sid.md §5.2's "share" only applies to
                // one channel's own repeated notes, never two different
                // channels), so the feeding voice's own instrument is
                // unambiguously this bus's tonal source -- see the P3
                // comment above on why FilterBusParams goes unused here.
                const VoiceParams &fp = vp.voices[bus_feeder[b]];
                uint8_t ins_idx = fp.instrument < INSTRUMENT_COUNT ? fp.instrument : 0;
                const Instrument &ins = INSTRUMENTS[ins_idx];
                int16_t f_half = sid_filter_f_half(SID_MODEL_6581, (uint16_t)vm[bus_feeder[b]].filter_cutoff_cur);
                int32_t q = sid_filter_q(SID_MODEL_6581, ins.filter_resonance);
                for (uint32_t i = 0; i < n; i++)
                    dry[base + i] += bus_filter[b].tick(bus_acc[b][i], f_half, q, ins.filter_mode_mask, /*saturate=*/true);
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
