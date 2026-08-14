#include "audio_engine.h"
#include "render.h"
#include "phrases.h"
#include "fx/delay.h"
#include "fx/reverb.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <arm_acle.h>

// Phoneme keyboard (module_speech.md "SPEECH_HOLD"): MAX_VOICES independent
// tract voices, each driven straight from
// VoiceParams (phase_inc = glottal pitch, phoneme = sustained vowel/
// fricative/nasal, gate = held/released) with no segment sequencer -- one
// MIDI note is one sustained phoneme. render.h's speech_render_voice() is
// the shared device/host render core (tools/host_render/render_speech.cpp
// calls the same function to render each phoneme to WAV).

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

// Stereo dry mix at output rate (44.1 kHz, post-ZOH). speech_render_voice()
// accumulates (+=) so up to MAX_VOICES can be mixed -- callers must clear
// both buffers first. `fx_buf` is the mono send/return scratch for the
// post-mix effect (mono send / stereo return).
static int32_t dry_l[SAMPLES_PER_BUFFER];
static int32_t dry_r[SAMPLES_PER_BUFFER];
static int32_t fx_buf[SAMPLES_PER_BUFFER];

// Per-voice render state (Core 1 only, never crosses ParamExchange).
static SpeechVoice voices[MAX_VOICES];

// Display telemetry (engine.h): published once per buffer by the normal
// (non-profiling) loop below. The profiling build's synthetic loop has no
// display to feed (see its own comment) and leaves this at its
// PHONEME_COUNT ("none") default.
static SpeechVoiceUiState s_voice_ui[MAX_VOICES];

void speech_voice_ui_state(uint32_t voice, SpeechVoiceUiState *out) {
    *out = (voice < MAX_VOICES) ? s_voice_ui[voice] : SpeechVoiceUiState{0, 0, PHONEME_COUNT, false};
}

// Post-mix effects (Core 1 only), linked unconditionally.
static FxDelay  fx_delay;
static FxReverb fx_reverb;
static uint8_t  s_last_fx_type = 0xFF;

#if defined(T00T_SPEECH_PROFILE) && T00T_SPEECH_PROFILE

// Profiling rig (T00T_SPEECH_PROFILE build): replaces the normal
// MIDI-driven loop below with a self-cycling, pin-only measurement build --
// no display, no stdio. Drives `voices[]` directly with synthetic content
// instead of going through ParamExchange, so each phase isolates exactly
// one cost: voice count (1/2/4/8), held vowel vs. voiced fricative,
// coefficient-recompute vs. per-sample cost, and static vs. actively-swept
// formant_shift/bandwidth_scale. MAX_VOICES is 8 in this build (engine.h),
// so the 8-voice phase has a real 8th slot.
struct ProfilePhase {
    uint32_t voice_count;     // voices 0..voice_count-1 render; rest stay idle
    uint8_t  phoneme;         // PH_A (vowel) or PH_Z (voiced fricative)
    bool     recompute_only;  // true: call tract_advance_subblock() only --
                               // skips the per-sample tract_process_mixed()
                               // loop, isolating coefficient-recompute cost
                               // from per-sample cost
    bool     sweep_tract;     // true: sweep formant_shift/bandwidth_scale
                               // across their full CC range every buffer
                               // instead of holding them at 1.0x
};

static constexpr ProfilePhase PROFILE_PHASES[] = {
    { 0, PH_A, false, false },  // idle: isolates fixed per-buffer overhead
    { 1, PH_A, false, false },
    { 2, PH_A, false, false },
    { 4, PH_A, false, false },
    { 8, PH_A, false, false },
    { 4, PH_Z, false, false },  // vs. the 4v phase above: voiced-fricative branch cost
    { 4, PH_A, true,  false },  // vs. the 4v phase above: coefficient-recompute-only cost
    { 4, PH_A, false, true  },  // vs. the 4v phase above: static vs. swept tract params
};
static constexpr uint32_t PROFILE_PHASE_COUNT = sizeof(PROFILE_PHASES) / sizeof(PROFILE_PHASES[0]);

