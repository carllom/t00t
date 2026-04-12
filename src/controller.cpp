#include "controller.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

static ButtonState buttons[NUM_BUTTONS] = {
    { 0,  0, 440.00f, 10000, WAVE_SAW_BLEP,    512, 5.0f,  0,     1638, 0,   0, false },  // A: A4 saw BLEP + vibrato
    { 6,  1, 523.25f, 10000, WAVE_SQUARE_BLEP,  512, 3.0f,  0,     0,    256, 0, false },  // B: C5 square BLEP + PWM
    { 11, 2, 659.25f, 10000, WAVE_TRIANGLE,     512, 5.0f,  16000, 0,    0,   0, false },  // C: E5 triangle + tremolo
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
            VoiceParams &vp = shadow.voices[b.channel];
            if (new_state) {
                vp.phase_inc = osc_phase_inc(b.freq_hz);
                vp.amplitude = b.amplitude;
                vp.waveform = b.waveform;
                vp.duty_cycle = b.duty_cycle;
                vp.lfo_rate = osc_phase_inc(b.lfo_hz);
                vp.lfo_depth = b.lfo_depth;
                vp.lfo_pitch_depth = b.lfo_pitch_depth;
                vp.lfo_pwm_depth = b.lfo_pwm_depth;
                vp.trigger++;
                vp.gate = true;
            } else {
                vp.gate = false;
            }
            changed = true;
        }
    }

    if (changed) {
        params->commit();
    }
}
