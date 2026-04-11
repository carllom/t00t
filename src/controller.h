#pragma once

#include "engine.h"
#include "voice.h"
#include <cstdint>

// Button definitions for Pimoroni Pico VGA Demo board
static constexpr uint32_t NUM_BUTTONS = 3;
static constexpr uint32_t DEBOUNCE_THRESHOLD = 10;  // 10ms at 1ms tick

struct ButtonState {
    uint32_t pin;
    uint8_t channel;
    float freq_hz;
    int16_t amplitude;
    uint8_t counter;    // integrator debounce counter
    bool debounced;     // current debounced state
};

// Initialize button GPIOs and state
void controller_init();

// Call once per 1ms tick. Debounces buttons and applies
// note on/off changes directly to the param shadow block.
void controller_tick(ParamExchange *params);
