#pragma once

#include "excitation.h"
#include "osc/noise.h"
#include "osc/sine.h"
#include "pan.h"
#include "phonemes.h"
#include "sequencer.h"
#include "tract.h"
#include <cstdint>

// Shared core of the speech engine's test-tone bring-up path: renders `native_frames`
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
// (module_speech.md "Timing Domains": "Sub-block <=64 frames (1.45 ms)"). Same
// value as the tracker's TRACKER_SUBBLOCK, for the same reason -- a unit of
// parameter constancy, not of output.
static constexpr uint32_t SPEECH_SUBBLOCK = 64;

// Headroom applied to both excitation sources (glottal pulse and LFSR
// noise) before they hit the tract: each res2p stage has unity DC
// gain, but a narrow-bandwidth resonator has real gain at its own resonant
// frequency (roughly 1/(1-r)), and five of them in series (the cascade)
// compounds that. Without this the cascade clips well before `amplitude`
// reaches its nominal full-scale value. Tuned empirically in
// tools/host_render/render_speech.cpp against every phoneme in phonemes.h,
// not just the five vowels -- the fricative/nasal branches share this
// headroom rather than getting their own (checked peak stays well under
// full scale for all twelve, run_fricative_checks()/run_voiced_fricative_
// checks()/run_nasal_checks()'s clip checks).
static constexpr float SPEECH_EXCITATION_HEADROOM = 1.0f / 12.0f;

// Renders `native_frames` samples of one voice's full tract output (cascade
// plus parallel fricative/nasal branches, mixed excitation) at the
// engine's native rate, panned and zero-order-held x2 into the
// (output-rate-sized) dry_l/dry_r accumulators -- callers must clear those
// buffers themselves and may call this once per active voice, since it
// accumulates (+=) rather than overwrites. `sv` is this voice's persistent
// per-voice render state (Res2p filters, ramped F/B, glottal phase); `fs` is
// the native rate in Hz (passed rather than baked in, same reasoning as
// speech_render_test_tone's `phase_inc`). `trigger` is VoiceParams::trigger
// -- a change vs. `sv.last_trigger` means a new note, so the tract snaps
// straight to the new phoneme's target and clears filter/phase state
// (tract_retrigger()) instead of gliding from whatever the previous note
// left behind. `formant_shift`/`bandwidth_scale` are raw Q8.8 (256 = 1.0x,
// see tract.h's tract_cc_to_q8_8()) live parameters -- updated every call
// regardless of trigger/phoneme, so a CC sweep reaches a held note.
// `jitter`/`shimmer` are raw 0-255 (VoiceParams, excitation.h); `lfo_rate`
// (Hz)/`lfo_depth` (0-1) are the vibrato pair, sampled once per sub-block
// (excitation.h's glottal_vibrato_advance/_inc) -- see speech_render_voice_
// seq() below for the shared reasoning, identical here since both paths
// drive the same glottal excitation.
inline void speech_render_voice(SpeechVoice &sv, uint32_t phase_inc, float fs, uint8_t trigger,
                                 int16_t amplitude, bool gate, uint8_t phoneme, int16_t pan,
                                 int16_t formant_shift, int16_t bandwidth_scale,
                                 uint8_t jitter, uint8_t shimmer, float lfo_rate, float lfo_depth,
                                 int32_t *dry_l, int32_t *dry_r, uint32_t native_frames) {
    sv.formant_shift_tgt = (float)formant_shift * (1.0f / 256.0f);
    sv.bandwidth_scale_tgt = (float)bandwidth_scale * (1.0f / 256.0f);
    // Unpacked only on an actual trigger/phoneme change -- PHONEME_TARGETS
    // holds the packed, flash-resident PhonemeDef, and phoneme_unpack() is the
    // one place that expands it back to a FormantTarget, so that cost is paid
    // on those two transitions only, not every buffer.
    if (trigger != sv.last_trigger) {
        sv.last_trigger = trigger;
        sv.last_phoneme = phoneme;
        tract_retrigger(sv, phoneme_unpack(PHONEME_TARGETS[phoneme % PHONEME_COUNT]));
        sv.glot_cycle_inc = phase_inc;  // seed the first cycle before any wrap has fired
    } else if (phoneme != sv.last_phoneme) {
        sv.last_phoneme = phoneme;
        tract_set_target(sv, phoneme_unpack(PHONEME_TARGETS[phoneme % PHONEME_COUNT]));
    }

    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);

    // Amplitude declick time constant: fast enough (~100 samples, ~4.5 ms at
    // SPEECH_RATE) that gate on/off doesn't thump, slow enough to stay well
    // under the tract's own ~12-15 ms glide so it never sounds like part of
    // the formant transition. Shared by both excitation sources so av/af
    // set the *balance* between them and gate/velocity scale both equally.
    constexpr float AMP_SMOOTH_COEFF = 0.01f;

    uint32_t n = 0;
    while (n < native_frames) {
        uint32_t k = native_frames - n;
        if (k > SPEECH_SUBBLOCK) k = SPEECH_SUBBLOCK;

        tract_advance_subblock(sv, fs);
        float amp_tgt = (gate ? (float)amplitude : 0.0f) * SPEECH_EXCITATION_HEADROOM;

        // Vibrato is resampled once per sub-block (not per sample, not
        // per glottal cycle) -- `base_inc` is this sub-block's nominal
        // (vibrato-applied) phase increment; jitter perturbs it further,
        // once per glottal cycle, inside the sample loop below.
        uint32_t base_inc = phase_inc;
        if (lfo_rate > 0.0f && lfo_depth > 0.0f) {
            float lfo_val = glottal_vibrato_advance(sv.lfo_phase, lfo_rate, k, fs);
            base_inc = glottal_vibrato_inc(phase_inc, lfo_val, lfo_depth);
        }

        for (uint32_t i = 0; i < k; i++) {
            sv.cur_amp += (amp_tgt - sv.cur_amp) * AMP_SMOOTH_COEFF;

            float voiced_src = glottal_pulse(sv.glottal_phase) * sv.av * sv.cur_amp * sv.glot_cycle_amp;
            float noise_f = (float)osc_noise(sv.noise_lfsr) * (1.0f / 32768.0f);
            float noise_src = noise_f * sv.af * sv.cur_amp;

            uint32_t prev_phase = sv.glottal_phase;
            sv.glottal_phase += sv.glot_cycle_inc;
            if (sv.glottal_phase < prev_phase) {  // wrapped: new glottal cycle starts next sample
                sv.glot_cycle_inc = glottal_jitter_inc(base_inc, jitter, sv.jitter_lfsr);
                sv.glot_cycle_amp = glottal_shimmer_mult(shimmer, sv.jitter_lfsr);
            }

            int32_t sample = (int32_t)tract_process_mixed(sv, voiced_src, noise_src);
            int32_t l = (sample * gain_l) >> 15;
            int32_t r = (sample * gain_r) >> 15;
            uint32_t o = (n + i) * 2;
            dry_l[o] += l; dry_l[o + 1] += l;
            dry_r[o] += r; dry_r[o + 1] += r;
        }
        n += k;
    }
}

