#pragma once

#include <cstdint>

// Chip module engine skeleton (module_chip.md §1).
//
// A Patch drives waveform/pw/ADSR/filter itself (§6's frame table VM), so
// VoiceParams carries the note's Part and Patch load instead of raw
// synthesis params; Core 1 copies the Part's cooked patch when the note
// starts (chip_render.h).
//
// MAX_VOICES = 32 (allocation pool / active-voice bitmap width -- a
// separate concern from the ~20-voice CPU budget target); FILTER_BUS_COUNT
// = 4 (module_chip.md §13.3). Both are needed before #include
// "engine_base.h", which sizes VoiceParamBlockT's bus[] array from
// FILTER_BUS_COUNT.
static constexpr uint32_t MAX_VOICES = 32;
static constexpr uint32_t FILTER_BUS_COUNT = 4;

#include "engine_base.h"

// module_chip.md §7.3: "Chip" survives only as a per-voice tonal-profile tag,
// dispatched in the render loop exactly like the groovebox's VoiceType --
// not a container, not an allocation unit. VT_SILENT = 0 so a zero-inited
// (unused) voice slot is silent by construction, same convention as every
// other engine's VoiceType.
enum VoiceType : uint8_t {
    VT_SILENT = 0,
    VT_SID,          // 6581/8580 voice
    VT_AY,           // AY-3-8910/YM2149 voice (module_chip.md §12.1) -- one
                      // AyTone + one AyNoise + one AyEnvelope per voice, not
                      // the real chip's shared-noise/shared-envelope-across-
                      // 3-channels topology (full 32-voice independence
                      // traded for that hardware trick)
    // later: VT_SN76489, VT_NES_PULSE, VT_NES_TRI, VT_NES_NOISE, VT_GB_WAVE
};

// VoiceParams carries only uint8_t filter_bus, with BUS_NONE as sentinel
// (module_chip.md §5). 0xff rather than -1 since filter_bus is unsigned
// (matching FILTER_BUS_COUNT's own type) and is compared with
// `< FILTER_BUS_COUNT` everywhere, which already excludes it without a
// separate check.
static constexpr uint8_t BUS_NONE = 0xff;

struct VoiceParams {
    VoiceType type;       // the chip of the Part's Patch as Core 0 saw it at note-on;
                           // VT_SILENT until the voice first plays. Core 1 plays the chip
                           // its own copy of the Patch names, which can be a later cook.
    uint16_t sid_freq;    // The note's pitch in both chips' own registers, converted by
    uint16_t ay_period;   // Core 0 (note_freq.h), so Core 1 has the one its Patch copy
                           // needs: SID's freq_reg (§4.1) and AY's 12-bit tone period
                           // (inverted: period, not frequency-proportional). Bend already
                           // applied. Each chip's own frame VM layers arpeggio + vibrato
                           // on top on Core 1: SID's wave table multiplies a ratio onto
                           // freq_reg (§6); AY's divides the period by the same ratio,
                           // since AY's period is inversely proportional to pitch
                           // (module_chip.md §12.2, note_freq.h).
    uint8_t  part;         // the Part this note plays for
    uint16_t load_id;      // the Part's Patch load when the note started
    uint16_t cook_seq;     // that Patch's latest cook when the note started
    uint8_t  trigger;      // generation counter, incremented on each note-on
    uint8_t  velocity;     // 1-127 (§13.7)
    bool     gate;
    uint8_t  filter_bus;   // filter bus index (< FILTER_BUS_COUNT), or BUS_NONE (§5) --
                           // set by Core 0's bind_filter() at note-on, from the
                           // Part's Patch's FILTER bits; Core 1 routes the voice
                           // through it only if its own Patch copy filters.
    uint8_t  speaker_preset;   // module_chip.md §10: SpeakerPreset (speaker_sim.h).
                           // A single global choice, not really per-voice, but
                           // VoiceParams is the only Core0->Core1 channel and
                           // this is one byte -- replicated identically to
                           // every voice by input_subsystem.cpp (CC17), Core 1
                           // reads it once from voice 0 rather than widening
                           // the shared VoiceParamBlockT (engine_base.h) for
                           // every other engine to carry unused.
};

// Telemetry for the LCD (module_chip.md §1): current table row + its
// Part, same shape as speech's per-voice phoneme display -- enough
// for the display to show what each voice is actually doing without
// widening the reverse channel bitmap itself.
struct ChipVoiceUiState {
    VoiceType type;        // module_chip.md §12.4: VT_SILENT/VT_SID/VT_AY -- how
                            // display.cpp labels the voice
    bool    held;         // gate currently true (note held, not just ringing out)
    uint8_t part;          // the Part the voice plays for
    uint8_t wave_pos;      // current row in the Patch's wave table (VT_SID) or its
                            // arpeggio (VT_AY) -- same field, same meaning either way
};
void chip_voice_ui_state(uint32_t voice, ChipVoiceUiState *out);
uint8_t chip_speaker_preset_ui();   // SpeakerPreset (speaker_sim.h), for display.cpp

// Default voice is zero-init -- VoiceType 0 is VT_SILENT, so this is the
// generic engine_base.h default already. No specialization needed (matches
// groovebox's engine.h).

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange   = ParamExchangeT<VoiceParams>;
