#pragma once

#include "phonemes.h"
#include "tract.h"
#include <cstdint>

// Per-voice segment sequencer (module_speech.md "Segment sequencer"): turns
// a phoneme string into a sequence of tract targets, one per segment, each
// held for its own phoneme-derived duration. Pure data/logic, no excitation
// or pan -- render.h's speech_render_voice_seq() is the render loop that
// calls this from inside its per-voice sub-block cut, same split as
// tract.h/excitation.h. No pico-sdk dependency (phonemes.h/tract.h aren't
// either), so this is shared by the device engine and
// tools/host_render/render_speech.cpp, same as every other speech DSP header.

// What note-off means for a voice that is sequencing an utterance
// (module_speech.md "Voice lifetime and note-off"). module_speech.md's own
// sketch lists a fourth SpeechMode value, SPEECH_HOLD, alongside these
// three -- this engine represents that case structurally instead
// (VoiceParams::utterance == SPEECH_NO_UTTERANCE bypasses the sequencer
// entirely and renders through the phoneme-keyboard path,
// speech_render_voice() in render.h) rather than as a mode this enum's
// logic has to branch on, since HOLD has no segments to advance through
// at all.
enum SpeechMode : uint8_t {
    SPEECH_MODE_ONESHOT,  // ignores note-off; completes when the utterance's last segment ends
    SPEECH_MODE_GATED,    // note-off jumps to the utterance's release segment -- the default
    SPEECH_MODE_LOOP,     // repeats from segment 0 while gated; degrades to ONESHOT once gate drops
};

// Sentinel for VoiceParams::utterance (engine.h): this voice is in the
// phoneme-keyboard path, not sequencing one of utterance.h's SPEECH_UTTERANCES.
static constexpr uint8_t SPEECH_NO_UTTERANCE = 0xFF;

// Live `rate` CC range (module_speech.md "CC map"): same tract_cc_to_q8_8()
// shape as formant_shift/bandwidth_scale (tract.h), but the target format is
// VoiceParams::rate's Q4.4 (16 == 1.0x), not Q8.8, since speech_seg_
// duration_samples() above already expects Q4.4. 0.25x..4x covers
// "noticeably slower" to "noticeably faster" without letting a fast phoneme
// round down to a zero-length segment (speech_seg_duration_samples() floors
// at 1 sample regardless, but 4x is already a stress case, not a typical
// performance setting).
inline constexpr float SPEECH_RATE_CC_MIN = 0.25f, SPEECH_RATE_CC_MAX = 4.0f;

inline uint8_t speech_rate_cc_to_q4_4(uint8_t cc) {
    float v = SPEECH_RATE_CC_MIN + (SPEECH_RATE_CC_MAX - SPEECH_RATE_CC_MIN) * (float)cc / 127.0f;
    float q = v * 16.0f + 0.5f;
    if (q < 1.0f) q = 1.0f;      // never 0 -- see speech_seg_duration_samples()'s own floor
    if (q > 255.0f) q = 255.0f;
    return (uint8_t)q;
}

// A hardcoded utterance: a phoneme string plus the segment index note-off
// jumps to under SPEECH_MODE_GATED (defaults to the last segment when a
// caller doesn't pick one explicitly -- see utterance.h). Not generated --
// text-to-phoneme (module_speech.md "Host Tooling") is a separate host-tool
// pipeline; this struct only needs a hardcoded string to be intelligible.
struct SpeechUtterance {
    const uint8_t *phonemes;
    uint16_t length;
    uint16_t release_index;
};

