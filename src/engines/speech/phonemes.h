#pragma once

#include "tract.h"
#include <cstdint>

// Hand-authored phoneme targets (#28 P1 vowels, #29 P2 fricatives/nasals).
// This is the ancestor of the eventual CSV-generated phonemes.h
// (speech.md "Architecture Placement"); SPEECH_HOLD's MIDI keyboard is the
// only consumer for now, so there is no duration/flags data yet -- just the
// static cascade/fric/nasal targets and excitation mix a held phoneme needs.
//
// F1-F3 of the five vowels are Peterson & Barney (1952) adult-male averages
// for the closest American-English vowel to each cardinal target. F4/F5 and
// every vowel bandwidth are typical cascade-synthesis defaults (in the
// spirit of Klatt 1980) -- higher formants and bandwidths vary far less
// across vowels than F1/F2, so one shared set for all five is a reasonable
// approximation at this fidelity target. Fricative and nasal figures are
// likewise typical/approximate (Klatt-style defaults for a single-resonator
// simplification, not measurements) -- good enough to make each phoneme
// class distinguishable, not a phonetics reference.
enum Phoneme : uint8_t {
    PH_I, PH_E, PH_A, PH_O, PH_U,   // vowels
    PH_S, PH_SH, PH_F,              // voiceless fricatives
    PH_Z, PH_V,                     // voiced fricatives (mixed excitation)
    PH_M, PH_N,                     // nasals
    PHONEME_COUNT
};

// Neutral, mid-central cascade shape behind a fricative constriction --
// speech.md's reduced Klatt split routes only the voiced component through
// the cascade, so this only matters for the voiced fricatives (PH_Z, PH_V);
// for the voiceless ones av=0 mutes the cascade entirely regardless of F/B.
static constexpr float FRIC_CASCADE_F[SPEECH_FORMANTS] = { 500.0f, 1500.0f, 2500.0f, 3300.0f, 3750.0f };
static constexpr float FRIC_CASCADE_B[SPEECH_FORMANTS] = {  90.0f,  120.0f,  170.0f,  200.0f,  200.0f };

// Nasal murmur cascade shape -- low, heavily-damped formants representing
// the (mostly closed) oral cavity, per place of articulation: labial /m/'s
// F2 locus is low, alveolar /n/'s is higher. The dedicated nasal pole
// (nasal_F/nasal_B below) carries the actual nasal-murmur resonance.
static constexpr float NASAL_B[SPEECH_FORMANTS] = { 130.0f, 100.0f, 160.0f, 200.0f, 200.0f };

