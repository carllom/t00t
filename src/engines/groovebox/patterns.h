#pragma once

#include <cstdint>

// Preset step sequences for the TB-303 (303-only for now). Each pattern is a
// list of 16th-note steps played from the incoming MIDI clock (24 PPQN → 6
// pulses per step). A pattern is selected/toggled by a key on the pattern
// channel; tempo follows the clock.
//
// Format, per step:
//   note  — MIDI note number; 0 = rest
//   flags — SEQ_ACCENT (louder + deeper filter sweep) and/or
//           SEQ_SLIDE  (glide/portamento INTO the next step — 303 convention:
//                       the following note is played legato/tied. A slide out of
//                       a rest is a no-op.)
//
// C2 = 36. Octave 2 = low, 3 = normal, 4 = high.

enum SeqFlags : uint8_t { SEQ_ACCENT = 0x01, SEQ_SLIDE = 0x02 };

struct SeqStep {
    uint8_t note;   // MIDI note; 0 = rest
    uint8_t flags;  // SEQ_ACCENT | SEQ_SLIDE
};

struct SeqPattern {
    uint8_t length;         // number of steps
    const SeqStep *steps;
};

// Pattern 0 (user): D#2 D#2 C#3 D#2  G#2 D#2 D#3 D#2  C#4 D#2 F#3 D#2  A#2 C#4 D#3 G4
// Octave 2 = low, 3 = normal, 4 = high (C2 = 36). All notes capitalized = all accented.
static const SeqStep seq_patt0[16] = {
    {39, SEQ_ACCENT}, {39, SEQ_ACCENT}, {49, SEQ_ACCENT}, {39, SEQ_ACCENT},  // D#2 D#2 C#3 D#2
    {44, SEQ_ACCENT}, {39, SEQ_ACCENT}, {51, SEQ_ACCENT}, {39, SEQ_ACCENT},  // G#2 D#2 D#3 D#2
    {61, SEQ_ACCENT}, {39, SEQ_ACCENT}, {54, SEQ_ACCENT}, {39, SEQ_ACCENT},  // C#4 D#2 F#3 D#2
    {46, SEQ_ACCENT}, {61, SEQ_ACCENT}, {51, SEQ_ACCENT}, {67, SEQ_ACCENT},  // A#2 C#4 D#3 G4
};

// Pattern 1 (user): A3~ b2 B3~ A3  G#3 F#2 b2 -   (8 steps, loops)
// A3 slides down to B2, B3 slides down to A3; step 8 is a rest.
static const SeqStep seq_patt1[8] = {
    {57, SEQ_ACCENT | SEQ_SLIDE}, {47, 0},           {59, SEQ_ACCENT | SEQ_SLIDE}, {57, SEQ_ACCENT},  // A3~ b2 B3~ A3
    {56, SEQ_ACCENT},             {42, SEQ_ACCENT},  {47, 0},                      {0, 0},            // G#3 F#2 b2 -
};

// Pattern 2 (user): g2 a#4 g3~ f4  D#3~ C2 C2~ C3  d#4~ d#2 f4~ G4~  a#2 g2~ c2~ g3~
// Wide octave-jumping line, heavy on slides.
static const SeqStep seq_patt2[16] = {
    {43, 0},                {70, 0},           {55, SEQ_SLIDE},              {65, 0},               // g2 a#4 g3~ f4
    {51, SEQ_ACCENT|SEQ_SLIDE}, {36, SEQ_ACCENT}, {36, SEQ_ACCENT|SEQ_SLIDE}, {48, SEQ_ACCENT},    // D#3~ C2 C2~ C3
    {63, SEQ_SLIDE},        {39, 0},           {65, SEQ_SLIDE},              {67, SEQ_ACCENT|SEQ_SLIDE}, // d#4~ d#2 f4~ G4~
    {46, 0},                {43, SEQ_SLIDE},   {36, SEQ_SLIDE},              {55, SEQ_SLIDE},       // a#2 g2~ c2~ g3~
};

static const SeqPattern seq_patterns[] = {
    { 16, seq_patt0 },
    {  8, seq_patt1 },
    { 16, seq_patt2 },
};
static constexpr uint8_t SEQ_PATTERN_COUNT = sizeof(seq_patterns) / sizeof(seq_patterns[0]);

// On the pattern channel, this note selects/toggles pattern 0; the next note
// pattern 1, and so on.
static constexpr uint8_t SEQ_PATTERN_BASE_NOTE = 36;
