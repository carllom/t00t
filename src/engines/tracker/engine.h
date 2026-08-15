#pragma once

#include <cstdint>

// XM channel count is fixed in the module header, 2-32 (module_tracker.md "Format
// Decision"). voice_alloc is not used at all — channel N is voice N, fixed
// assignment, no allocation, no stealing — so 32 is this engine's actual
// voice count, not a headroom margin like the other two engines' MAX_VOICES.
// The active-voice bitmap (one uint32_t, per engine_base.h) is exactly full.
static constexpr uint32_t MAX_VOICES = 32;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// This engine's own VoiceParams payload, required by engine_base.h's shared
// template shape (main.cpp instantiates a ParamExchange for every engine).
// The tracker's real per-tick state travels through player.h's ordered
// TickBlock ring instead -- audio_engine.cpp and midi_controller.cpp both
// ignore their ParamExchange pointer, so nothing here is actually read or
// written outside voice_params_default()'s initialization.
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
