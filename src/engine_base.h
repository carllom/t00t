#pragma once

#include "audio_common.h"
#include "hardware/sync.h"
#include <cstdint>

// Engine-agnostic definitions shared by every synthesis engine and by the
// reusable DSP layer (oscillators, filter, effects, voice allocator).
//
// Engine-specific state — each engine's own VoiceParams payload struct and
// its default-value logic — lives in that engine's own engine.h
// (src/engines/<engine>/engine.h), which instantiates the VoiceParamBlockT /
// ParamExchangeT templates below with its own VoiceParams type.

struct SampleDef;  // forward declaration (defined in osc/sample_def.h)

// MAX_VOICES is engine-specific (tracker/speech/FM want different voice
// counts) and is defined by each engine's own engine.h, before its
// #include "engine_base.h".

// Profiling pin — GPIO 22 (moved from GPIO 2 to avoid coupling to Button A on GPIO 0)
static constexpr uint32_t PROFILE_PIN = 22;

// Core 0 duty-cycle profiling pin — GPIO 21. High for the busy portion of
// main.cpp's loop (from wake to the __wfi() that sleeps until the next IRQ),
// mirroring how PROFILE_PIN (GPIO 22) brackets Core 1's per-buffer render.
static constexpr uint32_t PROFILE_PIN_CORE0 = 21;

enum Waveform : uint8_t { WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE, WAVE_SQUARE_BLEP, WAVE_SAW_BLEP, WAVE_SAMPLE };

enum FilterMode : uint8_t { FILTER_OFF, FILTER_LP, FILTER_BP, FILTER_HP, FILTER_NOTCH };

// Effect selector. CC74 picks the type; the same three knobs (CC72/73/75) then
// drive whichever effect is active.
enum EffectType : uint8_t { FX_OFF, FX_DELAY, FX_REVERB, FX_COUNT };

// Global effect parameters. Written by Core 0, read by Core 1. The three params
// are raw 0..127 controller values; each effect maps them to its own scale.
struct EffectParams {
    uint8_t type;   // EffectType (CC74)
    uint8_t mix;    // CC73: wet/dry — 0 = dry, 127 = full wet
    uint8_t p1;     // CC72: delay feedback / reverb room size
    uint8_t p2;     // CC75: delay time  / reverb damping
};

// Filter bus model (chip module, chip.md §5/§7.1) — a small typed pool of
// filters voices can share, sized to bound worst-case CPU load by
// construction rather than by what the player happens to play. FB_SVF is a
// plain two-pole filter any engine could reuse; FB_6581/FB_8580 select a
// chip-specific cutoff LUT plus optional saturation. FB_OFF marks an unbound
// slot. Not per-voice, not global — a new sibling in the param block.
enum FilterModel : uint8_t { FB_OFF, FB_6581, FB_8580, FB_SVF };

struct FilterBusParams {
    uint8_t  model;       // FilterModel
    uint8_t  mode_mask;   // LP | BP | HP, summed — SID mode-register semantics
    uint16_t cutoff;      // raw register units, mapped by the model's LUT
    uint16_t resonance;   // raw register units (SID: 0-15 nibble)
};

// FILTER_BUS_COUNT is engine-specific, same rule as MAX_VOICES above: each
// engine.h defines it (0 if unused) before its own #include "engine_base.h".
// Deliberately not a preprocessor #ifndef default here -- an engine that
// declares it as a real constexpr (as chip's engine.h does, so it can be
// used in other constexpr contexts) would have every later use of the name
// silently macro-replaced by this file's default, a bug that only shows up
// as an array sized 0 somewhere downstream.

// A complete snapshot of all voice parameters for one render pass.
template <typename VoiceParams>
struct VoiceParamBlockT {
    VoiceParams voices[MAX_VOICES];
    // Zero-size arrays aren't standard C++; engines with FILTER_BUS_COUNT=0
    // (the default) still get one harmless, never-indexed slot.
    FilterBusParams bus[FILTER_BUS_COUNT > 0 ? FILTER_BUS_COUNT : 1];
    EffectParams fx;
};

// Default value for a freshly-initialized voice slot. Generic default is
// value-initialization (all-zero fields). Engines whose "silent" state isn't
// simply all-zero specialize this in their own engine.h.
template <typename VoiceParams>
inline VoiceParams voice_params_default() { return VoiceParams{}; }

// Double-buffered parameter exchange between Core 0 and Core 1.
//
// Core 0 writes to the shadow block: blocks[1 - committed]
// Core 0 commits by flipping committed (single byte, atomic on M0+)
// Core 1 reads blocks[committed] at the start of each render pass.
//
// No locks: Core 0 never touches the committed block, Core 1 never
// touches the shadow block. The index flip is a single-byte store.
template <typename VoiceParams>
struct ParamExchangeT {
    VoiceParamBlockT<VoiceParams> blocks[2];
    volatile uint8_t committed;  // 0 or 1

    void init() {
        committed = 0;
        for (int b = 0; b < 2; b++) {
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                blocks[b].voices[v] = voice_params_default<VoiceParams>();
            }
            // Default: delay selected, ~300 ms (p2=36) / moderate feedback
            // (p1=55), fully dry (mix=0) so it's silent until CC73 opens it.
            blocks[b].fx = { FX_DELAY, 0, 55, 36 };
            // bus[] is left zero-init: FB_OFF == 0, so every bus starts
            // unbound — the correct default (chip.md §5.2: "most voices in
            // real tunes ran unfiltered, because the filter was scarce").
        }
    }

    // Core 0: get the shadow block to write into
    VoiceParamBlockT<VoiceParams> &shadow() {
        return blocks[1 - committed];
    }

    // Core 0: make the shadow visible to Core 1
    void commit() {
        __compiler_memory_barrier();
        committed = 1 - committed;
        __sev();  // wake Core 1 if it's in WFE
    }

    // Core 1: get the currently committed block to read from
    const VoiceParamBlockT<VoiceParams> &active() const {
        return blocks[committed];
    }
};
