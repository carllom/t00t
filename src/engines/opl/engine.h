#pragma once

#include <cstdint>

// MAX_VOICES=9, defined ahead of engine_base.h -- real OPL2 hardware's own
// channel count, not chosen for headroom the way FM's 16 was.
static constexpr uint32_t MAX_VOICES = 9;
static constexpr uint32_t FILTER_BUS_COUNT = 0;  // chip module only

#include "engine_base.h"

// The OPL engine: a 2- or 4-operator voice whose algorithm is one of six
// fixed routings (patch.h), rendered by ../fm/op.h's reused kernels, played
// from MIDI note on/off. Delay/reverb stay linked, not conditionally
// compiled.
//
// No fixed channel->voice mapping, so the plain latest-wins ParamExchange/
// voice_alloc every other dynamically-allocated engine uses is kept as-is --
// no CMakeLists.txt override needed.

// Per-note parameters, written by Core 0 and read by Core 1. The note's
// sound comes from its Part's cooked patch (opl_module.h): Core 1 copies it
// when the note starts, once the cook the note was played with has
// arrived (`load_id`/`cook_seq`), so no patch data crosses ParamExchange.
struct VoiceParams {
    uint32_t phase_inc;      // Q32 phase inc for a mult=1 operator at the note's (bend-scaled) frequency
    int16_t  amplitude;      // velocity, 0-32767
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    uint8_t  part;           // the Part the note plays on (< OPL_PART_COUNT)
    uint8_t  note;           // raw MIDI note (0-127), for key scale level/rate
    uint16_t load_id;        // the Patch load the note belongs to (PatchPlatformT::load_id())
    uint16_t cook_seq;       // the Part's latest cook when the note started (PatchPlatformT::cook_seq())
    int16_t  mod_wheel;      // Q15, 0..32767 -- CC1, scales vibrato depth
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 0, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
