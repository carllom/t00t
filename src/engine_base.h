#pragma once

#include "audio_common.h"
#include <cstdint>

// Engine-agnostic definitions shared by every synthesis engine and by the
// reusable DSP layer (oscillators, filter, effects, voice allocator).
//
// Engine-specific state — the VoiceParams payload, VoiceParamBlock, and the
// ParamExchange double-buffer — lives in each engine's own engine.h
// (src/engines/<engine>/engine.h), which includes this header.

struct SampleDef;  // forward declaration (defined in osc/sample_def.h)

static constexpr uint32_t MAX_VOICES = 16;

// Profiling pin — GPIO 22 (moved from GPIO 2 to avoid coupling to Button A on GPIO 0)
static constexpr uint32_t PROFILE_PIN = 22;

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
