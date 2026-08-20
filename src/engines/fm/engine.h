#pragma once

#include <cstdint>

#include "patch.h"  // FmPatch -- no pico-sdk dependency, safe ahead of engine_base.h

// MAX_VOICES=16, defined ahead of engine_base.h so patch.h is available
// before it. See module_fm.md §3.4/§9 for the current voice-count
// constraints.
static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (module_chip.md §5)

#include "engine_base.h"

// The FM engine: a 6-operator voice whose routing is patch data (patch.h),
// rendered by op.h's kernels, played from MIDI note on/off. Delay/reverb
// stay linked (module_fm.md §2).
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

// Channel -> patch lookup for the display (module_fm.md "Display"). Same
// Core-0-only state input_subsystem.cpp's note-on path already reads;
// always valid (defaults to &FM_TEST_PATCH), whether or not patches.h is
// present.
const FmPatch *fm_channel_patch(uint8_t channel);

// Per-voice channel/patch telemetry for the display's multitimbral grid.
// `program` is the channel's FM_PATCHES[] index at that voice's last
// note-on, for display alongside voice_alloc_active_mask() -- the caller
// decides whether a voice slot is worth showing.
struct FmVoiceUiState {
    uint8_t channel;
    uint8_t program;
};
void fm_voice_ui_state(uint32_t voice, FmVoiceUiState *out);
