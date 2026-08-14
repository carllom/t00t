#pragma once

#include <cstdint>

// XM channel count is fixed in the module header, 2-32 (tracker.md "Format
// Decision"). voice_alloc is not used at all — channel N is voice N, fixed
// assignment, no allocation, no stealing — so 32 is this engine's actual
// voice count, not a headroom margin like the other two engines' MAX_VOICES.
// The active-voice bitmap (one uint32_t, per engine_base.h) is exactly full.
static constexpr uint32_t MAX_VOICES = 32;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (chip.md §5)

#include "engine_base.h"

// Tracker engine skeleton (#13): proves the build seam, the MAX_VOICES=32
// deviation, and the stereo output tail before any XM/mixer logic lands. No
// pattern data, no sample playback, no tick handoff yet — VoiceParams here
// only carries enough to drive a fixed test tone. tracker.md's ordered
// TickBlock ring (replacing this ParamExchange's latest-wins semantics)
// lands with the real mixer.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point phase increment
    int16_t amplitude;   // 0-32767
    uint8_t trigger;     // generation counter, incremented on each note-on
    bool gate;           // true while voice should sound
    int16_t pan;         // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
