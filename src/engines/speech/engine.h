#pragma once

#include <cstdint>

#include "sequencer.h"  // SpeechMode, SPEECH_NO_UTTERANCE -- no pico-sdk dependency, safe ahead of engine_base.h

// Speech engine (#27): MAX_VOICES, defined ahead of engine_base.h per #10.
// Raised from the #27 placeholder of 4 to 8 per #31's P2 profiling decision
// (history_speech.md "Speech Engine P2 Profiling (#31)"): measured ~93.5 cycles/
// frame/voice, flat from 1 to 8 voices, so 8 voices is 22% of Core 1 --
// comfortably inside budget even with reverb's +8% on top -- and unlocks
// the "robot chorus" preset module_speech.md's Open Questions flagged as the
// payoff if the number landed better than expected. The other three
// engines are untouched.
static constexpr uint32_t MAX_VOICES = 8;

// #31 profiling build (T00T_SPEECH_PROFILE, `make ENGINE=speech
// SPEECH_PROFILE=1`) reused the same MAX_VOICES to give its 8-voice phase a
// real 8th slot -- now redundant since the decision above already set it to
// 8, kept only as the define audio_engine.cpp's alternate render loop is
// still gated on.
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// Native render rate, per module_speech.md "Native rate: 22.05 kHz, ZOH x2" --
// exactly SAMPLE_RATE/2, so the resample step to the shared 44.1 kHz output
// stage is a bare integer doubling, no fractional accumulator.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;

// Speech engine skeleton (#27): proves the fourth build-time engine seam --
// MAX_VOICES (4 at #27, raised to 8 by #31 above), the 22.05 kHz native /
// ZOH x2 resample seam, and delay/reverb staying linked (speech has no
// sample-RAM pressure, unlike the tracker) -- before any formant DSP
// exists. No segment sequencer, no tract filter, no phoneme data yet
// (module_speech.md's Phased Plan, P1+). VoiceParams here only carries enough to
// drive a fixed test tone through the standard ParamExchange/voice_alloc
// path.
//
// Settled in module_speech.md and deliberately NOT mirrored from the tracker: the
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
    // Live tract parameters (#29, module_speech.md "formant_shift"/"bandwidth_scale"):
    // Q8.8, 256 = 1.0x (neutral). Latest-wins like the rest of VoiceParams --
    // tract.h ramps them per sub-block so a CC sweep can't zipper.
    int16_t  formant_shift;
    int16_t  bandwidth_scale;
    // #34 segment sequencer (module_speech.md P3). `utterance` == SPEECH_NO_UTTERANCE
    // (the default) keeps a voice on the #28 phoneme-keyboard path above
    // (render.h's speech_render_voice(), driven by `phoneme`) entirely
    // unchanged; any other value selects one of utterance.h's
    // SPEECH_UTTERANCES and routes the voice through speech_render_voice_seq()
    // instead (audio_engine.cpp). `mode`/`rate` are read only in that second
    // case -- see sequencer.h for both.
    uint8_t    utterance;
    SpeechMode mode;
    uint8_t    rate;  // Q4.4 segment-duration scale, 16 = 1.0x nominal
    // #36 (module_speech.md P4 "MIDI Mapping"/"Vibrato LFO"/"Jitter and shimmer").
    // All four are live, same push-to-held-voices treatment as
    // formant_shift/bandwidth_scale above -- midi_controller.cpp's CC
    // handlers for CC1/CC24/CC25/CC26/CC76 write straight into a held
    // voice's shadow copy, not just the per-channel default for future
    // notes.
    uint8_t  jitter;      // 0-255, 0 = perfectly periodic pitch period (excitation.h)
    uint8_t  shimmer;     // 0-255, 0 = perfectly periodic amplitude (excitation.h)
    float    lfo_rate;    // vibrato LFO rate, Hz, 0 = off (excitation.h VIBRATO_*)
    float    lfo_depth;   // vibrato depth, 0.0-1.0
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 256, 256, SPEECH_NO_UTTERANCE, SPEECH_MODE_GATED, 16,
             0, 0, 0.0f, 0.0f };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;

// Per-voice formant-space + phoneme telemetry for the display (#37, module_speech.md
// "Display": "current phoneme plus a formant-space plot"). F1/F2 are
// tract.h's SpeechVoice::F[0]/F[1] -- the live, ramped values, not the
// phoneme's static target, so a plotted dot moves continuously as the
// segment sequencer (or a mid-note phoneme change, #28) glides between
// targets. `phoneme` is PHONEME_COUNT when the voice has never sounded (no
// valid index yet). Same "diagnostic, no locking" contract audio_engine.h
// documents for audio_engine_load(): Core 1 writes these once per buffer,
// Core 0's ~10 Hz display_task reads them without synchronization -- a
// torn read for one frame is invisible at that redraw rate.
struct SpeechVoiceUiState {
    uint16_t f1_hz, f2_hz;
    uint8_t  phoneme;
    bool     active;
};

void speech_voice_ui_state(uint32_t voice, SpeechVoiceUiState *out);
