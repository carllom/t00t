#pragma once

#include "audio_common.h"
#include <cstdint>

struct AudioOutput {
    virtual ~AudioOutput() = default;
    virtual bool init() = 0;
    virtual int16_t *get_buffer(uint32_t *num_samples) = 0;
    virtual void submit_buffer(uint32_t num_samples) = 0;
};

AudioOutput *create_i2s_output();
