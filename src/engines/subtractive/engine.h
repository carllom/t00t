#pragma once

#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// Subtractive engine — the general-purpose synth (ADSR + LFO + SVF + osc
// dispatch). Shared enums/constants (Waveform, FilterMode, EffectParams,
// PROFILE_PIN) and the VoiceParamBlockT/ParamExchangeT mechanism live in
// engine_base.h.

// Voice parameters: written by Core 0, read by Core 1.
// Only the per-note inputs -- no phase state, and nothing that comes from
// the Patch: the sound itself reaches Core 1 as the voice's Part's cooked
// patch (patch.h), copied per voice when the note starts.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point phase increment (pre-computed by Core 0)
    int16_t amplitude;   // velocity amplitude (0–32767)
    uint8_t trigger;     // generation counter, incremented on each note-on
    bool gate;           // true while key held, false on release
    uint8_t part;        // Part whose cooked patch this note plays (< SUB_PART_COUNT)
    uint8_t patch_tag;   // low byte of that cooked patch's slot generation, as Core 0 saw it
    int16_t mod_depth;   // mod-wheel vibrato depth, Q15 (0 = off) — dedicated LFO on Core 1
    int16_t pan;         // Q15 pan: -32768 = full left, 0 = center, 32767 = full right (CC10)
};

// Default voice: silent, Part 0, centered pan.
template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
