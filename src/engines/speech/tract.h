#pragma once

#include "res2p.h"
#include <cstdint>

// Formant cascade + parallel fricative/nasal branches (#28 cascade, #29
// parallel branches, speech.md "Cascade vs parallel"): F1->F2->F3->F4->F5
// two-pole resonators in series get relative formant amplitudes right
// automatically from the bandwidths -- what makes vowels sound natural
// without per-formant amplitude data. A single parallel resonator each for
// frication and the nasal pole, mixed into the cascade output, is "the
// Klatt split, reduced" -- covers everything the SC-01 could do without a
// full Klatt implementation. res2p.h is pure math with no pico-sdk
// dependency, so this header is shared by the device engine
// (audio_engine.cpp) and tools/host_render/render_speech.cpp.
inline constexpr uint32_t SPEECH_FORMANTS = 5;

// Target F/B for one phoneme, Hz, plus the parallel fricative/nasal
// resonator targets and the three excitation mix weights (0..1):
//   av - voiced (glottal pulse) amplitude, feeds the cascade and (further
//        scaled by an) the nasal branch
//   af - frication (LFSR noise) amplitude, feeds the fricative branch
//   an - nasal branch mix weight
// Voiced fricatives (/z/, /v/, /ʒ/) are av>0 and af>0 simultaneously
// (speech.md "Mixed excitation").
struct FormantTarget {
    float F[SPEECH_FORMANTS];
    float B[SPEECH_FORMANTS];
    float fric_F, fric_B;
    float nasal_F, nasal_B;
    float av, af, an;
};

// Live formant_shift / bandwidth_scale ranges (speech.md "MIDI Mapping":
// both on a CC by default). Shared by the device MIDI controller
// (midi_controller.cpp) and the host CC-sweep stability test
// (render_speech.cpp) so both use identical ranges -- picking the range in
// one place only.
inline constexpr float FORMANT_SHIFT_MIN = 0.7f, FORMANT_SHIFT_MAX = 1.6f;
inline constexpr float BANDWIDTH_SCALE_MIN = 0.4f, BANDWIDTH_SCALE_MAX = 3.0f;

// Maps a raw 0..127 CC value into [lo, hi] and encodes it Q8.8 (256 = 1.0x).
inline int16_t tract_cc_to_q8_8(uint8_t cc, float lo, float hi) {
    float v = lo + (hi - lo) * (float)cc / 127.0f;
    return (int16_t)(v * 256.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

// Per-voice tract + excitation render state (Core 1 only, never crosses
// ParamExchange). formant[] is the cascade; fric/nasal are the two parallel
// branches (#29). formant_shift/bandwidth_scale are ramped the same way as
// F/B -- never applied directly, so a CC sweep can't zipper.
struct SpeechVoice {
    Res2p    formant[SPEECH_FORMANTS];
    Res2p    fric, nasal;
    float    F[SPEECH_FORMANTS] = {0}, B[SPEECH_FORMANTS] = {0};
    float    F_tgt[SPEECH_FORMANTS] = {0}, B_tgt[SPEECH_FORMANTS] = {0};
    float    fric_F = 0, fric_B = 0, fric_F_tgt = 0, fric_B_tgt = 0;
    float    nasal_F = 0, nasal_B = 0, nasal_F_tgt = 0, nasal_B_tgt = 0;
    float    av = 0, af = 0, an = 0, av_tgt = 0, af_tgt = 0, an_tgt = 0;
    float    formant_shift = 1.0f, formant_shift_tgt = 1.0f;
    float    bandwidth_scale = 1.0f, bandwidth_scale_tgt = 1.0f;
    uint32_t glottal_phase = 0;
    uint16_t noise_lfsr = 0xACE1u;     // groovebox's LFSR seed (osc/noise.h) -- must be nonzero
    float    cur_amp = 0.0f;          // smoothed toward gate target, declicks on/off
    uint8_t  last_trigger = 0xFF;     // forces tract_retrigger() on first render
    uint8_t  last_phoneme = 0xFF;     // forces a target load on first render
    // #36 vibrato/jitter/shimmer state (speech.md P4, excitation.h). Reset
    // on retrigger, not on a plain segment/phoneme target change -- these
    // track the *note*, the same lifetime as glottal_phase above, not the
    // phoneme currently sounding.
    float    lfo_phase = 0.0f;         // vibrato LFO phase, 0..1 (excitation.h)
    uint32_t glot_cycle_inc = 0;       // this glottal cycle's jittered phase increment
    float    glot_cycle_amp = 1.0f;    // this glottal cycle's shimmer amplitude multiplier
    uint16_t jitter_lfsr = 0xF00Du;    // separate seed from noise_lfsr -- must be nonzero
    // #34 segment sequencer state (speech.md P3). Unused by speech_render_voice()
    // (#28's SPEECH_HOLD phoneme keyboard has no segments to sequence); only
    // speech_render_voice_seq() (render.h) touches these.
    uint16_t seg_index = 0;      // position within the current utterance (sequencer.h)
    uint32_t seg_remaining = 0;  // native samples left in the current segment
    bool     seq_done = false;   // utterance complete (#30 note-off) -- voice still allocated but silent
    bool     gate_prev = false;  // previous call's gate, to find the note-off edge once per buffer
    bool     active = false;     // still sounding -- audio_engine.cpp's active_mask reads this for sequenced voices
};

// New note: snap F/B/fric/nasal/av/af/an and the live formant_shift/
// bandwidth_scale (already updated to their current CC target by the
// caller) straight to their targets -- no glide from whatever the previous
// note left behind -- and clear filter/phase state so the new note starts
// clean.
inline void tract_retrigger(SpeechVoice &sv, const FormantTarget &t) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F[i] = sv.F_tgt[i] = t.F[i];
        sv.B[i] = sv.B_tgt[i] = t.B[i];
        res2p_reset(sv.formant[i]);
    }
    sv.fric_F = sv.fric_F_tgt = t.fric_F;
    sv.fric_B = sv.fric_B_tgt = t.fric_B;
    sv.nasal_F = sv.nasal_F_tgt = t.nasal_F;
    sv.nasal_B = sv.nasal_B_tgt = t.nasal_B;
    sv.av = sv.av_tgt = t.av;
    sv.af = sv.af_tgt = t.af;
    sv.an = sv.an_tgt = t.an;
    res2p_reset(sv.fric);
    res2p_reset(sv.nasal);
    sv.formant_shift = sv.formant_shift_tgt;
    sv.bandwidth_scale = sv.bandwidth_scale_tgt;
    sv.glottal_phase = 0;
    sv.cur_amp = 0.0f;
    // #36: vibrato restarts at phase 0 (sine == 0, no pitch jump on
    // trigger, same reasoning the subtractive engine's LFO retrigger uses);
    // shimmer's multiplier resets to unity. glot_cycle_inc is deliberately
    // left to the caller (render.h sets it from the note's actual phase_inc
    // on the same trigger edge) -- tract.h has no phase_inc to seed it with.
    sv.lfo_phase = 0.0f;
    sv.glot_cycle_amp = 1.0f;
}

