#include "mixer.h"
#include <cstring>
#include <algorithm>

void Mixer::init() {
    num_active = 0;
    for (uint32_t i = 0; i < MAX_VOICES; i++) {
        voices[i].active = false;
    }
}

void Mixer::note_on(uint8_t channel, float freq_hz, int16_t amplitude) {
    if (channel >= MAX_VOICES) return;
    if (!voices[channel].active) num_active++;
    voices[channel].init(freq_hz, amplitude);
}

void Mixer::note_off(uint8_t channel) {
    if (channel >= MAX_VOICES) return;
    if (voices[channel].active) {
        voices[channel].active = false;
        num_active--;
    }
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
