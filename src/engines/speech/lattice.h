#pragma once

#include "excitation.h"
#include <cassert>
#include <cstdint>

// LPC lattice tract: a 10th-order all-pole lattice filter, the sibling to
// tract.h's formant resonator cascade. Reuses excitation.h's glottal pulse
// train and LFSR noise unchanged -- only the tract filter itself and its
// frame data are new. No pico-sdk dependency, same reasoning as tract.h/
// render.h (see render.h's own header comment): shared by the device engine
// and host tooling.
//
// Renders natively at 8 kHz, the TMS5220's real frame rate, instead of
// tract.h's 22.05 kHz -- a plosive burst's spectral detail needs the
// headroom formants live in, but an all-pole lattice modeling a full vocal
// tract in one filter doesn't reach past 4 kHz either way, so halving again
// costs nothing audible while halving render cost a second time.
inline constexpr uint32_t SPEECH_LATTICE_ORDER = 10;
inline constexpr uint32_t SPEECH_LATTICE_RATE = 8000;

// Frame `gain` (LatticeFrame, below) is a Levinson-Durbin prediction-error
// gain -- correct for reproducing a white-noise-driven signal's energy, but
// this tract's excitation is a glottal pulse train, not white noise, so a
// pulse's much higher crest factor comes out noticeably quieter than the
// same nominal gain would for noise. Tuned empirically against the
// converted Talkie corpus's worst-case resonance (tools/talkie2lattice.py),
// not LATTICE_TEST_WORD -- the corpus's real chip-recorded reflection
// coefficients sit much closer to the unit circle than the test word's own
// smoother, synthetic ones, so the same excitation scale that never clips
// the corpus leaves the test word quiet by comparison.
inline constexpr float SPEECH_LATTICE_GAIN_BOOST = 2.5f;

// Live pitch-shift multiplier range (module_speech.md "MIDI Mapping"), a
// live CC override on top of a word's own recorded per-frame pitch
// contour -- one octave down to one octave up.
inline constexpr float SPEECH_LATTICE_PITCH_SHIFT_MIN = 0.5f, SPEECH_LATTICE_PITCH_SHIFT_MAX = 2.0f;

