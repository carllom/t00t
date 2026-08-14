#pragma once

#include <cstdint>

// Chip module engine skeleton (sid.md §1 P1/P2/P3, §14 item 2).
//
// "engines/chip/, VoiceType dispatch, static MIDI-channel->voice map,
// register-stream playback path" -- P0's rig (rig.h) proved the primitives
// and the CPU budget; P1 was the first build that actually played. P2 added
// the filter buses (§5) that budget was sized for. P3 adds the frame table
// VM (§6) -- an instrument now drives waveform/pw/ADSR/filter itself, so
// VoiceParams carries an instrument index instead of raw synthesis params;
// Core 0 only supplies note/gate/instrument-select, per §7.1's ParamExchange
// note ("the VM lives on Core 1").
//
// sid.md §13.3, confirmed by P0 (§9, §14a.9): MAX_VOICES = 32 (allocation
// pool / active-voice bitmap width -- a separate concern from the ~20-voice
// CPU budget target); FILTER_BUS_COUNT = 4, confirmed by the same
// measurement. Both are needed before #include "engine_base.h", which sizes
// VoiceParamBlockT's bus[] array from FILTER_BUS_COUNT.
static constexpr uint32_t MAX_VOICES = 32;
static constexpr uint32_t FILTER_BUS_COUNT = 4;

#include "engine_base.h"

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

// sid.md §5: "VoiceParams carries only uint8_t filter_bus, with BUS_NONE as
// sentinel." 0xff rather than -1 since filter_bus is unsigned (matching
// FILTER_BUS_COUNT's own type) and is compared with `< FILTER_BUS_COUNT`
// everywhere, which already excludes it without a separate check.
static constexpr uint8_t BUS_NONE = 0xff;

struct VoiceParams {
    VoiceType type;
    uint16_t freq;        // SID frequency register, the control-plane unit (§4.1) --
                           // base pitch (bend already applied by Core 0); the frame
                           // VM layers wave-table arpeggio + vibrato on top of this
                           // on Core 1, per note-on, not per sample
    uint8_t  instrument;   // index into INSTRUMENTS[] (engines/chip/instruments.h) --
                           // drives waveform/pw/ADSR/filter; §6, replaces P1's raw fields
    uint8_t  trigger;      // generation counter, incremented on each note-on
    uint8_t  velocity;     // 1-127 (§13.7)
    bool     gate;
    uint8_t  filter_bus;   // index into VoiceParamBlock::bus[], or BUS_NONE (§5) --
                           // set by Core 0's bind_filter() at note-on, from the
                           // selected instrument's uses_filter flag
};

// Default voice is zero-init -- VoiceType 0 is VT_SILENT, so this is the
// generic engine_base.h default already. No specialization needed (matches
// groovebox's engine.h).

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange   = ParamExchangeT<VoiceParams>;
