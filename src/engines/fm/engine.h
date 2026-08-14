#pragma once

#include <cstdint>

#include "patch.h"  // FmPatch -- no pico-sdk dependency, safe ahead of engine_base.h

// MAX_VOICES=16, defined ahead of engine_base.h per #10. Provisional, not
// settled: full-engine hardware measurement (F8) came back well above the
// projection this number was originally confirmed against, and the
// reverb-safe count may land closer to 11-14 -- see module_fm.md §3.4/§9 for
// the current numbers and what's still open before this changes.
static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// The FM engine: a 6-operator voice whose routing is patch data (patch.h),
// rendered by op.h's kernels, played from MIDI note on/off. Delay/reverb
// stay linked (module_fm.md §2: FM's whole working set is small enough that
// the shared 128 KB delay line costs it nothing it needs).
//
// FM has no fixed channel->voice mapping, so the plain latest-wins
// ParamExchange/voice_alloc that subtractive/groovebox/speech also use is
// kept as-is -- no CMakeLists.txt override needed.
struct VoiceParams {
    uint32_t phase_inc;      // Q32 phase inc for a ratio=1.0 operator at the note's (bend-scaled) frequency
    int16_t  amplitude;      // velocity, 0-32767 -- plain amplitude scaling of carrier output
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    int16_t  pan;            // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    const FmPatch *patch;    // the whole timbre, one pointer (module_fm.md §6.3) -- &FM_TEST_PATCH by
                              // default, or one of FM_PATCHES[] once tools/syx2patch.py has been run
                              // (T00T_FM_HAS_PATCHES)
    uint8_t  note;           // raw MIDI note (0-127), unbent -- key level/rate scaling need the note itself, not just phase_inc's already-bend-scaled frequency
    int16_t  mod_wheel;      // Q15, 0..32767 -- CC1, a source competing with the patch's own LFO PMD/AMD depth (lfo.h's max() rule)
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, &FM_TEST_PATCH, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
