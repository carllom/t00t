#pragma once

#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;
static constexpr uint32_t FILTER_BUS_COUNT = 0;   // chip module only (sid.md §5)

#include "engine_base.h"

// Subtractive engine — the general-purpose synth (ADSR + LFO + SVF + osc
// dispatch). Shared enums/constants (Waveform, FilterMode, EffectParams,
// PROFILE_PIN) and the VoiceParamBlockT/ParamExchangeT mechanism live in
// engine_base.h.

// Voice parameters: written by Core 0, read by Core 1.
// Only contains values needed for synthesis — no phase state.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point phase increment (pre-computed by Core 0)
    int16_t amplitude;   // base amplitude / velocity (0–32767)
    uint8_t trigger;     // generation counter, incremented on each note-on
    bool gate;           // true while key held, false on release
    Waveform waveform;   // oscillator waveform type
    uint16_t duty_cycle;  // duty cycle for square wave (0–1023, 512 = 50%)
    float lfo_rate;      // LFO frequency in Hz (0 = off)
    float lfo_depth;     // LFO → amplitude depth (0.0–1.0, 0 = off)
    float lfo_pitch_depth; // LFO → pitch depth (0.0–1.0, 0.05 ≈ ±1 semitone)
    float lfo_pwm_depth;   // LFO → duty cycle depth (0.0–1.0, fraction of full range)
    // Filter
    FilterMode filter_mode;    // filter type (OFF = bypass)
    uint16_t filter_cutoff;    // base cutoff in Hz (20–18000)
    uint16_t filter_resonance; // resonance 0–32767 (0 = none, 32767 = self-oscillation)
    int16_t filter_env_amount; // envelope → cutoff in Hz (signed, ±18000)
    float lfo_filter_depth;    // LFO → cutoff in Hz (signed, ±18000)
    const SampleDef *sample;   // sample definition (nullptr for non-sample waveforms)
    int16_t mod_depth;         // mod-wheel vibrato depth, Q15 (0 = off) — dedicated LFO on Core 1
    int16_t pan;                // Q15 pan: -32768 = full left, 0 = center, 32767 = full right (CC10)
};

// Default voice: audible-ready sine at a sane cutoff, everything else off,
// centered pan.
template <>
inline VoiceParams voice_params_default<VoiceParams>() {
    return { 0, 0, 0, false, WAVE_SINE, 512,
             0.0f, 0.0f, 0.0f, 0.0f,
             FILTER_OFF, 8000, 0, 0, 0.0f, nullptr, 0, 0 };
}

using VoiceParamBlock = VoiceParamBlockT<VoiceParams>;
using ParamExchange = ParamExchangeT<VoiceParams>;
