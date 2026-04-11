#include "controller.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Pimoroni Pico VGA Demo board buttons
static constexpr uint BUTTON_A_PIN = 0;
static constexpr uint BUTTON_B_PIN = 6;
static constexpr uint BUTTON_C_PIN = 11;

static constexpr uint NUM_BUTTONS = 3;

struct ButtonDef {
    uint pin;
    uint8_t channel;
    float freq_hz;
    int16_t amplitude;
};

static const ButtonDef button_defs[NUM_BUTTONS] = {
    { BUTTON_A_PIN, 0, 440.00f, 10000 },  // A4
    { BUTTON_B_PIN, 1, 523.25f, 10000 },  // C5
    { BUTTON_C_PIN, 2, 659.25f, 10000 },  // E5
};

struct ButtonController : Controller {
    bool prev_state[NUM_BUTTONS];

    void init() override {
        for (uint32_t i = 0; i < NUM_BUTTONS; i++) {
            gpio_init(button_defs[i].pin);
            gpio_set_dir(button_defs[i].pin, GPIO_IN);
            gpio_pull_down(button_defs[i].pin);
            prev_state[i] = false;
        }
    }

    uint32_t poll(ControlMessage *messages, uint32_t max_messages) override {
        uint32_t count = 0;

        for (uint32_t i = 0; i < NUM_BUTTONS && count < max_messages; i++) {
            // Buttons are active high with pull-down
            bool pressed = gpio_get(button_defs[i].pin);

            if (pressed && !prev_state[i]) {
                messages[count++] = {
                    .event = ControlEvent::NOTE_ON,
                    .channel = button_defs[i].channel,
                    .freq_hz = button_defs[i].freq_hz,
                    .amplitude = button_defs[i].amplitude,
                };
            } else if (!pressed && prev_state[i]) {
                messages[count++] = {
                    .event = ControlEvent::NOTE_OFF,
                    .channel = button_defs[i].channel,
                    .freq_hz = 0,
                    .amplitude = 0,
                };
            }

            prev_state[i] = pressed;
        }

        return count;
    }
};

static ButtonController button_instance;

Controller *create_button_controller() {
    return &button_instance;
}