// Phoneme changed while the voice is already sounding: just move the
// targets: tract_advance_subblock() ramps everything toward them,
// coefficients follow.
inline void tract_set_target(SpeechVoice &sv, const FormantTarget &t) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F_tgt[i] = t.F[i];
        sv.B_tgt[i] = t.B[i];
    }
    sv.fric_F_tgt = t.fric_F;
    sv.fric_B_tgt = t.fric_B;
    sv.nasal_F_tgt = t.nasal_F;
    sv.nasal_B_tgt = t.nasal_B;
    sv.av_tgt = t.av;
    sv.af_tgt = t.af;
    sv.an_tgt = t.an;
}

// Segment-advance snap for PHONEME_FLAG_TRANSITION_FAST segments (plosive
// bursts, #34/speech.md "Plosives"): sets F/B/fric/nasal/av/af/an straight to
// target, no glide -- but unlike tract_retrigger(), does not touch filter
// memory (res2p_reset) or glottal/noise phase, so the snap itself doesn't
// click by discontinuing state the preceding closure segment was still
// holding. A burst's nominal duration (10-15ms, phonemes.h) is on the same
// order as TRACT_RAMP_COEFF's own settle time, so a normal glide would
// barely start before the segment ends -- the transition needs to already be
// complete on the burst's first sample, not partway there.
inline void tract_snap_target(SpeechVoice &sv, const FormantTarget &t) {
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F[i] = sv.F_tgt[i] = t.F[i];
        sv.B[i] = sv.B_tgt[i] = t.B[i];
    }
    sv.fric_F = sv.fric_F_tgt = t.fric_F;
    sv.fric_B = sv.fric_B_tgt = t.fric_B;
    sv.nasal_F = sv.nasal_F_tgt = t.nasal_F;
    sv.nasal_B = sv.nasal_B_tgt = t.nasal_B;
    sv.av = sv.av_tgt = t.av;
    sv.af = sv.af_tgt = t.af;
    sv.an = sv.an_tgt = t.an;
}

// ~4-5 sub-blocks (~12-15 ms at SPEECH_SUBBLOCK/SPEECH_RATE) to settle on a
// new target -- enough to avoid a click on a mid-note phoneme change, short
// enough that P1's static, unchanging target reaches it well within a note.
inline constexpr float TRACT_RAMP_COEFF = 0.25f;

