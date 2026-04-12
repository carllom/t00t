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
    int16_t lfo_depth;       // LFO → amplitude depth (0–32767)
    int16_t lfo_pitch_depth; // LFO → pitch depth (0–32767)
    int16_t lfo_pwm_depth;   // LFO → duty cycle depth (0–512)
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
