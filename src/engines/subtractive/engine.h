#pragma once

#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// Subtractive engine — the general-purpose synth (ADSR + LFO + SVF + osc
// dispatch). Shared enums/constants (Waveform, FilterMode, EffectParams,
// PROFILE_PIN) and the VoiceParamBlockT/ParamExchangeT mechanism live in
// engine_base.h.

// The note-on group of a cooked patch (patch.h): fixed for a note when it
// starts, so Core 0 copies it into the note's VoiceParams.
struct SubNoteOnCooked {
    const SampleDef *sample;     // nullptr unless waveform == WAVE_SAMPLE
    Waveform waveform;
    FilterMode filter_mode;
};

// Voice parameters: written by Core 0, read by Core 1.
// The per-note inputs and the note's note-on fields -- no phase state. The
// live part of the sound comes from the voice's Part's cooked patch.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point phase increment (pre-computed by Core 0)
    int16_t amplitude;   // velocity amplitude (0–32767)
    uint8_t trigger;     // generation counter, incremented on each note-on
    bool gate;           // true while key held, false on release
    uint8_t part;        // Part whose live fields this note follows (< SUB_PART_COUNT)
    uint16_t load_id;    // the Patch load the note belongs to (PatchPlatformT::load_id())
    int16_t mod_depth;   // mod-wheel vibrato depth, Q15 (0 = off) — dedicated LFO on Core 1
    SubNoteOnCooked note_on;
};

// Default voice: silent, Part 0, sine. Pan, volume and FX send come from
// the voice's Part in the Multi.
template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 0, { nullptr, WAVE_SINE, FILTER_OFF } };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
