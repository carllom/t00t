#pragma once

#include "excitation.h"
#include "osc/sine.h"
#include "pan.h"
#include "phonemes.h"
#include "tract.h"
#include <cstdint>

// Shared core of the speech engine skeleton (#27): renders `native_frames`
// samples of a fixed test tone at the engine's native rate and zero-order-
// holds each one x2 into the (output-rate-sized) dry_l/dry_r buffers, i.e.
// dry_{l,r}[2*i] == dry_{l,r}[2*i+1] for every native sample i. Both the
// device path (engines/speech/audio_engine.cpp, called from the Core 1
// render loop) and the host path (tools/host_render/render_speech.cpp) call
// this exact function, so the ZOH seam and the frame-count arithmetic around
// it are proven identical on both before any formant DSP exists.
//
// Deliberately has no pico-sdk dependency (osc/sine.h, pan.h, excitation.h,
// tract.h and phonemes.h are all header-only, common-layer DSP) -- pulling
// in engine.h/engine_base.h here instead would drag in hardware/sync.h,
// which the standalone host build has no access to (see
// render_tracker_mixer.cpp's equivalent comment).
inline void speech_render_test_tone(uint32_t &phase, uint32_t phase_inc, int16_t pan,
                                     int32_t *dry_l, int32_t *dry_r, uint32_t native_frames) {
    constexpr int16_t TEST_TONE_AMPLITUDE = 16384;  // fixed headroom, no envelope yet
    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    for (uint32_t i = 0; i < native_frames; i++) {
        int32_t sample = (osc_sine(phase) * TEST_TONE_AMPLITUDE) >> 15;
        int32_t l = (sample * gain_l) >> 15;
        int32_t r = (sample * gain_r) >> 15;
        uint32_t o = i * 2;
        dry_l[o] = l; dry_l[o + 1] = l;
        dry_r[o] = r; dry_r[o + 1] = r;
        phase += phase_inc;
    }
}

// Sub-block size: how often coefficients get recomputed from ramped F/B
// (speech.md "Timing Domains": "Sub-block <=64 frames (1.45 ms)"). Same
// value as the tracker's TRACKER_SUBBLOCK, for the same reason -- a unit of
// parameter constancy, not of output.
static constexpr uint32_t SPEECH_SUBBLOCK = 64;

// Headroom applied to the glottal excitation before it hits the formant
// cascade: each res2p stage has unity DC gain, but a narrow-bandwidth
// resonator has real gain at its own resonant frequency (roughly 1/(1-r)),
// and five of them in series compounds that. Without this the cascade
// clips well before `amplitude` reaches its nominal full-scale value.
// Tuned empirically in tools/host_render/render_speech.cpp against the
// five vowel targets in phonemes.h.
static constexpr float SPEECH_EXCITATION_HEADROOM = 1.0f / 12.0f;

// Renders `native_frames` samples of one voice's formant-cascade output
// (#28) at the engine's native rate, panned and zero-order-held x2 into the
// (output-rate-sized) dry_l/dry_r accumulators -- callers must clear those
// buffers themselves and may call this once per active voice, since it
// accumulates (+=) rather than overwrites. `sv` is this voice's persistent
// per-voice render state (Res2p filters, ramped F/B, glottal phase); `fs` is
// the native rate in Hz (passed rather than baked in, same reasoning as
// speech_render_test_tone's `phase_inc`). `trigger` is VoiceParams::trigger
// -- a change vs. `sv.last_trigger` means a new note, so the tract snaps
// straight to the new phoneme's target and clears filter/phase state
// (tract_retrigger()) instead of gliding from whatever the previous note
// left behind.
inline void speech_render_voice(SpeechVoice &sv, uint32_t phase_inc, float fs, uint8_t trigger,
                                 int16_t amplitude, bool gate, uint8_t phoneme, int16_t pan,
                                 int32_t *dry_l, int32_t *dry_r, uint32_t native_frames) {
    const FormantTarget &tgt = VOWEL_TARGETS[phoneme % VOWEL_COUNT];
    if (trigger != sv.last_trigger) {
        sv.last_trigger = trigger;
        sv.last_phoneme = phoneme;
        tract_retrigger(sv, tgt);
    } else if (phoneme != sv.last_phoneme) {
        sv.last_phoneme = phoneme;
        tract_set_target(sv, tgt);
    }

    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    // Amplitude declick time constant: fast enough (~100 samples, ~4.5 ms at
    // SPEECH_RATE) that gate on/off doesn't thump, slow enough to stay well
    // under the tract's own ~12-15 ms glide so it never sounds like part of
    // the formant transition.
    constexpr float AMP_SMOOTH_COEFF = 0.01f;

    uint32_t n = 0;
    while (n < native_frames) {
        uint32_t k = native_frames - n;
        if (k > SPEECH_SUBBLOCK) k = SPEECH_SUBBLOCK;

        tract_advance_subblock(sv, fs);
        float amp_tgt = (gate ? (float)amplitude : 0.0f) * SPEECH_EXCITATION_HEADROOM;

        for (uint32_t i = 0; i < k; i++) {
            sv.cur_amp += (amp_tgt - sv.cur_amp) * AMP_SMOOTH_COEFF;
            float excitation = glottal_pulse(sv.glottal_phase) * sv.cur_amp;
            sv.glottal_phase += phase_inc;

            int32_t sample = (int32_t)tract_process(sv, excitation);
            int32_t l = (sample * gain_l) >> 15;
            int32_t r = (sample * gain_r) >> 15;
            uint32_t o = (n + i) * 2;
            dry_l[o] += l; dry_l[o + 1] += l;
            dry_r[o] += r; dry_r[o + 1] += r;
        }
        n += k;
    }
}
