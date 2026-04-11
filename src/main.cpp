#include "pico/stdlib.h"
#include "output.h"
#include "mixer.h"
#include "controller.h"
#include <cstdio>

static Mixer mixer;

int main() {
    stdio_init_all();
    voice_init_tables();

    AudioOutput *output = create_i2s_output();
    if (!output->init()) {
        panic("audio output init failed");
    }

    Controller *ctrl = create_button_controller();
    ctrl->init();

    mixer.init();

    while (true) {
        // Poll controller for note events
        ControlMessage messages[MAX_CONTROL_MESSAGES];
        uint32_t msg_count = ctrl->poll(messages, MAX_CONTROL_MESSAGES);
        for (uint32_t i = 0; i < msg_count; i++) {
            switch (messages[i].event) {
                case ControlEvent::NOTE_ON:
                    mixer.note_on(messages[i].channel, messages[i].freq_hz, messages[i].amplitude);
                    break;
                case ControlEvent::NOTE_OFF:
                    mixer.note_off(messages[i].channel);
                    break;
                default:
                    break;
            }
        }

        uint32_t num_samples;
        int16_t *buffer = output->get_buffer(&num_samples);
        mixer.render(buffer, num_samples);
        output->submit_buffer(num_samples);
    }

    return 0;
}
