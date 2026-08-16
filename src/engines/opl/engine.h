#pragma once

#include <cstdint>

#include "patches.h"  // OplPatch, OPL_PATCH_LEAD -- no pico-sdk dependency, safe ahead of engine_base.h

// MAX_VOICES=9, defined ahead of engine_base.h -- real OPL2 hardware's own
// channel count, not chosen for headroom the way FM's 16 was.
static constexpr uint32_t MAX_VOICES = 9;
static constexpr uint32_t FILTER_BUS_COUNT = 0;  // chip module only

#include "engine_base.h"

// The OPL engine: a 2-operator voice whose algorithm is one of two fixed
// routings (patch.h), rendered by ../fm/op.h's reused kernels, played from
// MIDI note on/off. Delay/reverb stay linked, not conditionally compiled.
//
// No fixed channel->voice mapping, so the plain latest-wins ParamExchange/
// voice_alloc every other dynamically-allocated engine uses is kept as-is --
// no CMakeLists.txt override needed.
struct VoiceParams {
    uint32_t phase_inc;      // Q32 phase inc for a mult=1 operator at the note's (bend-scaled) frequency
    int16_t  amplitude;      // velocity, 0-32767
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    int16_t  pan;            // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    const OplPatch *patch;   // the whole timbre, one pointer
    uint8_t  note;           // raw MIDI note (0-127), for key scale level/rate
    int16_t  mod_wheel;      // Q15, 0..32767 -- CC1, scales vibrato depth
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, &OPL_PATCH_LEAD, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;

// Channel -> patch lookup for the display. Same Core-0-only state
// midi_controller.cpp's note-on path already reads; always valid (defaults
// to &OPL_PATCH_LEAD).
const OplPatch *opl_channel_patch(uint8_t channel);
