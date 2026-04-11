#pragma once

#include <cstdint>

enum class ControlEvent : uint8_t {
    NONE,
    NOTE_ON,
    NOTE_OFF,
};

struct ControlMessage {
    ControlEvent event;
    uint8_t channel;     // voice/note slot
    float freq_hz;       // for NOTE_ON
    int16_t amplitude;   // for NOTE_ON
};

static constexpr uint32_t MAX_CONTROL_MESSAGES = 16;

struct Controller {
    virtual ~Controller() = default;
    virtual void init() = 0;
    virtual uint32_t poll(ControlMessage *messages, uint32_t max_messages) = 0;
};

Controller *create_button_controller();
