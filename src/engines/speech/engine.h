#pragma once

#include <cstdint>

// Speech engine (#27): MAX_VOICES=4, defined ahead of engine_base.h per #10.
// A placeholder pending the P2 profiling measurement (speech.md "Performance
// Budget") that may raise it -- the other three engines are untouched.
static constexpr uint32_t MAX_VOICES = 4;

#include "engine_base.h"

// Native render rate, per speech.md "Native rate: 22.05 kHz, ZOH x2" --
// exactly SAMPLE_RATE/2, so the resample step to the shared 44.1 kHz output
// stage is a bare integer doubling, no fractional accumulator.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;

// Speech engine skeleton (#27): proves the fourth build-time engine seam --
// MAX_VOICES=4, the 22.05 kHz native / ZOH x2 resample seam, and delay/
// reverb staying linked (speech has no sample-RAM pressure, unlike the
// tracker) -- before any formant DSP exists. No segment sequencer, no tract
// filter, no phoneme data yet (speech.md's Phased Plan, P1+). VoiceParams
// here only carries enough to drive a fixed test tone through the standard
// ParamExchange/voice_alloc path.
//
// Settled in speech.md and deliberately NOT mirrored from the tracker: the
// tracker's ordered TickBlock ring is not adopted here. Polyphonic speech
// has N independent per-voice segment clocks, not one global tick clock, so
// this engine keeps the plain latest-wins ParamExchange/voice_alloc that the
// subtractive and groovebox engines also use.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point (Q32) glottal phase increment, at SPEECH_RATE
    int16_t  amplitude;  // 0-32767
    uint8_t  trigger;    // generation counter, incremented on each note-on
    bool     gate;       // true while voice should sound
    int16_t  pan;        // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    uint8_t  phoneme;    // Vowel index (phonemes.h) -- SPEECH_HOLD phoneme keyboard, #28
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
