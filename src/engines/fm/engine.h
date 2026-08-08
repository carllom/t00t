#pragma once

#include <cstdint>

// FM engine skeleton (#41): MAX_VOICES=16, defined ahead of engine_base.h
// per #10. This is fm.md §3.4's working assumption for full 6-operator
// polyphony -- explicitly PROVISIONAL pending the P0 measurement gate
// ("Plan against 16 voices ... If P0 comes back better than expected, the
// surplus is spent on polyphony, not on features"; §3.4's sensitivity table
// ranges 12-20+ depending on measured cycles/operator). Only this engine --
// the other four are untouched.
static constexpr uint32_t MAX_VOICES = 16;

#include "engine_base.h"

// FM engine skeleton (#41): proves the fifth build-time engine seam --
// MAX_VOICES=16 (provisional), the FM-specific 4096-entry sine table
// (sine_tab.h, phase >> 20, no interpolation), and delay/reverb staying
// linked (fm.md §2: "FM's whole working set is ~12 KB, so the 128 KB delay
// line costs it nothing it needs") -- before any operator kernel, patch
// struct, envelope, LFO, or algorithm table exists (fm.md P1+). VoiceParams
// here only carries enough to drive a fixed test tone through the standard
// ParamExchange/voice_alloc path.
//
// Unlike the tracker (#13), FM has no fixed channel->voice mapping, so the
// plain latest-wins ParamExchange/voice_alloc that subtractive/groovebox/
// speech also use is kept as-is -- no CMakeLists.txt override needed.
struct VoiceParams {
    uint32_t phase_inc;  // Q32 phase increment into the FM sine table (sine_tab.h)
    int16_t  amplitude;  // 0-32767
    uint8_t  trigger;    // generation counter, incremented on each note-on
    bool     gate;       // true while voice should sound
    int16_t  pan;        // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
