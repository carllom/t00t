#pragma once

#include "phonemes.h"
#include "sequencer.h"

// Hardcoded test utterances (module_speech.md "Segment sequencer" exit
// criterion: "a hardcoded phoneme string is intelligible as a word"). Not
// generated -- the CSV/host-tool phrase pipeline (phrases.h) produces real
// phrase content separately; these two are hand-picked from phonemes.h's
// 48-phoneme set (tools/speech_phonemes.csv) to exercise specific claims:
// variable per-segment duration, F/B ramping across a phoneme boundary, and
// (CAT) an audible plosive closure/burst pair. Kept in place for
// tools/host_render/render_speech.cpp's regression checks, which need their
// exact known phoneme strings rather than whatever speech_phrases.txt
// currently says.

// "HELLO" ~ /hɛloʊ/, approximated from the available phoneme set -- there is
// no /oʊ/ diphthong target yet (phonemes.h has no OW row), so /o/ stands in
// as the closest cascade shape. Trailing SIL is the release segment
// SPEECH_MODE_GATED's note-off jumps to.
static constexpr uint8_t UTTERANCE_HELLO_PHONEMES[] = {
    PH_HH, PH_E, PH_L, PH_O, PH_SIL,
};

// "CAT" /kæt/. Both consonants carry their full closure/burst pair
// (PHONEME_FLAG_STOP_CLOSURE -> PLOSIVE|TRANSITION_FAST), the actual
// mechanism the closure/burst flags exist to prove: without it, /k/ and /t/ would render as
// bare fricative-like noise and the word collapses into mush.
static constexpr uint8_t UTTERANCE_CAT_PHONEMES[] = {
    PH_K_CL, PH_K_BR, PH_AE, PH_T_CL, PH_T_BR, PH_SIL,
};

enum SpeechUtteranceId : uint8_t {
    UTT_HELLO,
    UTT_CAT,
    SPEECH_UTTERANCE_COUNT,
};

inline constexpr SpeechUtterance SPEECH_UTTERANCES[SPEECH_UTTERANCE_COUNT] = {
    { UTTERANCE_HELLO_PHONEMES, 5, 4 },  // release = trailing SIL
    { UTTERANCE_CAT_PHONEMES,   6, 5 },  // release = trailing SIL
};
