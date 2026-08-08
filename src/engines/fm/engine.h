#pragma once

#include <cstdint>

#include "patch.h"  // FmPatch -- no pico-sdk dependency, safe ahead of engine_base.h

// FM engine skeleton (#41): MAX_VOICES=16, defined ahead of engine_base.h
// per #10. This is fm.md §3.4's working assumption for full 6-operator
// polyphony -- explicitly PROVISIONAL pending the P0 measurement gate
// ("Plan against 16 voices ... If P0 comes back better than expected, the
// surplus is spent on polyphony, not on features"; §3.4's sensitivity table
// ranges 12-20+ depending on measured cycles/operator). #43 measured
// 100.05 c/f/voice (kernel only) and *confirmed* 16 rather than raising it
// -- see fm.md §3.4. Only this engine -- the other four are untouched.
static constexpr uint32_t MAX_VOICES = 16;

#include "engine_base.h"

// FM engine (#44, fm.md P1): the first musically real slice -- a 6-operator
// voice whose routing is patch data (patch.h), rendered by op.h's kernels,
// played from MIDI note on/off. Supersedes #41's fixed test-tone skeleton;
// delay/reverb stay linked (fm.md §2: "FM's whole working set is ~12 KB, so
// the 128 KB delay line costs it nothing it needs").
//
// Unlike the tracker (#13), FM has no fixed channel->voice mapping, so the
// plain latest-wins ParamExchange/voice_alloc that subtractive/groovebox/
// speech also use is kept as-is -- no CMakeLists.txt override needed.
struct VoiceParams {
    uint32_t phase_inc;      // Q32 phase inc for a ratio=1.0 operator at the note's (bend-scaled) frequency
    int16_t  amplitude;      // velocity, 0-32767 -- "plain amplitude" scaling of carrier output (#44; sensitivity is #45)
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    int16_t  pan;            // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    const FmPatch *patch;    // the whole timbre, one pointer (fm.md §6.3) -- always &FM_TEST_PATCH until P3's converter
    uint8_t  note;           // #48: raw MIDI note (0-127), unbent -- key level/rate scaling need the note itself, not just phase_inc's already-bend-scaled frequency
    int16_t  mod_wheel;      // #49: Q15, 0..32767 -- CC1, scales the patch's own LFO PMD/AMD depth (lfo.h)
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, &FM_TEST_PATCH, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
