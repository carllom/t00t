#include "controller.h"
#include "voice_alloc.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// Note tables for cycling through on each keypress
static const float notes_a[] = { 440.00f, 493.88f, 554.37f, 587.33f };  // A4 B4 C#5 D5
static const float notes_b[] = { 329.63f, 369.99f, 392.00f, 440.00f };  // E4 F#4 G4  A4
static const float notes_c[] = { 523.25f, 587.33f, 659.25f, 739.99f };  // C5 D5  E5  F#5

static ButtonState buttons[NUM_BUTTONS] = {
    //  pin  amp    waveform          duty lfo_hz depth pitch pwm  notes      num idx voice cnt deb
    {   0,   10000, WAVE_SAW_BLEP,    512, 5.0f,  0,    1638, 0,   notes_a,   4,  0,  -1,   0, false },
    {   6,   10000, WAVE_SQUARE_BLEP,  512, 3.0f,  0,    0,    256, notes_b,   4,  0,  -1,   0, false },
    {   11,  10000, WAVE_TRIANGLE,     512, 5.0f,  16000,0,    0,   notes_c,   4,  0,  -1,   0, false },
};

void controller_init() {
    for (uint32_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(buttons[i].pin);
        gpio_set_dir(buttons[i].pin, GPIO_IN);
        gpio_pull_down(buttons[i].pin);
        buttons[i].counter = 0;
        buttons[i].debounced = false;
        buttons[i].note_index = 0;
        buttons[i].allocated_voice = -1;
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
            if (new_state) {
                // Note on: allocate a voice and cycle to next note
                float freq = b.notes[b.note_index];
                b.note_index = (b.note_index + 1) % b.num_notes;

                int v = voice_alloc_allocate();
                if (v >= 0) {
                    VoiceParams &vp = shadow.voices[v];
                    vp.phase_inc = osc_phase_inc(freq);
                    vp.amplitude = b.amplitude;
                    vp.waveform = b.waveform;
                    vp.duty_cycle = b.duty_cycle;
                    vp.lfo_rate = osc_phase_inc(b.lfo_hz);
                    vp.lfo_depth = b.lfo_depth;
                    vp.lfo_pitch_depth = b.lfo_pitch_depth;
                    vp.lfo_pwm_depth = b.lfo_pwm_depth;
                    vp.trigger++;
                    vp.gate = true;
                    b.allocated_voice = (int8_t)v;
                }
            } else {
                // Note off: release the allocated voice
                if (b.allocated_voice >= 0) {
                    shadow.voices[b.allocated_voice].gate = false;
                    voice_alloc_release(b.allocated_voice);
                    b.allocated_voice = -1;
                }
            }
            changed = true;
        }
    }

    if (changed) {
        params->commit();
    }
}
