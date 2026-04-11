#include "voice.h"
#include <cmath>

int16_t sine_table[WAVETABLE_SIZE];

void voice_init_tables() {
    for (uint32_t i = 0; i < WAVETABLE_SIZE; i++) {
        sine_table[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / (float)WAVETABLE_SIZE));
    }
}
