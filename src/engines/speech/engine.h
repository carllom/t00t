#pragma once

#include <cstdint>

#include "sequencer.h"  // SpeechMode, SPEECH_NO_UTTERANCE -- no pico-sdk dependency, safe ahead of engine_base.h

// Speech engine: MAX_VOICES, defined ahead of engine_base.h, same as every
// other engine sets its own voice count there. 8 voices is affordable
// enough at this engine's per-voice cost to make the "robot chorus" preset
// (presets.h) a real option.
static constexpr uint32_t MAX_VOICES = 8;

// The profiling build (T00T_SPEECH_PROFILE, `make ENGINE=speech
// SPEECH_PROFILE=1`) reuses the same MAX_VOICES so its 8-voice phase has a
// real 8th slot -- kept only as long as audio_engine.cpp's alternate render
// loop is gated on this define.
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// Native render rate (module_speech.md "Native rate"): exactly
// SAMPLE_RATE/2, so the resample step to the shared 44.1 kHz output stage
// is a bare integer doubling, no fractional accumulator.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;

// Per-voice note/render parameters, latest-wins through the standard
// ParamExchange/voice_alloc path (module_speech.md "Control plane").
// `phoneme` drives the SPEECH_HOLD phoneme keyboard (render.h's
// speech_render_voice()) when `utterance == SPEECH_NO_UTTERANCE` (the
// default); any other `utterance` value selects one of
// utterance.h's/phrases.h's SPEECH_UTTERANCES and routes the voice through
// speech_render_voice_seq() instead (audio_engine.cpp), which also reads
// `mode`/`rate` -- see sequencer.h for both.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point (Q32) glottal phase increment, at SPEECH_RATE
    int16_t  amplitude;  // 0-32767
    uint8_t  trigger;    // generation counter, incremented on each note-on
    bool     gate;       // true while voice should sound
    int16_t  pan;        // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    uint8_t  phoneme;    // Phoneme index (phonemes.h) -- SPEECH_HOLD phoneme keyboard
    // Live tract parameters (module_speech.md "formant_shift"/"bandwidth_scale"):
    // Q8.8, 256 = 1.0x (neutral). Latest-wins like the rest of VoiceParams --
    // tract.h ramps them per sub-block so a CC sweep can't zipper.
    int16_t  formant_shift;
    int16_t  bandwidth_scale;
    uint8_t    utterance;
    SpeechMode mode;
    uint8_t    rate;  // Q4.4 segment-duration scale, 16 = 1.0x nominal
    // Vibrato/jitter/shimmer (module_speech.md "Vibrato LFO"/"Jitter and
    // shimmer"). All four are live, same push-to-held-voices treatment as
    // formant_shift/bandwidth_scale above -- input_subsystem.cpp's CC
    // handlers for CC1/CC24/CC25/CC26/CC76 write straight into a held
    // voice's shadow copy, not just the per-channel default for future
    // notes.
    uint8_t  jitter;      // 0-255, 0 = perfectly periodic pitch period (excitation.h)
    uint8_t  shimmer;     // 0-255, 0 = perfectly periodic amplitude (excitation.h)
    float    lfo_rate;    // vibrato LFO rate, Hz, 0 = off (excitation.h VIBRATO_*)
    float    lfo_depth;   // vibrato depth, 0.0-1.0
    // Selects which tract render.h renders this voice through (tract.h's
    // SpeechTract), set from CC102 (input_subsystem.cpp).
    SpeechTract tract;
    // KEY_PER_WORD addressing (module_speech.md "LPC Lattice Tract"): the
    // word this voice plays under SPEECH_TRACT_LATTICE, resolved by
    // input_subsystem.cpp from the note number and the channel's current
    // Program-Change page. Defaults to lattice.h's LATTICE_TEST_WORD so a
    // voice is always valid whether or not a real corpus
    // (lattice_words.h) has been generated locally.
    const LatticeWord *lattice_word;
    int16_t lattice_pitch_shift;  // Q8.8, 256 = 1.0x -- live override on the word's own pitch contour
    // Voiced-excitation source, SPEECH_TRACT_LATTICE only (lattice.h's own
    // header comment): false is excitation.h's glottal_pulse(), shared with
    // the formant tract; true is the real TMS5220's own chirp table.
    bool lattice_chirp_exciter;
    // Live throat/mouth timbre pair, SPEECH_TRACT_SAM only (sam.h) -- Q8.8,
    // 256 = 1.0x, the same encoding and live push-to-held-voices treatment
    // as formant_shift/bandwidth_scale above. Set from CC105/CC106
    // (input_subsystem.cpp).
    int16_t sam_throat;
    int16_t sam_mouth;
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 256, 256, SPEECH_NO_UTTERANCE, SPEECH_MODE_GATED, 16,
             0, 0, 0.0f, 0.0f, SPEECH_TRACT_FORMANT, &LATTICE_TEST_WORD, 256, false, 256, 256 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;

// Per-voice formant-space + phoneme telemetry for the display
// (module_speech.md "Display"). F1/F2 are tract.h's SpeechVoice::F[0]/F[1]
// -- the live, ramped values, not the
// phoneme's static target, so a plotted dot moves continuously as the
// segment sequencer (or a mid-note phoneme change) glides between
// targets. `phoneme` is PHONEME_COUNT when the voice has never sounded (no
// valid index yet). `tract` lets display.cpp show a tract-appropriate
// indicator (SAM has no phoneme-label table to look `phoneme` up in). Same
// "diagnostic, no locking" contract audio_engine.h documents for
// audio_engine_load(): Core 1 writes these once per buffer, Core 0's
// ~10 Hz display_task reads them without synchronization -- a torn read
// for one frame is invisible at that redraw rate.
struct SpeechVoiceUiState {
    uint16_t f1_hz, f2_hz;
    uint8_t  phoneme;
    bool     active;
    SpeechTract tract;
};

void speech_voice_ui_state(uint32_t voice, SpeechVoiceUiState *out);
