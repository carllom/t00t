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

// A complete snapshot of all voice parameters for one render pass.
template <typename VoiceParams>
struct VoiceParamBlockT {
    VoiceParams voices[MAX_VOICES];
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
