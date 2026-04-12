#pragma once

#include "engine.h"
#include "samples.h"

// Reusable voice configuration — shared by buttons, MIDI channels, etc.
// Describes *what* a voice sounds like, not the per-note runtime state.
struct VoicePreset {
    int16_t amplitude;       // base amplitude (0–32767). MIDI may scale by velocity.
    Waveform waveform;
    uint16_t duty_cycle;     // square wave duty (0–1023, 512 = 50%)
    float lfo_rate;          // LFO Hz (0 = off)
    float lfo_depth;         // LFO → amplitude (0.0–1.0)
    float lfo_pitch_depth;   // LFO → pitch (0.0–1.0)
    float lfo_pwm_depth;     // LFO → duty cycle (0.0–1.0)
    FilterMode filter_mode;
    uint16_t filter_cutoff;    // Hz (20–18000)
    uint16_t filter_resonance; // 0–32767
    int16_t filter_env_amount; // envelope → cutoff Hz (±18000)
    float lfo_filter_depth;    // LFO → cutoff Hz (±18000)
    const SampleDef *sample;   // nullptr for non-sample waveforms
};

// Apply a voice preset to VoiceParams (does not touch phase_inc, trigger, gate)
inline void voice_apply_preset(VoiceParams &vp, const VoicePreset &pr) {
    vp.amplitude = pr.amplitude;
    vp.waveform = pr.waveform;
    vp.duty_cycle = pr.duty_cycle;
    vp.lfo_rate = pr.lfo_rate;
    vp.lfo_depth = pr.lfo_depth;
    vp.lfo_pitch_depth = pr.lfo_pitch_depth;
    vp.lfo_pwm_depth = pr.lfo_pwm_depth;
    vp.filter_mode = pr.filter_mode;
    vp.filter_cutoff = pr.filter_cutoff;
    vp.filter_resonance = pr.filter_resonance;
    vp.filter_env_amount = pr.filter_env_amount;
    vp.lfo_filter_depth = pr.lfo_filter_depth;
    vp.sample = pr.sample;
}

// Master preset list — single source of truth for all voice configurations.
// Buttons, MIDI channels, sequences, etc. reference presets by index.

enum PresetId : uint8_t {
    PRESET_FAIRLIGHT,   // 0: Fairlight CMI sample, LP filter
    PRESET_SQUARE_PWM,  // 1: Square BLEP with PWM LFO
    PRESET_SAW_FILTER,  // 2: Saw BLEP with filter LFO
    PRESET_COUNT
};

static const VoicePreset presets[PRESET_COUNT] = {
    // PRESET_FAIRLIGHT: Fairlight sample, LP 400Hz, res 16000, env 8000
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  400, 16000, 8000, 0.0f,    &sararr1_sample },
    // PRESET_SQUARE_PWM: Square BLEP, PWM LFO 3Hz, LP 800Hz, res 20000
    { 10000, WAVE_SQUARE_BLEP, 512, 3.0f, 0.0f, 0.0f, 0.5f, FILTER_LP,  800, 20000, 4000, 0.0f,    nullptr },
    // PRESET_SAW_FILTER: Saw BLEP, filter LFO 2Hz, LP 200Hz, res 24000
    { 10000, WAVE_SAW_BLEP,    512, 2.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  200, 24000, 0,    2000.0f, nullptr },
};
