#pragma once

#include "excitation.h"
#include "lattice.h"
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
// it are identical on both.
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
// (module_speech.md "Timing Domains") -- a unit of parameter constancy, not
// of output.
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
    bool retriggering = (trigger != sv.last_trigger);
    // A tract switch always lands on a retrigger (a new note-on) -- placement-
    // construct `fmt` fresh before writing into it, so a voice that last held
    // an LPC word never reads that word's leftover state through the union.
    if (retriggering && sv.tract != SPEECH_TRACT_FORMANT) new (&sv.fmt) FormantVoiceState();
    sv.tract = SPEECH_TRACT_FORMANT;

    sv.fmt.formant_shift_tgt = (float)formant_shift * (1.0f / 256.0f);
    sv.fmt.bandwidth_scale_tgt = (float)bandwidth_scale * (1.0f / 256.0f);
    // Unpacked only on an actual trigger/phoneme change -- PHONEME_TARGETS
    // holds the packed, flash-resident PhonemeDef, and phoneme_unpack() is the
    // one place that expands it back to a FormantTarget, so that cost is paid
    // on those two transitions only, not every buffer.
    if (retriggering) {
        sv.last_trigger = trigger;
        sv.last_phoneme = phoneme;
        tract_retrigger(sv.fmt, phoneme_unpack(PHONEME_TARGETS[phoneme % PHONEME_COUNT]));
        sv.glottal_phase = 0;
        sv.cur_amp = 0.0f;
        sv.lfo_phase = 0.0f;
        sv.glot_cycle_amp = 1.0f;
        sv.glot_cycle_inc = phase_inc;  // seed the first cycle before any wrap has fired
    } else if (phoneme != sv.last_phoneme) {
        sv.last_phoneme = phoneme;
        tract_set_target(sv.fmt, phoneme_unpack(PHONEME_TARGETS[phoneme % PHONEME_COUNT]));
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

        tract_advance_subblock(sv.fmt, fs);
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

            float voiced_src = glottal_pulse(sv.glottal_phase) * sv.fmt.av * sv.cur_amp * sv.glot_cycle_amp;
            float noise_f = (float)osc_noise(sv.noise_lfsr) * (1.0f / 32768.0f);
            float noise_src = noise_f * sv.fmt.af * sv.cur_amp;

            uint32_t prev_phase = sv.glottal_phase;
            sv.glottal_phase += sv.glot_cycle_inc;
            if (sv.glottal_phase < prev_phase) {  // wrapped: new glottal cycle starts next sample
                sv.glot_cycle_inc = glottal_jitter_inc(base_inc, jitter, sv.jitter_lfsr);
                sv.glot_cycle_amp = glottal_shimmer_mult(shimmer, sv.jitter_lfsr);
            }

            int32_t sample = (int32_t)tract_process_mixed(sv.fmt, voiced_src, noise_src);
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
    // module_speech.md "Underrun policy", extended to malformed sequencer
    // data: an empty/null utterance renders silence rather than
    // dereferencing utt.phonemes[0] below, verified by
    // tools/host_render/render_speech.cpp deliberately constructing one.
    bool malformed = (utt.length == 0 || utt.phonemes == nullptr);

    bool retriggering = (trigger != sv.last_trigger);
    // A tract switch always lands on a retrigger (a new note-on) -- see
    // speech_render_voice()'s matching comment above.
    if (retriggering && sv.tract != SPEECH_TRACT_FORMANT) new (&sv.fmt) FormantVoiceState();
    sv.tract = SPEECH_TRACT_FORMANT;

    sv.fmt.formant_shift_tgt = (float)formant_shift * (1.0f / 256.0f);
    sv.fmt.bandwidth_scale_tgt = (float)bandwidth_scale * (1.0f / 256.0f);

    if (retriggering) {
        sv.last_trigger = trigger;
        sv.gate_prev = gate;
        sv.seq_done = malformed;
        sv.glottal_phase = 0;
        sv.cur_amp = 0.0f;
        sv.lfo_phase = 0.0f;
        sv.glot_cycle_amp = 1.0f;
        if (!malformed) {
            speech_seg_load(sv, utt, 0, rate, fs, /*retrigger=*/true);
        } else {
            tract_retrigger(sv.fmt, phoneme_unpack(PHONEME_TARGETS[PH_SIL]));
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

        tract_advance_subblock(sv.fmt, fs);
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

            float voiced_src = glottal_pulse(sv.glottal_phase) * sv.fmt.av * sv.cur_amp * sv.glot_cycle_amp;
            float noise_f = (float)osc_noise(sv.noise_lfsr) * (1.0f / 32768.0f);
            float noise_src = noise_f * sv.fmt.af * sv.cur_amp;

            uint32_t prev_phase = sv.glottal_phase;
            sv.glottal_phase += sv.glot_cycle_inc;
            if (sv.glottal_phase < prev_phase) {  // wrapped: new glottal cycle starts next sample
                sv.glot_cycle_inc = glottal_jitter_inc(base_inc, jitter, sv.jitter_lfsr);
                sv.glot_cycle_amp = glottal_shimmer_mult(shimmer, sv.jitter_lfsr);
            }

            int32_t sample = (int32_t)tract_process_mixed(sv.fmt, voiced_src, noise_src);
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

// LPC-lattice render: one voice stepping through a fixed LatticeWord's
// coefficient frames (lattice.h), reusing this voice's shared glottal-pulse/
// LFSR-noise excitation exactly like speech_render_voice_seq() above, but
// driving lattice.h's all-pole synth instead of tract.h's formant cascade.
// Unlike every other render function in this file, `output_frames` counts
// samples at the *output* rate directly -- there's no native-frames-then-
// ZOH-x2 step here, since SPEECH_LATTICE_RATE/`out_fs` has no exact-integer
// shortcut. The native 8 kHz render is pulled through a linear-
// interpolation upsampler on demand, one native sample at a time whenever
// the accumulated fractional position (`sv.lat.resample_frac`, per-voice
// state) crosses 1.0 -- carried across calls, so a buffer boundary never
// re-zeroes it into a phase glitch.
// `mode`/`pitch_shift` are VoiceParams::mode/lattice_pitch_shift straight
// through (engine.h): `mode` gives this tract the same GATED/ONESHOT/LOOP
// note-off contract speech_render_voice_seq() already has (a GATED note-off
// jumps to the word's own final frame -- always the corpus decoder's
// trailing silent STOP frame -- instead of cutting mid-glide; LOOP restarts
// at frame 0 while still gated); `pitch_shift` is a raw Q8.8 multiplier on
// top of each frame's own recorded pitch_hz. `chirp_exciter` selects both
// the voiced and unvoiced source together (paired under one flag for now,
// not independently switchable): false keeps excitation.h's
// glottal_pulse()/osc_noise() (shared with the formant tract), true
// switches to lattice.h's lattice_chirp_pulse()/lattice_chip_noise() (the
// real TMS5220's own excitation table and noise generator) -- see
// lattice.h's own header comments for why that's a real, not cosmetic,
// difference in character.
inline void speech_render_voice_lattice(SpeechVoice &sv, uint8_t trigger, int16_t amplitude, bool gate,
                                         const LatticeWord &word, SpeechMode mode, int16_t pitch_shift,
                                         bool chirp_exciter, int16_t pan, float out_fs,
                                         int32_t *dry_l, int32_t *dry_r, uint32_t output_frames) {
    bool malformed = (word.length == 0 || word.frames == nullptr);
    float pitch_mult = (float)pitch_shift * (1.0f / 256.0f);

    bool retriggering = (trigger != sv.last_trigger);
    // A tract switch always lands on a retrigger -- see speech_render_voice()'s
    // matching comment above.
    if (retriggering && sv.tract != SPEECH_TRACT_LATTICE) new (&sv.lat) LatticeVoiceState();
    sv.tract = SPEECH_TRACT_LATTICE;

    if (retriggering) {
        sv.last_trigger = trigger;
        sv.gate_prev = gate;
        sv.glottal_phase = 0;
        sv.cur_amp = 0.0f;
        lattice_reset(sv.lat);
        sv.lat.word_done = malformed;
        if (!malformed) lattice_load_frame(sv.lat, word, 0, /*retrigger=*/true, pitch_mult);
        else sv.lat.frame_remaining = 0xFFFFFFFFu;  // see speech_sequencer_advance()'s comment on why
    } else if (!malformed && !sv.lat.word_done) {
        // Note-off edge (mirrors speech_render_voice_seq()'s own check
        // above): SPEECH_MODE_GATED jumps straight to the word's last frame
        // rather than cutting mid-glide. ONESHOT/LOOP have no extra work to
        // do here; LOOP's own note-off handling lives in the frame-advance
        // branch below.
        if (sv.gate_prev && !gate && mode == SPEECH_MODE_GATED && sv.lat.frame_index < word.length - 1) {
            lattice_load_frame(sv.lat, word, (uint16_t)(word.length - 1), /*retrigger=*/false, pitch_mult);
        }
        sv.gate_prev = gate;
    }
    sv.active = !sv.lat.word_done;

    int32_t gain_l, gain_r;
    pan_gains_q15(pan, gain_l, gain_r);
    constexpr float AMP_SMOOTH_COEFF = 0.01f;
    const float native_step = (float)SPEECH_LATTICE_RATE / out_fs;  // < 1: upsampling

    for (uint32_t i = 0; i < output_frames; i++) {
        sv.lat.resample_frac += native_step;
        while (sv.lat.resample_frac >= 1.0f) {
            sv.lat.resample_frac -= 1.0f;
            sv.lat.y_prev = sv.lat.y_cur;

            if (!malformed && !sv.lat.word_done && sv.lat.frame_remaining == 0) {
                lattice_advance(sv.lat, word, mode == SPEECH_MODE_LOOP && gate, pitch_mult);
                sv.active = !sv.lat.word_done;
            }
            // Frame length divides evenly into the sub-block size (lattice.h's
            // static_assert), so this fires exactly SPEECH_LATTICE_INTERP_STEPS
            // times per frame -- once right after a fresh load, then every
            // SPEECH_LATTICE_SUBBLOCK native samples after that.
            if ((sv.lat.frame_remaining % SPEECH_LATTICE_SUBBLOCK) == 0) {
                lattice_advance_subblock(sv.lat);
            }

            float amp_tgt = (gate && !sv.lat.word_done ? (float)amplitude : 0.0f) * SPEECH_EXCITATION_HEADROOM;
            sv.cur_amp += (amp_tgt - sv.cur_amp) * AMP_SMOOTH_COEFF;

            // Done voices still tick the filter with zero excitation each
            // sample (a natural ring-down) rather than being cut out of the
            // loop -- dry_l/dry_r still need every output frame filled.
            float exc = 0.0f;
            if (!sv.lat.word_done) {
                float g = sv.lat.gain * sv.cur_amp * SPEECH_LATTICE_GAIN_BOOST;
                if (chirp_exciter) g *= SPEECH_LATTICE_CHIRP_GAIN_SCALE;
                if (sv.lat.voiced) {
                    float pulse = chirp_exciter ? lattice_chirp_pulse(sv.lat.chirp_idx) : glottal_pulse(sv.glottal_phase);
                    exc = pulse * g;
                    uint32_t prev_phase = sv.glottal_phase;
                    sv.glottal_phase += sv.lat.phase_inc;
                    // chirp_idx tracks native samples since the last pitch-period
                    // wrap regardless of which exciter is selected (lattice.h's
                    // own comment on why) -- detected the same way the formant
                    // path detects a new glottal cycle for jitter/shimmer.
                    if (sv.glottal_phase < prev_phase) sv.lat.chirp_idx = 0;
                    else sv.lat.chirp_idx++;
                } else {
                    // Paired with the voiced source under the same
                    // chirp_exciter flag for now (not independently
                    // switchable yet) -- see lattice.h's own header
                    // comment on lattice_chip_noise() for why it's a real
                    // texture difference, not just a level change.
                    float noise_f = chirp_exciter ? lattice_chip_noise(sv.lat.tms_rng)
                                                   : (float)osc_noise(sv.noise_lfsr) * (1.0f / 32768.0f);
                    exc = noise_f * g;
                }
            }
            sv.lat.y_cur = lattice_tick(sv.lat, exc);

            if (sv.lat.frame_remaining != 0xFFFFFFFFu) sv.lat.frame_remaining--;
        }

        float y = sv.lat.y_prev + (sv.lat.y_cur - sv.lat.y_prev) * sv.lat.resample_frac;
        int32_t sample = (int32_t)y;
        int32_t l = (sample * gain_l) >> 15;
        int32_t r = (sample * gain_r) >> 15;
        dry_l[i] += l;
        dry_r[i] += r;
    }
}
