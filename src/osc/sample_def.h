#pragma once

#include <cstdint>

// Descriptor for a PCM sample (Q15 int16_t data).
// Pointed to by VoiceParams::sample when waveform == WAVE_SAMPLE.
struct SampleDef {
    const int8_t *data;
    uint32_t num_samples;
    uint32_t sample_rate;  // native playback rate in Hz
    float base_freq;       // frequency the sample plays at native rate
    bool looped;
    uint32_t loop_start;   // sample index
    uint32_t loop_end;     // sample index (exclusive)
};