// Sequenced render (module_speech.md "Segment sequencer"). Unlike
// speech_render_voice() above (the SPEECH_HOLD phoneme keyboard -- one
// note, one sustained phoneme, no sequencer at all), this steps the voice
// through SPEECH_UTTERANCES[utterance_id]'s phoneme string one segment at a
// time, with the sub-block cut point moved *inside* the per-voice loop
// (module_speech.md "Sub-block cut point moves inside the voice loop"): k =
// min(frames left in this call, samples left in the current segment,
// SPEECH_SUBBLOCK) -- not just min(frames left, SPEECH_SUBBLOCK), the way
// the HOLD path above cuts, since a sequenced voice's segment boundary can
// fall anywhere inside a sub-block or a buffer. `utt` is the utterance being
// spoken -- callers resolve it (utterance.h's SPEECH_UTTERANCES, indexed by
// VoiceParams::utterance) rather than this function reaching into that
// fixture table itself, same reasoning render.h already gives for taking
// `phase_inc`/`fs` as parameters instead of baking in a specific source.
// `mode`/`rate` are VoiceParams::mode/rate straight through (engine.h).
// `jitter`/`shimmer`/`lfo_rate`/`lfo_depth` (module_speech.md "Vibrato
// LFO"/"Jitter and shimmer"): applied to the glottal excitation exactly like
// speech_render_voice() above, independent of the sequencer -- a phoneme
// boundary changes tract targets, not excitation character, so jitter/
// shimmer/vibrato run continuously across segment boundaries within one
// utterance rather than resetting per segment.
inline void speech_render_voice_seq(SpeechVoice &sv, uint32_t phase_inc, float fs, uint8_t trigger,
                                     int16_t amplitude, bool gate, const SpeechUtterance &utt, SpeechMode mode,
                                     uint8_t rate, int16_t pan, int16_t formant_shift, int16_t bandwidth_scale,
                                     uint8_t jitter, uint8_t shimmer, float lfo_rate, float lfo_depth,
                                     int32_t *dry_l, int32_t *dry_r, uint32_t native_frames) {
    sv.formant_shift_tgt = (float)formant_shift * (1.0f / 256.0f);
    sv.bandwidth_scale_tgt = (float)bandwidth_scale * (1.0f / 256.0f);

    // module_speech.md "Underrun policy", extended to malformed sequencer data: an
    // empty/null utterance renders silence rather than dereferencing
    // utt.phonemes[0] below -- the acceptance criterion "any inconsistency
    // renders silence", verified by tools/host_render/render_speech.cpp
    // deliberately constructing one.
    bool malformed = (utt.length == 0 || utt.phonemes == nullptr);

    if (trigger != sv.last_trigger) {
        sv.last_trigger = trigger;
        sv.gate_prev = gate;
        sv.seq_done = malformed;
        if (!malformed) {
            speech_seg_load(sv, utt, 0, rate, fs, /*retrigger=*/true);
        } else {
            tract_retrigger(sv, phoneme_unpack(PHONEME_TARGETS[PH_SIL]));
            sv.seg_remaining = 0xFFFFFFFFu;  // see speech_sequencer_advance()'s comment on why
        }
        sv.glot_cycle_inc = phase_inc;  // seed the first cycle before any wrap has fired
    } else if (!malformed && !sv.seq_done) {
        // Note-off edge, checked once per call rather than per-sample --
        // SPEECH_MODE_GATED's jump to the release segment. ONESHOT/LOOP
        // have no extra work to do here; their note-off handling lives in
        // speech_sequencer_advance()'s end-of-utterance branch instead.
        if (sv.gate_prev && !gate && mode == SPEECH_MODE_GATED && sv.seg_index < utt.release_index) {
            speech_seg_load(sv, utt, utt.release_index, rate, fs, /*retrigger=*/false);
        }
        sv.gate_prev = gate;
    }
    sv.active = !sv.seq_done;

    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);
    constexpr float AMP_SMOOTH_COEFF = 0.01f;

    uint32_t n = 0;
    while (n < native_frames) {
        if (!malformed && !sv.seq_done && sv.seg_remaining == 0) {
            speech_sequencer_advance(sv, utt, mode, gate, rate, fs);
            sv.active = !sv.seq_done;
        }
        uint32_t k = native_frames - n;
        if (k > sv.seg_remaining) k = sv.seg_remaining;
        if (k > SPEECH_SUBBLOCK) k = SPEECH_SUBBLOCK;

        tract_advance_subblock(sv, fs);
        // Done/malformed voices render true silence (amp_tgt 0, declicked
        // through cur_amp same as a normal gate release) rather than being
        // special-cased out of the loop -- they still need dry_l/dry_r
        // filled for every frame of this call.
        float amp_tgt = (gate && !sv.seq_done ? (float)amplitude : 0.0f) * SPEECH_EXCITATION_HEADROOM;

        // Vibrato resampled once per sub-block, same as
        // speech_render_voice() above.
        uint32_t base_inc = phase_inc;
        if (lfo_rate > 0.0f && lfo_depth > 0.0f) {
            float lfo_val = glottal_vibrato_advance(sv.lfo_phase, lfo_rate, k, fs);
            base_inc = glottal_vibrato_inc(phase_inc, lfo_val, lfo_depth);
        }

        for (uint32_t i = 0; i < k; i++) {
            sv.cur_amp += (amp_tgt - sv.cur_amp) * AMP_SMOOTH_COEFF;

            float voiced_src = glottal_pulse(sv.glottal_phase) * sv.av * sv.cur_amp * sv.glot_cycle_amp;
            float noise_f = (float)osc_noise(sv.noise_lfsr) * (1.0f / 32768.0f);
            float noise_src = noise_f * sv.af * sv.cur_amp;

            uint32_t prev_phase = sv.glottal_phase;
            sv.glottal_phase += sv.glot_cycle_inc;
            if (sv.glottal_phase < prev_phase) {  // wrapped: new glottal cycle starts next sample
                sv.glot_cycle_inc = glottal_jitter_inc(base_inc, jitter, sv.jitter_lfsr);
                sv.glot_cycle_amp = glottal_shimmer_mult(shimmer, sv.jitter_lfsr);
            }

            int32_t sample = (int32_t)tract_process_mixed(sv, voiced_src, noise_src);
            int32_t l = (sample * gain_l) >> 15;
            int32_t r = (sample * gain_r) >> 15;
            uint32_t o = (n + i) * 2;
            dry_l[o] += l; dry_l[o + 1] += l;
            dry_r[o] += r; dry_r[o + 1] += r;
        }
        n += k;
        if (!sv.seq_done) sv.seg_remaining -= k;
    }
}
