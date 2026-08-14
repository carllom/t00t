#pragma once

#include <cstdint>

// Chip module engine skeleton (chip.md §1 P1/P2/P3, §14 item 2).
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
// chip.md §13.3, confirmed by P0 (§9, §14a.9): MAX_VOICES = 32 (allocation
// pool / active-voice bitmap width -- a separate concern from the ~20-voice
// CPU budget target); FILTER_BUS_COUNT = 4, confirmed by the same
// measurement. Both are needed before #include "engine_base.h", which sizes
// VoiceParamBlockT's bus[] array from FILTER_BUS_COUNT.
static constexpr uint32_t MAX_VOICES = 32;
static constexpr uint32_t FILTER_BUS_COUNT = 4;

#include "engine_base.h"

// chip.md §7.3: "Chip" survives only as a per-voice tonal-profile tag,
// dispatched in the render loop exactly like the groovebox's VoiceType --
// not a container, not an allocation unit. VT_SILENT = 0 so a zero-inited
// (unused) voice slot is silent by construction, same convention as every
// other engine's VoiceType.
enum VoiceType : uint8_t {
    VT_SILENT = 0,
    VT_SID,          // 6581/8580 voice
    VT_AY,           // AY-3-8910/YM2149 voice (chip.md §12.1, AY-P1) -- one
                      // AyTone + one AyNoise + one AyEnvelope per voice, not
                      // the real chip's shared-noise/shared-envelope-across-
                      // 3-channels topology (§12.1's own design note: full
                      // 32-voice independence traded for that hardware trick)
    // later: VT_SN76489, VT_NES_PULSE, VT_NES_TRI, VT_NES_NOISE, VT_GB_WAVE
};

// chip.md §5: "VoiceParams carries only uint8_t filter_bus, with BUS_NONE as
// sentinel." 0xff rather than -1 since filter_bus is unsigned (matching
// FILTER_BUS_COUNT's own type) and is compared with `< FILTER_BUS_COUNT`
// everywhere, which already excludes it without a separate check.
static constexpr uint8_t BUS_NONE = 0xff;

struct VoiceParams {
    VoiceType type;
    uint16_t freq;        // The target chip's own pitch register, already converted by
                           // Core 0 (note_freq.h) -- SID's freq_reg for VT_SID (§4.1),
                           // AY's 12-bit tone period for VT_AY (inverted: period, not
                           // frequency-proportional). Bend already applied. For VT_SID
                           // the frame VM layers wave-table arpeggio + vibrato on top of
                           // this on Core 1, per note-on, not per sample; VT_AY has no
                           // frame VM yet (AY-P1, chip.md §12.1) so this is the whole story.
    uint8_t  instrument;   // index into INSTRUMENTS[] (VT_SID, engines/chip/instruments.h)
                           // or AY_INSTRUMENTS[] (VT_AY, ay_instruments.h) -- which table
                           // depends on type, resolved by midi_controller.cpp at note-on
                           // from one combined selection space (TOTAL_INSTRUMENT_COUNT)
    uint8_t  trigger;      // generation counter, incremented on each note-on
    uint8_t  velocity;     // 1-127 (§13.7)
    bool     gate;
    uint8_t  filter_bus;   // index into VoiceParamBlock::bus[], or BUS_NONE (§5) --
                           // set by Core 0's bind_filter() at note-on, from the
                           // selected instrument's uses_filter flag. Always BUS_NONE
                           // for VT_AY -- this chip has no filter model.
    uint8_t  speaker_preset;   // chip.md §1 P5, §10: SpeakerPreset (speaker_sim.h).
                           // A single global choice, not really per-voice, but
                           // VoiceParams is the only Core0->Core1 channel and
                           // this is one byte -- replicated identically to
                           // every voice by midi_controller.cpp (CC17), Core 1
                           // reads it once from voice 0 rather than widening
                           // the shared VoiceParamBlockT (engine_base.h) for
                           // every other engine to carry unused.
};

// chip.md §1 P5 open question 3 ("telemetry struct contents for the LCD"):
// current table row + active instrument, same shape as speech's per-voice
// phoneme display -- enough for the display to show what each voice is
// actually doing without widening the reverse channel bitmap itself.
struct ChipVoiceUiState {
    VoiceType type;        // chip.md §12.3: VT_SILENT/VT_SID/VT_AY -- display.cpp
                            // needs this to know which instrument table `instrument`
                            // indexes and how to label the voice (was implicitly
                            // VT_SID-only through P2, when AY voices always reported
                            // the all-zero/inactive state here)
    bool    held;         // gate currently true (note held, not just ringing out)
    uint8_t instrument;    // index into INSTRUMENTS[] (VT_SID) or AY_INSTRUMENTS[] (VT_AY)
    uint8_t wave_pos;      // current row in the instrument's wave table (VT_SID) or
                            // tone table (VT_AY) -- same field, same meaning either way
};
void chip_voice_ui_state(uint32_t voice, ChipVoiceUiState *out);
uint8_t chip_speaker_preset_ui();   // SpeakerPreset (speaker_sim.h), for display.cpp

// Default voice is zero-init -- VoiceType 0 is VT_SILENT, so this is the
// generic engine_base.h default already. No specialization needed (matches
// groovebox's engine.h).

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange   = ParamExchangeT<VoiceParams>;
