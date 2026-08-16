#pragma once

#include "res2p.h"
#include <cstdint>

// S.A.M. tract: three independently-driven formant resonators summed in
// parallel, unlike tract.h's five-formant cascade where each stage feeds
// the next. Summing separate resonances instead of chaining them gives a
// buzzier, less smooth character on its own, before any pitch-contour or
// reciter work exists. No pico-sdk dependency, so this header is shared by
// the device engine (audio_engine.cpp) and tools/host_render/render_speech.cpp.
inline constexpr uint32_t SAM_FORMANTS = 3;

// Target state for one allophone: three independent formant resonances
// (frequency, bandwidth, amplitude weight) plus one frication-branch target
// that approximates an unvoiced consonant through the shared noise
// excitation rather than a sampled burst. `duration_ms` is read only by the
// sequenced (phrase) render path, not the phoneme-keyboard HOLD path.
struct SamAllophoneTarget {
    float F[SAM_FORMANTS];
    float B[SAM_FORMANTS];
    float amp[SAM_FORMANTS];  // per-formant parallel amplitude weight, 0..1
    float fric_F, fric_B;
    float af;                 // frication-branch amplitude weight, 0..1
    uint8_t duration_ms;
};

// Per-voice render state: SpeechVoice's third union member (tract.h). One
// Res2p per formant, each driven and summed independently rather than
// chained; `fric` shapes the shared LFSR noise excitation the same way as
// the formant tract's own fricative branch, giving unvoiced consonants
// their own spectral shape without a second excitation source.
// `pitch_offset`/`pitch_offset_tgt` (semitones, ramped the same way as
// F/B/amp) are the sequenced render path's pitch-contour state -- a
// property of *where in a phrase* a segment sits (stressed vs. not), not
// of the allophone itself, so they're set/ramped independently of
// sam_retrigger()/sam_set_target() below by sam_snap_pitch()/
// sam_set_pitch_target().
struct SamVoiceState {
    Res2p formant[SAM_FORMANTS];
    Res2p fric;
    float F[SAM_FORMANTS] = {0}, B[SAM_FORMANTS] = {0};
    float F_tgt[SAM_FORMANTS] = {0}, B_tgt[SAM_FORMANTS] = {0};
    float amp[SAM_FORMANTS] = {0}, amp_tgt[SAM_FORMANTS] = {0};
    float fric_F = 0, fric_B = 0, fric_F_tgt = 0, fric_B_tgt = 0;
    float af = 0, af_tgt = 0;
    float pitch_offset = 0.0f, pitch_offset_tgt = 0.0f;
};

// New note: snap straight to the target -- no glide from whatever the
// previous note left behind -- and clear filter memory so the new note
// starts clean.
inline void sam_retrigger(SamVoiceState &sv, const SamAllophoneTarget &t) {
    for (uint32_t i = 0; i < SAM_FORMANTS; i++) {
        sv.F[i] = sv.F_tgt[i] = t.F[i];
        sv.B[i] = sv.B_tgt[i] = t.B[i];
        sv.amp[i] = sv.amp_tgt[i] = t.amp[i];
        res2p_reset(sv.formant[i]);
    }
    sv.fric_F = sv.fric_F_tgt = t.fric_F;
    sv.fric_B = sv.fric_B_tgt = t.fric_B;
    sv.af = sv.af_tgt = t.af;
    res2p_reset(sv.fric);
}

// Allophone changed while the voice is already sounding: move the targets
// only -- sam_advance_subblock() ramps everything toward them.
inline void sam_set_target(SamVoiceState &sv, const SamAllophoneTarget &t) {
    for (uint32_t i = 0; i < SAM_FORMANTS; i++) {
        sv.F_tgt[i] = t.F[i];
        sv.B_tgt[i] = t.B[i];
        sv.amp_tgt[i] = t.amp[i];
    }
    sv.fric_F_tgt = t.fric_F;
    sv.fric_B_tgt = t.fric_B;
    sv.af_tgt = t.af;
}

// Sub-block ramp coefficient for F/B/amp/fric/pitch targets -- short enough
// to settle well within a note, long enough that a mid-note target change
// doesn't click.
inline constexpr float SAM_RAMP_COEFF = 0.25f;

