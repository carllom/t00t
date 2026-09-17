#pragma once

#include <cstdint>

#include "tables.h"  // WaveTable, WT_BANK -- no pico-sdk dependency, safe ahead of engine_base.h

// MAX_VOICES=8, defined ahead of engine_base.h so tables.h is available --
// matches real PPG Wave 2.2/2.3 hardware's own polyphony exactly
// (module_wavetable.md's PPG Architecture Reference), not a headroom-driven
// choice like fm's/subtractive's MAX_VOICES=16. One wavetable partial per
// voice: this skeleton has no partial pool yet (module_wavetable.md's
// Two-Level Allocation is future work), so voice count and partial count
// are the same thing here. Chosen at 8 rather than 16 specifically because
// the per-voice filter below is unmeasured on real hardware together with
// the rest of the chassis -- see module_wavetable.md's Decision Record for
// the budget math this sidesteps.
static constexpr uint32_t MAX_VOICES = 8;
static constexpr uint32_t FILTER_BUS_COUNT = 0;  // per-voice filter state below, not a shared bus -- see engine's own SVFilter array

#include "engine_base.h"

// The wavetable engine skeleton: one wavetable partial per voice (osc/
// wavetable.h), an ADSR envelope (src/envelope.h, reused unmodified), a
// basic per-voice lowpass filter (audio_engine.cpp), no LFO, no partial
// pool -- see module_wavetable.md for what's deferred and why. Dynamically
// allocated like every other engine except tracker/groovebox, so the plain
// latest-wins ParamExchange/voice_alloc is kept as-is.
struct VoiceParams {
    uint32_t phase_inc;      // Q0.32 phase increment (osc/wavetable.h), at the note's (bend-scaled) frequency
    uint8_t  wave_pos;       // 0..WT_INDEX_SIZE-1 -- live via CC1 (mod wheel wave-scan)
    int16_t  amplitude;      // velocity, 0-32767
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    int16_t  pan;            // Q15 pan: -32768 = full left, 0 = center, 32767 = full right
    const WaveTable *table;  // which wavetable (Program Change selects, future notes only)
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, WT_INDEX_SIZE / 2, 0, 0, false, 0, &WT_BANK[0] };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
