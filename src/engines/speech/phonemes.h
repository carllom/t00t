#pragma once

#include "tract.h"
#include <cstdint>

// Hardcoded formant targets for the five cardinal vowels (#28, speech.md
// P1: "Formant targets ... are hardcoded in this slice. The CSV-driven
// table generator comes later"). This is the hand-authored ancestor of the
// eventual generated phonemes.h (speech.md "Architecture Placement");
// SPEECH_HOLD's MIDI keyboard is the only consumer for now.
//
// F1-F3 are Peterson & Barney (1952) adult-male averages for the closest
// American-English vowel to each cardinal target. F4/F5 and every bandwidth
// are typical cascade-synthesis defaults (in the spirit of Klatt 1980) --
// higher formants and bandwidths vary far less across vowels than F1/F2, so
// one shared set for all five is a reasonable approximation at this
// fidelity target.
enum Vowel : uint8_t { VOWEL_I, VOWEL_E, VOWEL_A, VOWEL_O, VOWEL_U, VOWEL_COUNT };

inline constexpr FormantTarget VOWEL_TARGETS[VOWEL_COUNT] = {
    // /i/ (heed)     F1  F2    F3    F4    F5
    { { 270.0f, 2290.0f, 3010.0f, 3300.0f, 3750.0f }, { 60.0f,  90.0f, 150.0f, 200.0f, 200.0f } },
    // /e/ (~head, ɛ)
    { { 530.0f, 1840.0f, 2480.0f, 3300.0f, 3750.0f }, { 70.0f, 100.0f, 150.0f, 200.0f, 200.0f } },
    // /a/ (~father, ɑ)
    { { 730.0f, 1090.0f, 2440.0f, 3300.0f, 3750.0f }, { 90.0f, 110.0f, 170.0f, 200.0f, 200.0f } },
    // /o/ (~hawed, ɔ)
    { { 570.0f,  840.0f, 2410.0f, 3300.0f, 3750.0f }, { 80.0f, 100.0f, 170.0f, 200.0f, 200.0f } },
    // /u/ (who'd)
    { { 300.0f,  870.0f, 2240.0f, 3300.0f, 3750.0f }, { 60.0f,  80.0f, 150.0f, 200.0f, 200.0f } },
};
