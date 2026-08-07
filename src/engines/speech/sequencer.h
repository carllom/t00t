#pragma once

#include "phonemes.h"
#include "tract.h"
#include <cstdint>

// Per-voice segment sequencer (#34, speech.md P3 "Segment sequencer"): turns
// a phoneme string into a sequence of tract targets, one per segment, each
// held for its own phoneme-derived duration. Pure data/logic, no excitation
// or pan -- render.h's speech_render_voice_seq() is the render loop that
// calls this from inside its per-voice sub-block cut, same split as
// tract.h/excitation.h. No pico-sdk dependency (phonemes.h/tract.h aren't
// either), so this is shared by the device engine and
// tools/host_render/render_speech.cpp, same as every other speech DSP header.

// #30 "Voice lifetime and note-off": what note-off means for a voice that is
// sequencing an utterance. speech.md's own sketch lists a fourth SpeechMode
// value, SPEECH_HOLD, alongside these three -- this engine represents that
// case structurally instead (VoiceParams::utterance == SPEECH_NO_UTTERANCE
// bypasses the sequencer entirely and renders through the #28 phoneme-
// keyboard path, speech_render_voice() in render.h) rather than as a mode
// this enum's logic has to branch on, since HOLD has no segments to advance
// through at all.
enum SpeechMode : uint8_t {
    SPEECH_MODE_ONESHOT,  // ignores note-off; completes when the utterance's last segment ends
    SPEECH_MODE_GATED,    // note-off jumps to the utterance's release segment -- #30 default
    SPEECH_MODE_LOOP,     // repeats from segment 0 while gated; degrades to ONESHOT once gate drops
};

// Sentinel for VoiceParams::utterance (engine.h): this voice is in the #28
// phoneme-keyboard path, not sequencing one of utterance.h's SPEECH_UTTERANCES.
static constexpr uint8_t SPEECH_NO_UTTERANCE = 0xFF;

// A hardcoded utterance: a phoneme string plus the segment index note-off
// jumps to under SPEECH_MODE_GATED (defaults to the last segment when a
// caller doesn't pick one explicitly, per #30 -- see utterance.h). Not
// generated -- text-to-phoneme (speech.md "phrases.h") is P4 host-tool work;
// #34's exit criterion only needs a hardcoded string to be intelligible.
struct SpeechUtterance {
    const uint8_t *phonemes;
    uint16_t length;
    uint16_t release_index;
};

// Segment duration for `phoneme`, scaled by the voice's `rate` (Q4.4, 16 =
// 1.0x -- VoiceParams::rate, engine.h) and rendered at native rate `fs`.
// Despite the field's name, this is speech.md's literal "Q4.4 scale on all
// segment durations, 1.0 = nominal": a *larger* rate value means *longer*
// segments, i.e. slower speech -- not "higher = faster" as the name alone
// would suggest. Floored to 1 sample so rate == 0 (or a very short phoneme
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
// speech.md "Plosives": PHONEME_FLAG_STOP_CLOSURE segments are silent
// (av = af = 0) in phonemes.h's own data already (#32's CSV), but this
// forces it explicitly too -- belt-and-suspenders against a future CSV row
// that forgets to zero them, since a closure that isn't silent breaks the
// closure/burst contrast plosive intelligibility depends on.
inline void speech_seg_load(SpeechVoice &sv, const SpeechUtterance &utt, uint16_t idx,
                             uint8_t rate_q4_4, float fs, bool retrigger) {
    uint8_t phoneme = utt.phonemes[idx];
    const PhonemeDef &def = PHONEME_TARGETS[phoneme % PHONEME_COUNT];
    FormantTarget t = phoneme_unpack(def);
    if (def.flags & PHONEME_FLAG_STOP_CLOSURE) { t.av = 0.0f; t.af = 0.0f; }

    if (retrigger) {
        tract_retrigger(sv, t);
    } else if (def.flags & PHONEME_FLAG_TRANSITION_FAST) {
        tract_snap_target(sv, t);
    } else {
        tract_set_target(sv, t);
    }
    sv.seg_index = idx;
    sv.seg_remaining = speech_seg_duration_samples(phoneme, rate_q4_4, fs);
}

// Advances to the next segment once the current one's seg_remaining reaches
// zero (speech.md's per-voice equivalent of the tracker's tick-advance).
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