inline constexpr FormantTarget PHONEME_TARGETS[PHONEME_COUNT] = {
    // Vowels: F1..F5 / B1..B5, no fricative/nasal branch (af=an=0).
    // /i/ (heed)
    { { 270.0f, 2290.0f, 3010.0f, 3300.0f, 3750.0f }, { 60.0f,  90.0f, 150.0f, 200.0f, 200.0f },
      4000.0f, 300.0f, 250.0f, 300.0f, 1.0f, 0.0f, 0.0f },
    // /e/ (~head, ɛ)
    { { 530.0f, 1840.0f, 2480.0f, 3300.0f, 3750.0f }, { 70.0f, 100.0f, 150.0f, 200.0f, 200.0f },
      4000.0f, 300.0f, 250.0f, 300.0f, 1.0f, 0.0f, 0.0f },
    // /a/ (~father, ɑ)
    { { 730.0f, 1090.0f, 2440.0f, 3300.0f, 3750.0f }, { 90.0f, 110.0f, 170.0f, 200.0f, 200.0f },
      4000.0f, 300.0f, 250.0f, 300.0f, 1.0f, 0.0f, 0.0f },
    // /o/ (~hawed, ɔ)
    { { 570.0f,  840.0f, 2410.0f, 3300.0f, 3750.0f }, { 80.0f, 100.0f, 170.0f, 200.0f, 200.0f },
      4000.0f, 300.0f, 250.0f, 300.0f, 1.0f, 0.0f, 0.0f },
    // /u/ (who'd)
    { { 300.0f,  870.0f, 2240.0f, 3300.0f, 3750.0f }, { 60.0f,  80.0f, 150.0f, 200.0f, 200.0f },
      4000.0f, 300.0f, 250.0f, 300.0f, 1.0f, 0.0f, 0.0f },

    // Voiceless fricatives: av=0 (cascade muted), af drives the parallel
    // fricative resonator only. Distinguished from each other by fric_F --
    // /s/ bright/high, /sh/ lower, /f/ diffuse/weak (lower af).
    // /s/
    { { FRIC_CASCADE_F[0], FRIC_CASCADE_F[1], FRIC_CASCADE_F[2], FRIC_CASCADE_F[3], FRIC_CASCADE_F[4] },
      { FRIC_CASCADE_B[0], FRIC_CASCADE_B[1], FRIC_CASCADE_B[2], FRIC_CASCADE_B[3], FRIC_CASCADE_B[4] },
      6000.0f, 500.0f, 250.0f, 300.0f, 0.0f, 1.0f, 0.0f },
    // /sh/ (ʃ)
    { { FRIC_CASCADE_F[0], FRIC_CASCADE_F[1], FRIC_CASCADE_F[2], FRIC_CASCADE_F[3], FRIC_CASCADE_F[4] },
      { FRIC_CASCADE_B[0], FRIC_CASCADE_B[1], FRIC_CASCADE_B[2], FRIC_CASCADE_B[3], FRIC_CASCADE_B[4] },
      2700.0f, 400.0f, 250.0f, 300.0f, 0.0f, 0.8f, 0.0f },
    // /f/
    { { FRIC_CASCADE_F[0], FRIC_CASCADE_F[1], FRIC_CASCADE_F[2], FRIC_CASCADE_F[3], FRIC_CASCADE_F[4] },
      { FRIC_CASCADE_B[0], FRIC_CASCADE_B[1], FRIC_CASCADE_B[2], FRIC_CASCADE_B[3], FRIC_CASCADE_B[4] },
      7500.0f, 600.0f, 250.0f, 300.0f, 0.0f, 0.6f, 0.0f },

    // Voiced fricatives: av and af both nonzero (speech.md "mixed
    // excitation"). Same fric_F targets as their voiceless counterparts
    // (/z/ ~ /s/, /v/ ~ /f/), plus a reduced voiced cascade component.
    // /z/
    { { FRIC_CASCADE_F[0], FRIC_CASCADE_F[1], FRIC_CASCADE_F[2], FRIC_CASCADE_F[3], FRIC_CASCADE_F[4] },
      { FRIC_CASCADE_B[0], FRIC_CASCADE_B[1], FRIC_CASCADE_B[2], FRIC_CASCADE_B[3], FRIC_CASCADE_B[4] },
      6000.0f, 500.0f, 250.0f, 300.0f, 0.5f, 0.6f, 0.0f },
    // /v/
    { { FRIC_CASCADE_F[0], FRIC_CASCADE_F[1], FRIC_CASCADE_F[2], FRIC_CASCADE_F[3], FRIC_CASCADE_F[4] },
      { FRIC_CASCADE_B[0], FRIC_CASCADE_B[1], FRIC_CASCADE_B[2], FRIC_CASCADE_B[3], FRIC_CASCADE_B[4] },
      7500.0f, 600.0f, 250.0f, 300.0f, 0.5f, 0.4f, 0.0f },

    // Nasals: fully voiced (av=1), no frication (af=0), nasal pole active
    // (an). Distinguished from each other by cascade F2 (place of
    // articulation) the same way the vowels are.
    // /m/ (bilabial, low F2 locus)
    { { 300.0f, 1000.0f, 2200.0f, 3300.0f, 3750.0f },
      { NASAL_B[0], NASAL_B[1], NASAL_B[2], NASAL_B[3], NASAL_B[4] },
      4000.0f, 300.0f, 250.0f, 100.0f, 1.0f, 0.0f, 0.7f },
    // /n/ (alveolar, higher F2 locus)
    { { 300.0f, 1600.0f, 2600.0f, 3300.0f, 3750.0f },
      { NASAL_B[0], NASAL_B[1], NASAL_B[2], NASAL_B[3], NASAL_B[4] },
      4000.0f, 300.0f, 250.0f, 100.0f, 1.0f, 0.0f, 0.7f },
};
