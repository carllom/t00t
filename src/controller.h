#pragma once

#include "engine.h"
#include "button_shaping.h"
#include <cstdint>

// Button definitions for Pimoroni Pico VGA Demo board
static constexpr uint32_t NUM_BUTTONS = 3;
static constexpr uint32_t DEBOUNCE_THRESHOLD = 10;  // 10ms at 1ms tick

struct ButtonState {
    uint32_t pin;
    ButtonShapingConfig shaping;  // note/channel/velocity this button plays
    uint8_t counter;
    bool debounced;
};

// Initialize button GPIOs and state
void controller_init();

// Call once per 1ms tick. Debounces buttons and applies
// note on/off changes via the voice allocator.
void controller_tick(ParamExchange *params);