// TMS5220 "chirp" excitation table (module_speech.md "LPC Lattice Tract"):
// the real chip's own voiced-excitation waveform, decap-verified data --
// re-expressed here as this project's own array, not copied file text,
// the same standing talkie2lattice.py's own K-coefficient tables already
// have (chip hardware behavior, not any one emulator's creative work).
// Cross-checked against two independent MAME source trees: the current
// mame/src/devices/sound/tms5110r.hxx TI_LATER_CHIRP table (used by
// TMS5110A/TMS5200/TMS5220 -- the chip family this tract targets) and the
// older historic-mame single chirptable[] (used by the earlier
// TMS5100/TMC0281 and kept only as an independent check on the fetch
// itself, not as this table's source -- its values differ, correctly,
// since it's a different, earlier chip). Values are the table's own raw
// bytes, unnormalized; lattice_chirp_pulse() below normalizes them.
//
// Real voiced speech has an excitation shape much closer to this --  a
// short, sharp burst at the start of each pitch period followed by
// silence for the rest of it -- than excitation.h's glottal_pulse(), a
// smooth bipolar triangle spanning the whole period. That difference is
// most of what gives the TMS5220 its characteristic buzzy edge, which the
// shared, smoother triangle doesn't reproduce.
inline constexpr uint32_t LATTICE_CHIRP_LENGTH = 52;
inline constexpr int16_t LATTICE_CHIRP_TABLE[LATTICE_CHIRP_LENGTH] = {
    0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50,
    0x25, 0x26, 0x4c, 0x44, 0x1a, 0x32, 0x3b, 0x13,
    0x37, 0x1a, 0x25, 0x1f, 0x1d, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
// The table's own peak magnitude (0x71 = 113) -- normalizing by this,
// not an assumed full-scale 127, reproduces the real chip's actual
// headroom instead of over- or under-driving a rescaled version of it.
inline constexpr float LATTICE_CHIRP_PEAK = 113.0f;

// `sample_in_period` is how many native samples into the current pitch
// period this sample is (render.h tracks it in LatticeVoiceState::chirp_idx,
// reset on every glottal-phase wraparound) -- the real chip indexes its
// chirp ROM the same way, sample-locked to the pitch period, and holds at
// the table's last (zero) entry once the period outlasts it. Unlike
// glottal_pulse()'s continuous phase-fraction lookup, this is a discrete,
// per-sample counter, matching how the table itself is sample-quantized
// hardware data, not a continuously-defined waveform.
inline float lattice_chirp_pulse(uint32_t sample_in_period) {
    uint32_t idx = sample_in_period < LATTICE_CHIRP_LENGTH ? sample_in_period : LATTICE_CHIRP_LENGTH - 1;
    return (float)LATTICE_CHIRP_TABLE[idx] * (1.0f / LATTICE_CHIRP_PEAK);
}

// One 25 ms coefficient frame -- the TMS5220's own frame period, at the
// lattice's 8 kHz native rate that's exactly 200 samples. `pitch_hz == 0`
// marks an unvoiced/silent frame (LFSR noise excitation, scaled by `gain`,
// which is 0 for true silence).
struct LatticeFrame {
    float k[SPEECH_LATTICE_ORDER];  // reflection coefficients, |k[i]| < 1
    float gain;                      // excitation scale for this frame
    float pitch_hz;                  // 0 = unvoiced/silent
};

struct LatticeWord {
    const LatticeFrame *frames;
    uint16_t length;
};

inline constexpr uint32_t SPEECH_LATTICE_FRAME_SAMPLES = 200;  // 25 ms @ 8 kHz
// The TMS5220 interpolated its reflection coefficients 8 times per 25 ms
// frame -- every 25 samples at 8 kHz. Matched here rather than picking an
// arbitrary sub-block size.
inline constexpr uint32_t SPEECH_LATTICE_SUBBLOCK = 25;
inline constexpr uint32_t SPEECH_LATTICE_INTERP_STEPS = SPEECH_LATTICE_FRAME_SAMPLES / SPEECH_LATTICE_SUBBLOCK;
static_assert(SPEECH_LATTICE_FRAME_SAMPLES % SPEECH_LATTICE_SUBBLOCK == 0,
              "a coefficient frame must divide evenly into interpolation sub-blocks");

// Per-voice lattice-tract render state (tract.h's SpeechVoice union member).
// `b[]` is the lattice's backward-residual delay line (b_0..b_{ORDER-1}, one
// sample of memory per stage) -- the lattice's only filter memory, playing
// the same role tract.h's Res2p::s1/s2 play for the formant cascade.
// `k`/`gain` ramp toward `k_tgt`/`gain_tgt` linearly over
// SPEECH_LATTICE_INTERP_STEPS sub-blocks (`k_step`/`gain_step`, set once per
// frame load) -- unlike tract.h's F/B, which ramp toward a target and get
// re-derived into resonator coefficients every sub-block, `k` *is* the
// filter coefficient, so ramping it directly is the render cost, not a
// step before one. This is safe specifically because the interval (-1, 1)
// is convex: interpolating between two in-range reflection coefficients can
// never leave that range, so the filter can't go unstable mid-glide the way
// interpolating biquad coefficients can (tract.h's stability rule).
// `resample_frac`/`y_prev`/`y_cur` are the fractional-ratio upsampler's
// state (44.1 kHz/8 kHz has no integer shortcut) -- carried across render
// calls so buffer boundaries don't introduce phase drift. `chirp_idx` is
// lattice_chirp_pulse()'s own state -- how many native samples into the
// current pitch period, reset on every glottal-phase wraparound -- kept
// unconditionally regardless of which exciter is selected, since tracking
// it is cheap and it means a mid-note exciter switch has correct state
// from its very first sample instead of a stale or default one.
struct LatticeVoiceState {
    float    k[SPEECH_LATTICE_ORDER] = {0};
    float    k_step[SPEECH_LATTICE_ORDER] = {0};
    float    gain = 0.0f, gain_step = 0.0f;
    float    b[SPEECH_LATTICE_ORDER] = {0};
    uint16_t frame_index = 0;
    uint32_t frame_remaining = 0;
    uint32_t phase_inc = 0;    // this frame's glottal phase increment, at SPEECH_LATTICE_RATE
    bool     voiced = false;   // this frame's excitation source: glottal pulse vs. LFSR noise
    bool     word_done = false;
    float    resample_frac = 0.0f;
    float    y_prev = 0.0f, y_cur = 0.0f;
    uint32_t chirp_idx = 0;
};

inline void lattice_reset(LatticeVoiceState &ls) {
    for (uint32_t i = 0; i < SPEECH_LATTICE_ORDER; i++) {
        ls.k[i] = ls.k_step[i] = 0.0f;
        ls.b[i] = 0.0f;
    }
    ls.gain = ls.gain_step = 0.0f;
    ls.frame_index = 0;
    ls.frame_remaining = 0;
    ls.phase_inc = 0;
    ls.voiced = false;
    ls.word_done = false;
    ls.resample_frac = 0.0f;
    ls.y_prev = ls.y_cur = 0.0f;
    ls.chirp_idx = 0;
}

// Loads frame `idx` of `w` into `ls`: `retrigger` snaps straight to the
// frame's coefficients (a new note -- no glide from whatever the previous
// note left behind, matching tract_retrigger()'s same rule for the formant
// tract); otherwise sets up a linear ramp from the current coefficients to
// this frame's, covering SPEECH_LATTICE_INTERP_STEPS sub-blocks.
// `pitch_mult` scales the frame's own recorded pitch_hz before it becomes a
// phase increment -- the live pitch-shift CC's override, re-applied on
// every frame load so a CC change reaches a word already in progress within
// one frame period.
inline void lattice_load_frame(LatticeVoiceState &ls, const LatticeWord &w, uint16_t idx, bool retrigger,
                                float pitch_mult) {
    const LatticeFrame &f = w.frames[idx];
    for (uint32_t i = 0; i < SPEECH_LATTICE_ORDER; i++) {
        if (retrigger) ls.k[i] = f.k[i];
        ls.k_step[i] = (f.k[i] - ls.k[i]) * (1.0f / (float)SPEECH_LATTICE_INTERP_STEPS);
    }
    if (retrigger) ls.gain = f.gain;
    ls.gain_step = (f.gain - ls.gain) * (1.0f / (float)SPEECH_LATTICE_INTERP_STEPS);
    ls.frame_index = idx;
    ls.frame_remaining = SPEECH_LATTICE_FRAME_SAMPLES;
    ls.voiced = f.pitch_hz > 0.0f;
    ls.phase_inc = ls.voiced ? glottal_phase_inc(f.pitch_hz * pitch_mult, (float)SPEECH_LATTICE_RATE) : 0;
}

// Advances to the next coefficient frame once the current one's
// frame_remaining reaches zero -- sequencer.h's speech_sequencer_advance(),
// adapted to a fixed frame array instead of a phoneme string. `loop` is
// `mode == SPEECH_MODE_LOOP && gate` (render.h) -- SpeechMode itself isn't
// known here, the same split render.h keeps between sequencer.h's mode
// logic and lattice.h's frame data. Reaching the end of the word without
// looping marks it done; render.h's speech_render_voice_lattice() uses
// `word_done` to stop rendering and to clear the active-voice bitmap, same
// contract as SpeechVoice::seq_done.
inline void lattice_advance(LatticeVoiceState &ls, const LatticeWord &w, bool loop, float pitch_mult) {
    uint16_t next = (uint16_t)(ls.frame_index + 1);
    if (next >= w.length) {
        if (loop) {
            next = 0;
        } else {
            ls.word_done = true;
            // Must be nonzero for the same reason speech_sequencer_advance()'s
            // end-of-utterance branch leaves seg_remaining nonzero: the render
            // loop cuts each iteration at min(..., frame_remaining, ...), and a
            // stale 0 here would spin the loop without ever consuming a sample.
            ls.frame_remaining = 0xFFFFFFFFu;
            return;
        }
    }
    lattice_load_frame(ls, w, next, /*retrigger=*/false, pitch_mult);
}

// Ramps k/gain one sub-block toward this frame's target and asserts the
// lattice-only exception to tract.h's stability rule: interpolating between
// two |k[i]| < 1 coefficients can't leave that range (see LatticeVoiceState's
// comment), so this assert only ever fires on bad frame data, not on the
// interpolation itself.
inline void lattice_advance_subblock(LatticeVoiceState &ls) {
    for (uint32_t i = 0; i < SPEECH_LATTICE_ORDER; i++) {
        ls.k[i] += ls.k_step[i];
        assert(ls.k[i] > -1.0f && ls.k[i] < 1.0f);
    }
    ls.gain += ls.gain_step;
}

// One sample through the order-10 all-pole synthesis lattice (Markel &
// Gray's standard recursion): `excitation` is this sample's glottal-pulse-
// or-noise source, already scaled by frame gain and amplitude. `ls.b[]`
// holds each stage's backward-residual sample from the previous tick --
// the lattice's entire filter memory.
inline float lattice_tick(LatticeVoiceState &ls, float excitation) {
    float f = excitation;
    float b_new[SPEECH_LATTICE_ORDER];
    for (int32_t stage = (int32_t)SPEECH_LATTICE_ORDER - 1; stage >= 0; stage--) {
        float b_prev = ls.b[stage];
        float f_prev = f - ls.k[stage] * b_prev;
        b_new[stage] = b_prev + ls.k[stage] * f_prev;
        f = f_prev;
    }
    ls.b[0] = f;  // b_0[n] == f_0[n], the output itself
    for (uint32_t j = 1; j < SPEECH_LATTICE_ORDER; j++) ls.b[j] = b_new[j - 1];
    return f;
}

// Hardcoded test word (module_speech.md's "LPC sibling engine" bring-up
// slice): no corpus converter exists yet, so this is a small, hand-built
// fixture, not real vocabulary. Coefficients are Levinson-Durbin analyses
// (order 10, 8 kHz) of this project's own /i/, /a/, /u/ formant-cascade
// impulse responses (phonemes.h's PH_I/PH_A/PH_U targets) -- a real LPC
// analysis of a known-stable signal, not hand-guessed numbers, and every
// resulting |k| comes out well inside (-1, 1) (largest magnitude ~0.90).
// Each vowel repeats 4 frames (100 ms) to be clearly holdable; a silent
// frame brackets both ends.
inline constexpr LatticeFrame LATTICE_TEST_SIL = { {0,0,0,0,0,0,0,0,0,0}, 0.0f, 0.0f };
inline constexpr LatticeFrame LATTICE_TEST_I =
    { { 0.8985f, 0.9008f, 0.8034f, 0.2391f, -0.3671f, -0.4523f, -0.4470f, 0.0996f, 0.5910f, 0.5753f },
      0.1331f, 130.0f };
inline constexpr LatticeFrame LATTICE_TEST_A =
    { { 0.8383f, -0.1447f, -0.2475f, 0.5225f, 0.4721f, 0.2501f, 0.0272f, -0.0776f, 0.3329f, 0.5436f },
      0.2020f, 115.0f };
inline constexpr LatticeFrame LATTICE_TEST_U =
    { { 0.3138f, -0.7581f, -0.1682f, 0.5491f, 0.0802f, -0.0994f, -0.1060f, -0.1362f, 0.3787f, 0.5807f },
      0.0209f, 105.0f };

inline constexpr LatticeFrame LATTICE_TEST_FRAMES[] = {
    LATTICE_TEST_SIL,
    LATTICE_TEST_I, LATTICE_TEST_I, LATTICE_TEST_I, LATTICE_TEST_I,
    LATTICE_TEST_A, LATTICE_TEST_A, LATTICE_TEST_A, LATTICE_TEST_A,
    LATTICE_TEST_U, LATTICE_TEST_U, LATTICE_TEST_U, LATTICE_TEST_U,
    LATTICE_TEST_SIL,
};
inline constexpr LatticeWord LATTICE_TEST_WORD = {
    LATTICE_TEST_FRAMES, sizeof(LATTICE_TEST_FRAMES) / sizeof(LATTICE_TEST_FRAMES[0])
};
