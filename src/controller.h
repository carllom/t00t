#pragma once

#include "engine.h"
#include <cstdint>

// Button definitions for Pimoroni Pico VGA Demo board
static constexpr uint32_t NUM_BUTTONS = 3;
static constexpr uint32_t DEBOUNCE_THRESHOLD = 10;  // 10ms at 1ms tick

struct ButtonState {
    uint32_t pin;
    uint8_t preset_id;       // index into master preset list
    // Note cycling
    const float *notes;
    uint8_t num_notes;
    uint8_t note_index;
    int8_t allocated_voice;
    uint8_t counter;
    bool debounced;
};

// Initialize button GPIOs and state
void controller_init();

// Call once per 1ms tick. Debounces buttons and applies
// note on/off changes via the voice allocator.
void controller_tick(ParamExchange *params);
