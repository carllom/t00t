#pragma once

#include "../engine.h"
#include "sine.h"
#include "square.h"

// Dispatch to the correct oscillator based on waveform type.
// All oscillators share the same interface: phase in, sample out.
// Inline so the compiler sees through the switch in the hot loop.
inline int32_t osc_sample(Waveform waveform, uint32_t phase) {
    switch (waveform) {
        case WAVE_SQUARE: return osc_square(phase);
        default:          return osc_sine(phase);
    }
}