// Segment duration for `phoneme`, scaled by the voice's `rate` (Q4.4, 16 =
// 1.0x -- VoiceParams::rate, engine.h) and rendered at native rate `fs`.
// Despite the field's name, a *larger* rate value means *longer* segments,
// i.e. slower speech -- not "higher = faster" as the name alone would
// suggest. Floored to 1 sample so rate == 0 (or a very short phoneme
// at a very fast rate) can never produce a zero-duration segment, which
// would make the sequencer re-advance every single iteration of the render
// loop below without ever consuming a sample.
inline uint32_t speech_seg_duration_samples(uint8_t phoneme, uint8_t rate_q4_4, float fs) {
    const PhonemeDef &def = PHONEME_TARGETS[phoneme % PHONEME_COUNT];
    float scale = (float)rate_q4_4 * (1.0f / 16.0f);
    float samples = (float)def.duration_ms * 0.001f * fs * scale;
    if (samples < 1.0f) samples = 1.0f;
    return (uint32_t)samples;
}

// Loads segment `idx` of `utt` into `sv`: picks retrigger (new note -- full
// reset, tract_retrigger()) vs. snap (this segment's PHONEME_FLAG_
// TRANSITION_FAST, i.e. a plosive burst -- tract_snap_target()) vs. the
// normal glide (tract_set_target()), and sets seg_index/seg_remaining.
// module_speech.md "Plosives": PHONEME_FLAG_STOP_CLOSURE segments are silent
// (av = af = 0) in phonemes.h's own generated data already, but this
// forces it explicitly too -- belt-and-suspenders against a future CSV row
// that forgets to zero them, since a closure that isn't silent breaks the
// closure/burst contrast plosive intelligibility depends on.
inline void speech_seg_load(SpeechVoice &sv, const SpeechUtterance &utt, uint16_t idx,
                             uint8_t rate_q4_4, float fs, bool retrigger) {
    uint8_t phoneme = utt.phonemes[idx];
    const PhonemeDef &def = PHONEME_TARGETS[phoneme % PHONEME_COUNT];
    FormantTarget t = phoneme_unpack(def);
    if (def.flags & PHONEME_FLAG_STOP_CLOSURE) { t.av = 0.0f; t.af = 0.0f; }

    // sv.fmt, not sv: SpeechVoice's tract-specific state is a union
    // (tract.h) since the LPC lattice tract was added as a sibling; the
    // segment sequencer itself is otherwise unchanged.
    if (retrigger) {
        tract_retrigger(sv.fmt, t);
    } else if (def.flags & PHONEME_FLAG_TRANSITION_FAST) {
        tract_snap_target(sv.fmt, t);
    } else {
        tract_set_target(sv.fmt, t);
    }
    sv.seg_index = idx;
    sv.seg_remaining = speech_seg_duration_samples(phoneme, rate_q4_4, fs);
}

// Advances to the next segment once the current one's seg_remaining reaches
// zero (module_speech.md's per-voice equivalent of the tracker's tick-advance).
// Reaching the end of the utterance: SPEECH_MODE_LOOP starts a new pass if
// still gated, otherwise (LOOP with no gate, ONESHOT, or any other mode)
// marks the utterance complete -- sv.seq_done, which render.h's
// speech_render_voice_seq() uses both to stop rendering (silence, not the
// last segment's stale parameters) and to clear audio_engine.cpp's
// active_mask bit so voice_alloc can reclaim the voice.
inline void speech_sequencer_advance(SpeechVoice &sv, const SpeechUtterance &utt, SpeechMode mode,
                                      bool gate, uint8_t rate_q4_4, float fs) {
    uint16_t next = (uint16_t)(sv.seg_index + 1);
    if (next >= utt.length) {
        if (mode == SPEECH_MODE_LOOP && gate) {
            next = 0;
        } else {
            sv.seq_done = true;
            // Must be nonzero: render.h's while (n < native_frames) loop
            // cuts each iteration at min(frames left, seg_remaining,
            // SUBBLOCK), so a seg_remaining left at its stale 0 (the value
            // that triggered this advance) would cut every remaining
            // iteration to k == 0 and spin forever instead of finishing the
            // buffer. sv.seq_done itself (not this value) is what render.h
            // actually gates rendering-vs-silence on from here.
            sv.seg_remaining = 0xFFFFFFFFu;
            return;
        }
    }
    speech_seg_load(sv, utt, next, rate_q4_4, fs, /*retrigger=*/false);
}
