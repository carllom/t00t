#include "pico/stdlib.h"
#include "output.h"
#include "mixer.h"
#include "voice.h"
#include <cstdio>

static Mixer mixer;

int main() {
    stdio_init_all();
    voice_init_tables();

    AudioOutput *output = create_i2s_output();
    if (!output->init()) {
        panic("audio output init failed");
    }

    mixer.init();
    mixer.add_voice(440.0f, 20000);

    while (true) {
        uint32_t num_samples;
        int16_t *buffer = output->get_buffer(&num_samples);
        mixer.render(buffer, num_samples);
        output->submit_buffer(num_samples);
    }

    return 0;
}
