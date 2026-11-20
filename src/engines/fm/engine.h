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

// Per-note parameters, written by Core 0 and read by Core 1. The note's
// sound comes from its Part's cooked patch (fm_module.h): Core 1 copies it
// when the note starts, once the cook the note was played with has
// arrived (`load_id`/`cook_seq`), so no patch data crosses ParamExchange.
struct VoiceParams {
    uint32_t phase_inc;      // Q32 phase inc for a ratio=1.0 operator at the note's (bend-scaled) frequency
    int16_t  amplitude;      // velocity, 0-32767
    uint8_t  trigger;        // generation counter, incremented on each note-on
    bool     gate;           // true while voice should sound
    uint8_t  part;           // the Part the note plays on (< FM_PART_COUNT)
    uint8_t  note;           // raw MIDI note (0-127), unbent -- key level/rate scaling need the note itself, not just phase_inc's already-bend-scaled frequency
    uint16_t load_id;        // the Patch load the note belongs to (PatchPlatformT::load_id())
    uint16_t cook_seq;       // the Part's latest cook when the note started (PatchPlatformT::cook_seq())
    int16_t  mod_wheel;      // Q15, 0..32767 -- CC1, a source competing with the patch's own LFO PMD/AMD depth (lfo.h's max() rule)
};

template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, 0, 0, 0, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;

// Per-voice channel/Part telemetry for the display's multitimbral grid:
// the channel and the Part's program at that voice's last note-on, for
// display alongside voice_alloc_active_mask() -- the caller decides whether
// a voice slot is worth showing.
struct FmVoiceUiState {
    uint8_t channel;
    uint8_t program;
};
void fm_voice_ui_state(uint32_t voice, FmVoiceUiState *out);
