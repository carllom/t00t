#pragma once

#include "engine.h"
#include "osc/common.h"
#include <cstdint>

// Button definitions for Pimoroni Pico VGA Demo board
static constexpr uint32_t NUM_BUTTONS = 3;
static constexpr uint32_t DEBOUNCE_THRESHOLD = 10;  // 10ms at 1ms tick

struct ButtonState {
    uint32_t pin;
    int16_t amplitude;
    Waveform waveform;
    uint16_t duty_cycle;     // duty cycle for square (0–1023, 512 = 50%)
    float lfo_hz;            // LFO rate in Hz (0 = off)
    float lfo_depth;         // LFO → amplitude depth (0.0–1.0)
    float lfo_pitch_depth;   // LFO → pitch depth (0.0–1.0, 0.05 ≈ ±1 semitone)
    float lfo_pwm_depth;     // LFO → duty cycle depth (0.0–1.0, fraction of range)
    // Filter
    FilterMode filter_mode;
    uint16_t filter_cutoff;    // base cutoff Hz
    uint16_t filter_resonance; // 0–32767
    int16_t filter_env_amount; // envelope → cutoff Hz
    float lfo_filter_depth;    // LFO → cutoff Hz
    const SampleDef *sample;   // sample definition (nullptr for non-sample waveforms)
    // Note cycling
    const float *notes;      // pointer to note frequency table
    uint8_t num_notes;       // number of notes in table
    uint8_t note_index;      // current index into notes (cycles on press)
    int8_t allocated_voice;  // voice slot from allocator (-1 = none)
    uint8_t counter;         // integrator debounce counter
    bool debounced;          // current debounced state
};

// Initialize button GPIOs and state
void controller_init();

// Call once per 1ms tick. Debounces buttons and applies
// note on/off changes via the voice allocator.
void controller_tick(ParamExchange *params);
