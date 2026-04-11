#pragma once

#include "audio_common.h"
#include <cstdint>

// Two raw stereo buffers: 256 samples × 2 channels × int16_t = 1024 bytes each.
// DMA plays one while Core 1 fills the other.
struct AudioBuffers {
    int16_t data[2][SAMPLES_PER_BUFFER * 2];  // [buffer_index][sample]
};

// Initialize PIO I2S + DMA with interrupt-driven buffer swap.
// On DMA completion, the ISR starts DMA on the next buffer and pushes
// the index of the buffer needing fill to Core 1 via multicore FIFO.
void i2s_output_init(AudioBuffers *buffers);

// Get pointer to a specific buffer for filling.
inline int16_t *i2s_buffer_ptr(AudioBuffers *buffers, uint8_t index) {
    return buffers->data[index];
}