// Safety floor/ceiling applied to every resonator's *final* (post
// formant_shift/bandwidth_scale) f/bw, regardless of phoneme data or how
// extreme the live CC values are: a bandwidth of exactly 0 drives
// res2p_radius() to exactly 1.0 (pole on the unit circle, res2p_set()'s own
// debug assert would fire), and a frequency near/above Nyquist at high
// formant_shift would fold back rather than sound "shifted". Neither the
// phoneme table nor the CC range (FORMANT_SHIFT_MIN/MAX,
// BANDWIDTH_SCALE_MIN/MAX above) should ever reach these, but the floor/
// ceiling is what the P2 acceptance criterion ("a CC sweep must never be
// able to push a pole outside the unit circle") actually rests on.
inline constexpr float TRACT_MIN_BANDWIDTH_HZ = 20.0f;
inline constexpr float TRACT_MAX_FREQ_FRACTION = 0.45f;   // of fs

inline void tract_apply_coeffs(Res2p &r, float f_raw, float b_raw, float shift, float bw_scale, float fs) {
    float f = f_raw * shift;
    float fmax = fs * TRACT_MAX_FREQ_FRACTION;
    if (f > fmax) f = fmax;
    float bw = b_raw * bw_scale;
    if (bw < TRACT_MIN_BANDWIDTH_HZ) bw = TRACT_MIN_BANDWIDTH_HZ;
    res2p_set(r, f, bw, fs);
}

// Ramp every F/B/fric/nasal/av/af/an and formant_shift/bandwidth_scale one
// sub-block toward its target, and recompute every resonator's coefficients
// from the ramped values -- never from the target directly, and never by
// interpolating a1/a2/b0 (speech.md "Resonator and the stability rule").
inline void tract_advance_subblock(SpeechVoice &sv, float fs) {
    sv.formant_shift += (sv.formant_shift_tgt - sv.formant_shift) * TRACT_RAMP_COEFF;
    sv.bandwidth_scale += (sv.bandwidth_scale_tgt - sv.bandwidth_scale) * TRACT_RAMP_COEFF;

    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        sv.F[i] += (sv.F_tgt[i] - sv.F[i]) * TRACT_RAMP_COEFF;
        sv.B[i] += (sv.B_tgt[i] - sv.B[i]) * TRACT_RAMP_COEFF;
        tract_apply_coeffs(sv.formant[i], sv.F[i], sv.B[i], sv.formant_shift, sv.bandwidth_scale, fs);
    }

    sv.fric_F += (sv.fric_F_tgt - sv.fric_F) * TRACT_RAMP_COEFF;
    sv.fric_B += (sv.fric_B_tgt - sv.fric_B) * TRACT_RAMP_COEFF;
    tract_apply_coeffs(sv.fric, sv.fric_F, sv.fric_B, sv.formant_shift, sv.bandwidth_scale, fs);

    sv.nasal_F += (sv.nasal_F_tgt - sv.nasal_F) * TRACT_RAMP_COEFF;
    sv.nasal_B += (sv.nasal_B_tgt - sv.nasal_B) * TRACT_RAMP_COEFF;
    tract_apply_coeffs(sv.nasal, sv.nasal_F, sv.nasal_B, sv.formant_shift, sv.bandwidth_scale, fs);

    sv.av += (sv.av_tgt - sv.av) * TRACT_RAMP_COEFF;
    sv.af += (sv.af_tgt - sv.af) * TRACT_RAMP_COEFF;
    sv.an += (sv.an_tgt - sv.an) * TRACT_RAMP_COEFF;
}

// Cascade only (F1->F2->F3->F4->F5 in series). Exposed separately from
// tract_process_mixed() below so the host harness can measure the cascade's
// transfer function in isolation (tools/host_render/render_speech.cpp's
// impulse-response check, #28).
inline float tract_process(SpeechVoice &sv, float voiced_src) {
    float y = voiced_src;
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) y = res2p_tick(sv.formant[i], y);
    return y;
}

// Full tract (#29): cascade (voiced path) plus the two parallel branches,
// summed at the output. `voiced_src` (glottal pulse * av) drives both the
// cascade and -- further scaled by an, since the nasal cavity is excited by
// the same glottal source -- the nasal pole; `noise_src` (LFSR noise * af)
// drives the fricative resonator. See FormantTarget's comment for why av/af
// land here rather than as a post-branch mix gain.
inline float tract_process_mixed(SpeechVoice &sv, float voiced_src, float noise_src) {
    float cascade_out = tract_process(sv, voiced_src);
    float fric_out = res2p_tick(sv.fric, noise_src);
    float nasal_out = res2p_tick(sv.nasal, voiced_src * sv.an);
    return cascade_out + fric_out + nasal_out;
}
