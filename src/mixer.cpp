#include "mixer.h"
#include <cstring>
#include <algorithm>

void Mixer::init() {
    num_active = 0;
    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        voices[i].active = false;
    }
}

Voice *Mixer::add_voice(float freq_hz, int16_t amplitude) {
    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) {
            voices[i].init(freq_hz, amplitude);
            num_active++;
            return &voices[i];
        }
    }
    return nullptr;
}

void Mixer::render(int16_t *output, uint32_t num_samples) {
    // Clear scratch buffer
    memset(scratch, 0, num_samples * sizeof(int32_t));

    // Accumulate all active voices
    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        voices[i].render(scratch, num_samples);
    }

    // Clip to int16_t range and write stereo interleaved output
    for (uint32_t i = 0; i < num_samples; i++) {
        int32_t s = scratch[i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        int16_t out = (int16_t)s;
        output[i * 2 + 0] = out; // left
        output[i * 2 + 1] = out; // right
    }
}
