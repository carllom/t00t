#include "voice.h"
#include <cmath>

// Sine table generated at startup (avoids flash storage alignment issues)
int16_t sine_table_storage[WAVETABLE_SIZE];
const int16_t *const sine_table_ptr = sine_table_storage;

void voice_init_tables() {
    for (uint32_t i = 0; i < WAVETABLE_SIZE; i++) {
        sine_table_storage[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / (float)WAVETABLE_SIZE));
    }
}

void Voice::init(float freq_hz, int16_t amp) {
    // phase_inc = freq_hz / SAMPLE_RATE * WAVETABLE_SIZE, in fixed-point
    phase_inc = (uint32_t)((freq_hz / (float)SAMPLE_RATE) * (float)WAVETABLE_SIZE * (float)(1 << PHASE_FRAC_BITS));
    phase = 0;
    amplitude = amp;
    active = true;
}

void Voice::render(int32_t *mix_buffer, uint32_t num_samples) {
    if (!active) return;

    for (uint32_t i = 0; i < num_samples; i++) {
        uint32_t idx = (phase >> PHASE_FRAC_BITS) & WAVETABLE_MASK;
        uint32_t frac = phase & ((1 << PHASE_FRAC_BITS) - 1);

        // Linear interpolation between adjacent table entries
        int16_t s0 = sine_table_storage[idx];
        int16_t s1 = sine_table_storage[(idx + 1) & WAVETABLE_MASK];
        int32_t sample = s0 + (((int32_t)(s1 - s0) * (int32_t)frac) >> PHASE_FRAC_BITS);

        // Scale by amplitude and accumulate into mix buffer
        mix_buffer[i] += (sample * amplitude) >> 15;

        phase += phase_inc;
    }
}
