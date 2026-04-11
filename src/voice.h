#pragma once

#include "audio_common.h"

// Wavetable size must be power of 2 for fast masking
static constexpr uint32_t WAVETABLE_SIZE = 1024;
static constexpr uint32_t WAVETABLE_MASK = WAVETABLE_SIZE - 1;

// Fixed-point phase: 22.10 format (22 integer bits, 10 fractional bits)
// Integer part indexes into wavetable, fractional part for interpolation
static constexpr uint32_t PHASE_FRAC_BITS = 10;

struct Voice {
    uint32_t phase;      // fixed-point phase accumulator
    uint32_t phase_inc;  // fixed-point phase increment per sample
    int16_t amplitude;   // output amplitude (0-32767)
    bool active;

    void init(float freq_hz, int16_t amp);
    void render(int32_t *mix_buffer, uint32_t num_samples);
};

// Pre-computed sine wavetable
extern const int16_t sine_table[WAVETABLE_SIZE];

void voice_init_tables();
