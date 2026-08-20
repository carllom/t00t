#include "controller.h"
#include "midi/midi_controller.h"
#include "voice_alloc.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

static ButtonState buttons[NUM_BUTTONS] = {
    //  pin  note (channel, fixed_velocity)  cnt deb
    {   0,   { 69, 0, 100 },  0, false },  // A4 on channel 0 (FAIRLIGHT preset)
    {   6,   { 64, 1, 100 },  0, false },  // E4 on channel 1 (SQUARE_PWM preset)
    {   11,  { 72, 2, 100 },  0, false },  // C5 on channel 2 (SAW_FILTER preset)
};

void controller_init() {
    for (uint32_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(buttons[i].pin);
        gpio_set_dir(buttons[i].pin, GPIO_IN);
        gpio_pull_down(buttons[i].pin);
        buttons[i].counter = 0;
        buttons[i].debounced = false;
    }
}

void controller_tick(ParamExchange *params) {
    // Update voice allocator with latest Core 1 bitmap
    voice_alloc_update();

    bool changed = false;
    VoiceParamBlock &shadow = params->shadow();

    // Sync shadow from committed state so we apply deltas to current truth
    shadow = params->active();

    for (uint32_t i = 0; i < NUM_BUTTONS; i++) {
        ButtonState &b = buttons[i];
        bool raw = gpio_get(b.pin);

        // Integrator debounce: count up when pressed, down when released
        if (raw) {
            if (b.counter < DEBOUNCE_THRESHOLD) b.counter++;
        } else {
            if (b.counter > 0) b.counter--;
        }

        bool new_state = b.debounced;
        if (b.counter >= DEBOUNCE_THRESHOLD) {
            new_state = true;
        } else if (b.counter == 0) {
            new_state = false;
        }

        if (new_state != b.debounced) {
            b.debounced = new_state;

            // Voice resolution is set_note()'s own job (the Voice
            // Allocation Interface) -- this dispatches the same way a MIDI
            // note-on/off would, with value.voice left unresolved.
            SensorEvent ev = sensor_event_button(i, new_state);
            InputValue value = shape_button_event(ev, b.shaping);
            midi_controller_dispatch_note(shadow, b.shaping.note, value);
            changed = true;
        }
    }

    if (changed) {
        params->commit();
    }
}
