#pragma once

#include <cstdint>

// Chip module, F0 measurement build (sid.md §1 P0, §14 item 1).
//
// This is NOT the engine skeleton. sid.md §1 puts the skeleton at P1 --
// "engines/chip/, VoiceType dispatch, static MIDI-channel->voice map,
// register-stream playback path" -- and P0 explicitly at "No engine, no VM".
// What lives here is the smallest flashable thing that can carry rig.h onto
// hardware, because cycles per frame cannot be measured on a host.
//
// VoiceParams therefore exists only to satisfy engine_base.h's templates and
// main.cpp's plumbing; the rig drives its voices directly and never reads it,
// exactly as the speech engine's SPEECH_PROFILE build bypasses ParamExchange.
// P1 replaces this file wholesale.

// sid.md §13.3: "MAX_VOICES = 32, FILTER_BUS_COUNT = 4 provisionally. Revisit
// after P0." Provisional is the operative word -- these are the values whose
// justification the hardware checkpoint exists to supply or refute.
static constexpr uint32_t MAX_VOICES = 32;

#include "engine_base.h"

// sid.md §7.1 makes FilterBusParams a new sibling in the param block, a change
// to engine_base.h shared by all engines. That change belongs to P2 (§14 item
// 3), not here: making it now would touch four other engines' builds to
// support a rig that does not read it. FILTER_BUS_COUNT is defined so the
// intent is on record and rig.h's sizing has a name.
static constexpr uint32_t FILTER_BUS_COUNT = 4;

struct VoiceParams {
    uint32_t freq;      // SID frequency register, the control-plane unit (§4.1)
    uint16_t pw;        // 12-bit pulse width
    uint8_t  waveform;  // SidWave bits
    uint8_t  ad;        // ADSR attack/decay nibbles
    uint8_t  sr;        // ADSR sustain/release nibbles
    uint8_t  trigger;   // generation counter, incremented on each note-on
    uint8_t  velocity;  // 1-127 (§13.7)
    bool     gate;
};

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange   = ParamExchangeT<VoiceParams>;
