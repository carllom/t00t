#pragma once

#include <cstdint>

// Chip module engine skeleton (sid.md §1 P1, §14 item 2).
//
// "engines/chip/, VoiceType dispatch, static MIDI-channel->voice map,
// register-stream playback path" -- P0's rig (rig.h) proved the primitives
// and the CPU budget; this is the first build that actually plays. No filter
// buses yet (P2), no frame table VM yet (P3, so no vibrato/arpeggio/hard-sync
// sweep) -- a note is freq+pw+waveform+ADSR held static for its duration,
// same as P1 in every other engine that has one.
//
// sid.md §13.3, confirmed by P0 (§9, §14a.9): MAX_VOICES = 32 (allocation
// pool / active-voice bitmap width -- a separate concern from the ~20-voice
// CPU budget target). FILTER_BUS_COUNT is on record for P2; unused until then.
static constexpr uint32_t MAX_VOICES = 32;

#include "engine_base.h"

static constexpr uint32_t FILTER_BUS_COUNT = 4;

// sid.md §7.3: "Chip" survives only as a per-voice tonal-profile tag,
// dispatched in the render loop exactly like the groovebox's VoiceType --
// not a container, not an allocation unit. VT_SILENT = 0 so a zero-inited
// (unused) voice slot is silent by construction, same convention as every
// other engine's VoiceType.
enum VoiceType : uint8_t {
    VT_SILENT = 0,
    VT_SID,          // 6581/8580 voice
    // later: VT_AY, VT_SN76489, VT_NES_PULSE, VT_NES_TRI, VT_NES_NOISE, VT_GB_WAVE
};

struct VoiceParams {
    VoiceType type;
    uint16_t freq;      // SID frequency register, the control-plane unit (§4.1)
    uint16_t pw;        // 12-bit pulse width
    uint8_t  waveform;  // SidWave bits
    uint8_t  ad;        // ADSR attack/decay nibbles
    uint8_t  sr;        // ADSR sustain/release nibbles
    uint8_t  trigger;   // generation counter, incremented on each note-on
    uint8_t  velocity;  // 1-127 (§13.7)
    bool     gate;
};

// Default voice is zero-init -- VoiceType 0 is VT_SILENT, so this is the
// generic engine_base.h default already. No specialization needed (matches
// groovebox's engine.h).

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange   = ParamExchangeT<VoiceParams>;
