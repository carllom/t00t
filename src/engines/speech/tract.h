#pragma once

#include "res2p.h"
#include <cstdint>

// Formant cascade (#28, speech.md "Cascade vs parallel"): F1->F2->F3->F4->F5
// two-pole resonators in series. Cascade (not parallel) gets relative
// formant amplitudes right automatically from the bandwidths -- what makes
// vowels sound natural without per-formant amplitude data -- so no
// per-formant gain table is needed. res2p.h is pure math with no pico-sdk
// dependency, so this header is shared by the device engine
// (audio_engine.cpp) and tools/host_render/render_speech.cpp. Fricative
// branch and nasal pole are next slice (speech.md P2); this is voiced-only.
inline constexpr uint32_t SPEECH_FORMANTS = 5;

// Target F/B for one phoneme, Hz. Hardcoded per vowel in phonemes.h for
// this slice (speech.md: "the CSV-driven table generator comes later").
struct FormantTarget {
    float F[SPEECH_FORMANTS];
    float B[SPEECH_FORMANTS];
};

// Per-voice tract + excitation render state (Core 1 only, never crosses
// ParamExchange). Trimmed from speech.md's full SpeechVoice struct to what
// this voiced-only, no-sequencer slice needs: no seg_remaining/seg_index
// (no segments yet), no nasal/fric resonators (skipped this slice), no
// noise_state (no unvoiced excitation yet).
struct SpeechVoice {
    Res2p    formant[SPEECH_FORMANTS];
    float    F[SPEECH_FORMANTS] = {0}, B[SPEECH_FORMANTS] = {0};
    float    F_tgt[SPEECH_FORMANTS] = {0}, B_tgt[SPEECH_FORMANTS] = {0};
    uint32_t glottal_phase = 0;
    float    cur_amp = 0.0f;          // smoothed toward gate target, declicks on/off
    uint8_t  last_trigger = 0xFF;     // forces tract_retrigger() on first render
    uint8_t  last_phoneme = 0xFF;     // forces a target load on first render
};

// New note: snap F/B straight to the target (no glide from whatever the
// previous note left behind) and clear filter/phase state so the new note
// starts clean.
inline void tract_retrigger(SpeechVoice &sv, const FormantTarget &t) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F[i] = sv.F_tgt[i] = t.F[i];
        sv.B[i] = sv.B_tgt[i] = t.B[i];
        res2p_reset(sv.formant[i]);
    }
    sv.glottal_phase = 0;
    sv.cur_amp = 0.0f;
}

// Phoneme changed while the voice is already sounding: just move the
// target: tract_advance_subblock() ramps F/B toward it, coefficients follow.
inline void tract_set_target(SpeechVoice &sv, const FormantTarget &t) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F_tgt[i] = t.F[i];
        sv.B_tgt[i] = t.B[i];
    }
}

// ~4-5 sub-blocks (~12-15 ms at SPEECH_SUBBLOCK/SPEECH_RATE) to settle on a
// new target -- enough to avoid a click on a mid-note phoneme change, short
// enough that P1's static, unchanging target reaches it well within a note.
inline constexpr float TRACT_RAMP_COEFF = 0.25f;

// Ramp F/B one sub-block toward target and recompute every resonator's
// coefficients from the ramped values -- never from the target directly,
// and never by interpolating a1/a2/b0 (speech.md "Resonator and the
// stability rule"). res2p_set()'s own debug assert(a2 < 1.0f) is the
// backstop against a coefficient set that isn't stable.
inline void tract_advance_subblock(SpeechVoice &sv, float fs) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F[i] += (sv.F_tgt[i] - sv.F[i]) * TRACT_RAMP_COEFF;
        sv.B[i] += (sv.B_tgt[i] - sv.B[i]) * TRACT_RAMP_COEFF;
        res2p_set(sv.formant[i], sv.F[i], sv.B[i], fs);
    }
}

inline float tract_process(SpeechVoice &sv, float excitation) {
    float y = excitation;
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) y = res2p_tick(sv.formant[i], y);
    return y;
}