// ~4s per phase, long enough for a stable scope/logic analyzer reading.
static constexpr uint32_t PROFILE_HOLD_BUFFERS = (4000000u / BUF_PERIOD_US) + 1;

// Fixed ~110 Hz glottal pitch -- pitch is irrelevant to render cost, so one
// value for the whole rig keeps every phase directly comparable.
static const uint32_t PROFILE_PHASE_INC = glottal_phase_inc(110.0f, (float)SPEECH_RATE);
static constexpr int16_t PROFILE_AMPLITUDE = 32767;

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    (void)params;  // synthetic content only -- MIDI input is ignored in this build

    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    res2p_init();
    osc_init_sine();
    for (uint32_t v = 0; v < MAX_VOICES; v++) voices[v] = SpeechVoice{};

    uint32_t phase_idx = 0, phase_buf_count = 0, sweep_step = 0;
    uint8_t trigger = 0;

    while (true) {
        uint32_t buf_index = multicore_fifo_pop_blocking();
        const ProfilePhase &ph = PROFILE_PHASES[phase_idx];

        if (phase_buf_count == 0) {
            // New phase: retrigger every voice this phase uses straight onto
            // its target, matching what a real note-on does (tract_retrigger())
            // -- and mark it so speech_render_voice() below doesn't redo the
            // same work on every buffer of the phase.
            trigger++;
            FormantTarget t = phoneme_unpack(PHONEME_TARGETS[ph.phoneme % PHONEME_COUNT]);
            for (uint32_t v = 0; v < ph.voice_count; v++) {
                tract_retrigger(voices[v], t);
                voices[v].last_trigger = trigger;
                voices[v].last_phoneme = ph.phoneme;
            }
            sweep_step = 0;
        }

        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER; i++) { dry_l[i] = 0; dry_r[i] = 0; }

        int16_t formant_shift = 256, bandwidth_scale = 256;  // 1.0x, static default
        if (ph.sweep_tract) {
            // Triangle sweep across the full live-CC range every buffer,
            // same range tract.h's CC handler uses -- exercises exactly what
            // a CC74-style live sweep does to the per-voice ramp targets.
            sweep_step = (sweep_step + 1) % 256;
            uint8_t tri = (uint8_t)(sweep_step < 128 ? sweep_step : (255 - sweep_step));
            formant_shift   = tract_cc_to_q8_8(tri, FORMANT_SHIFT_MIN, FORMANT_SHIFT_MAX);
            bandwidth_scale = tract_cc_to_q8_8((uint8_t)(127 - tri), BANDWIDTH_SCALE_MIN, BANDWIDTH_SCALE_MAX);
        }

        gpio_put(PROFILE_PIN, 1);
        uint32_t t_start = time_us_32();

        for (uint32_t v = 0; v < ph.voice_count; v++) {
            if (ph.recompute_only) {
                voices[v].formant_shift_tgt = (float)formant_shift * (1.0f / 256.0f);
                voices[v].bandwidth_scale_tgt = (float)bandwidth_scale * (1.0f / 256.0f);
                uint32_t n = 0;
                while (n < NATIVE_SAMPLES_PER_BUFFER) {
                    uint32_t k = NATIVE_SAMPLES_PER_BUFFER - n;
                    if (k > SPEECH_SUBBLOCK) k = SPEECH_SUBBLOCK;
                    tract_advance_subblock(voices[v], (float)SPEECH_RATE);
                    n += k;
                }
            } else {
                speech_render_voice(voices[v], PROFILE_PHASE_INC, (float)SPEECH_RATE, trigger,
                                     PROFILE_AMPLITUDE, true, ph.phoneme, 0,
                                     formant_shift, bandwidth_scale,
                                     /*jitter=*/0, /*shimmer=*/0, /*lfo_rate=*/0.0f, /*lfo_depth=*/0.0f,
                                     dry_l, dry_r, NATIVE_SAMPLES_PER_BUFFER);
            }
        }

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

        if (++phase_buf_count >= PROFILE_HOLD_BUFFERS) {
            phase_buf_count = 0;
            phase_idx = (phase_idx + 1) % PROFILE_PHASE_COUNT;
        }
    }
}

