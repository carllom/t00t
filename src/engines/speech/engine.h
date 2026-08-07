#pragma once

#include <cstdint>

// Speech engine (#27): MAX_VOICES, defined ahead of engine_base.h per #10.
// Raised from the #27 placeholder of 4 to 8 per #31's P2 profiling decision
// (engine.md "Speech Engine P2 Profiling (#31)"): measured ~93.5 cycles/
// frame/voice, flat from 1 to 8 voices, so 8 voices is 22% of Core 1 --
// comfortably inside budget even with reverb's +8% on top -- and unlocks
// the "robot chorus" preset speech.md's Open Questions flagged as the
// payoff if the number landed better than expected. The other three
// engines are untouched.
static constexpr uint32_t MAX_VOICES = 8;

// #31 profiling build (T00T_SPEECH_PROFILE, `make ENGINE=speech
// SPEECH_PROFILE=1`) reused the same MAX_VOICES to give its 8-voice phase a
// real 8th slot -- now redundant since the decision above already set it to
// 8, kept only as the define audio_engine.cpp's alternate render loop is
// still gated on.

#include "engine_base.h"

// Native render rate, per speech.md "Native rate: 22.05 kHz, ZOH x2" --
// exactly SAMPLE_RATE/2, so the resample step to the shared 44.1 kHz output
// stage is a bare integer doubling, no fractional accumulator.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;

// Speech engine skeleton (#27): proves the fourth build-time engine seam --
// MAX_VOICES (4 at #27, raised to 8 by #31 above), the 22.05 kHz native /
// ZOH x2 resample seam, and delay/reverb staying linked (speech has no
// sample-RAM pressure, unlike the tracker) -- before any formant DSP
// exists. No segment sequencer, no tract filter, no phoneme data yet
// (speech.md's Phased Plan, P1+). VoiceParams here only carries enough to
// drive a fixed test tone through the standard ParamExchange/voice_alloc
// path.
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
    uint8_t  phoneme;    // Phoneme index (phonemes.h) -- SPEECH_HOLD phoneme keyboard, #28
    // Live tract parameters (#29, speech.md "formant_shift"/"bandwidth_scale"):
    // Q8.8, 256 = 1.0x (neutral). Latest-wins like the rest of VoiceParams --
    // tract.h ramps them per sub-block so a CC sweep can't zipper.
    int16_t  formant_shift;
    int16_t  bandwidth_scale;
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 256, 256 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