// Ramp every F/B/amp/fric/pitch target one sub-block and recompute each
// resonator's coefficients from the ramped values -- never interpolate a
// resonator's own a1/a2/b0 directly, since walking between two coefficient
// sets can push a pole outside the unit circle.
inline void sam_advance_subblock(SamVoiceState &sv, float fs) {
    for (uint32_t i = 0; i < SAM_FORMANTS; i++) {
        sv.F[i] += (sv.F_tgt[i] - sv.F[i]) * SAM_RAMP_COEFF;
        sv.B[i] += (sv.B_tgt[i] - sv.B[i]) * SAM_RAMP_COEFF;
        sv.amp[i] += (sv.amp_tgt[i] - sv.amp[i]) * SAM_RAMP_COEFF;
        res2p_set(sv.formant[i], sv.F[i], sv.B[i], fs);
    }
    sv.fric_F += (sv.fric_F_tgt - sv.fric_F) * SAM_RAMP_COEFF;
    sv.fric_B += (sv.fric_B_tgt - sv.fric_B) * SAM_RAMP_COEFF;
    sv.af += (sv.af_tgt - sv.af) * SAM_RAMP_COEFF;
    res2p_set(sv.fric, sv.fric_F, sv.fric_B, fs);
    sv.pitch_offset += (sv.pitch_offset_tgt - sv.pitch_offset) * SAM_RAMP_COEFF;
}

// Pitch-contour target (semitones), set independently of the allophone
// target above -- see SamVoiceState's own comment. `sam_snap_pitch()` is a
// new note's straight-to-target snap (mirrors sam_retrigger()'s "no glide
// from the previous note" rule); `sam_set_pitch_target()` is a mid-sequence
// segment change, ramped by sam_advance_subblock() like everything else.
inline void sam_snap_pitch(SamVoiceState &sv, float semitones) { sv.pitch_offset = sv.pitch_offset_tgt = semitones; }
inline void sam_set_pitch_target(SamVoiceState &sv, float semitones) { sv.pitch_offset_tgt = semitones; }

// Sums the three independently-driven formant resonators plus the
// frication branch, each scaled by its own weight -- the parallel topology
// that distinguishes this tract from tract.h's cascade.
inline float sam_process_mixed(SamVoiceState &sv, float voiced_src, float noise_src) {
    float out = 0.0f;
    for (uint32_t i = 0; i < SAM_FORMANTS; i++) out += res2p_tick(sv.formant[i], voiced_src * sv.amp[i]);
    out += res2p_tick(sv.fric, noise_src * sv.af);
    return out;
}

// A SAM phrase: allophone indices (resolved through render.h's
// sam_allophone_target(), same as the phoneme-keyboard path) plus a
// parallel per-segment pitch-contour target (semitones) baked in by the
// reciter (tools/samgen.py) -- S.A.M.'s stress overshoot/undershoot,
// applied the same way tools/nrl_rules.py's own release_index already
// mirrors SpeechUtterance's shape for the formant tract.
struct SamUtterance {
    const uint8_t *allophones;
    const int8_t *pitch_offset;
    uint16_t length;
    uint16_t release_index;
};

// Hardcoded bring-up fixture: no reciter or allophone-table import tool
// exists yet, so this is a small, hand-authored set -- silence, three
// vowels, one fricative -- enough to prove the tract audible end-to-end
// before any data pipeline exists. F1-F3 are Peterson & Barney (1952)
// published adult-male averages, the same reference values the formant
// tract's own phoneme table is checked against; this tract's model just
// carries three formants of that data instead of five. Per-formant
// amplitude weights are a simple decreasing approximation (F1 strongest),
// not measured data.
enum SamAllophone : uint8_t { SAM_AL_SIL, SAM_AL_AA, SAM_AL_IY, SAM_AL_UW, SAM_AL_S, SAM_ALLOPHONE_COUNT };

inline constexpr SamAllophoneTarget SAM_TEST_ALLOPHONES[SAM_ALLOPHONE_COUNT] = {
    // SIL
    { {500, 1500, 2500}, {90, 110, 170}, {0.0f, 0.0f, 0.0f}, 4000, 300, 0.0f, 100 },
    // AA (father)
    { {730, 1090, 2440}, {90, 110, 170}, {1.0f, 0.7f, 0.3f}, 4000, 300, 0.0f, 120 },
    // IY (see)
    { {270, 2290, 3010}, {60, 90, 150}, {1.0f, 0.6f, 0.3f}, 4000, 300, 0.0f, 120 },
    // UW (boot)
    { {300, 870, 2240}, {60, 80, 150}, {1.0f, 0.5f, 0.2f}, 4000, 300, 0.0f, 120 },
    // S (unvoiced fricative)
    { {500, 1500, 2500}, {90, 120, 170}, {0.0f, 0.0f, 0.0f}, 6000, 500, 1.0f, 110 },
};