#else

void audio_engine_run(AudioBuffers *buffers, ParamExchange *params) {
    // Init profiling pin
    gpio_init(PROFILE_PIN);
    gpio_set_dir(PROFILE_PIN, GPIO_OUT);
    gpio_put(PROFILE_PIN, 0);

    res2p_init();    // must run before any res2p_radius()/res2p_set() call
    osc_init_sine();  // pan_gains_q15() (speech_render_voice) reads this table --
                       // easy to drop by mistake: skipping it multiplies every
                       // sample by gain 0 from an all-zero wavetable, silent
                       // output despite correct DSP upstream.
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voices[v] = SpeechVoice{};
        s_voice_ui[v] = { 0, 0, PHONEME_COUNT, false };
    }
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
            if (p.utterance == SPEECH_NO_UTTERANCE) {
                // Phoneme keyboard: one sustained phoneme, no sequencer --
                // active mirrors gate directly, since there's no utterance
                // to outlast it.
                speech_render_voice(voices[v], p.phase_inc, (float)SPEECH_RATE, p.trigger,
                                     p.amplitude, p.gate, p.phoneme, p.pan,
                                     p.formant_shift, p.bandwidth_scale,
                                     p.jitter, p.shimmer, p.lfo_rate, p.lfo_depth,
                                     dry_l, dry_r, NATIVE_SAMPLES_PER_BUFFER);
                if (p.gate) active_mask |= (1u << v);
                s_voice_ui[v] = { (uint16_t)voices[v].F[0], (uint16_t)voices[v].F[1], p.phoneme, p.gate };
            } else {
                // Segment sequencer: active stays set until the
                // utterance itself completes (sv.active, sequencer.h/
                // render.h), which under SPEECH_MODE_GATED can outlive
                // note-off by up to the release segment's own duration.
                // SPEECH_PHRASES (phrases.h, generated by tools/speechgen.py
                // gen-phrases) is the utterance source here -- same
                // SpeechUtterance shape as utterance.h's hand-picked HELLO/CAT
                // fixtures, which stay in place only for
                // tools/host_render/render_speech.cpp's regression checks,
                // which need their exact known phoneme strings, not whatever
                // speech_phrases.txt currently says.
                const SpeechUtterance &utt = SPEECH_PHRASES[p.utterance % SPEECH_PHRASE_COUNT];
                speech_render_voice_seq(voices[v], p.phase_inc, (float)SPEECH_RATE, p.trigger,
                                         p.amplitude, p.gate, utt,
                                         p.mode, p.rate, p.pan, p.formant_shift, p.bandwidth_scale,
                                         p.jitter, p.shimmer, p.lfo_rate, p.lfo_depth,
                                         dry_l, dry_r, NATIVE_SAMPLES_PER_BUFFER);
                if (voices[v].active) active_mask |= (1u << v);
                // The sequencer (sequencer.h) tracks position as seg_index
                // into `utt`, not a phoneme id directly -- resolve it here,
                // same lookup speech_seg_load() itself does.
                uint8_t cur_phoneme = (voices[v].seg_index < utt.length) ? utt.phonemes[voices[v].seg_index] : PH_SIL;
                s_voice_ui[v] = { (uint16_t)voices[v].F[0], (uint16_t)voices[v].F[1], cur_phoneme, voices[v].active };
            }
        }

        // Post-mix effect (delay / reverb, selected by CC74). Mono send /
        // stereo return: downmix the stereo dry mix to mono, run the
        // (still-mono) effect on it, then add its wet output identically to
        // both channels. Clear the newly selected effect's buffer on a type
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

#endif  // T00T_SPEECH_PROFILE
