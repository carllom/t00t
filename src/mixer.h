#pragma once

#include "audio_common.h"
#include "voice.h"

static constexpr uint32_t MAX_VOICES = 16;

struct Mixer {
    Voice voices[MAX_VOICES];
    int32_t scratch[SAMPLES_PER_BUFFER];
    uint32_t num_active;

    void init();
    void note_on(uint8_t channel, float freq_hz, int16_t amplitude);
    void note_off(uint8_t channel);
    void render(int16_t *output, uint32_t num_samples);
};
