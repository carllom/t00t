#pragma once

#include "audio_common.h"
#include "voice.h"

static constexpr uint32_t MAX_VOICES = 16;

struct Mixer {
    Voice voices[MAX_VOICES];
    int32_t scratch[SAMPLES_PER_BUFFER];
    uint32_t num_active;

    void init();
    Voice *add_voice(float freq_hz, int16_t amplitude);
    void render(int16_t *output, uint32_t num_samples);
};
