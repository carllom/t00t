#pragma once

#include "square.h"
#include <cstdint>

// 808 "metal" oscillator bank: six 50%-duty square oscillators at fixed
// inharmonic frequencies, summed. This clangy, atonal cluster is the basis of
// the TR-808 hi-hats and cymbals (and, with a 2-osc subset, the cowbell).
// Band-pass + high-pass filtering downstream shapes it into hat / cymbal tones.
//
// The six frequencies are the classic 808 metal-oscillator set (Hz). They are
// fixed — the hi-hat is not pitch-tracked; character comes from the filtering
// and decay, not the note.
static constexpr float METAL_FREQS[6] = {
    205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f
};
static constexpr int METAL_OSC_COUNT = 6;

// Sum `count` squares from their phase accumulators. Caller advances the
// phases. Output ~[-32767, 32767] (count ±32767 squares summed, then divided by
// count). Hats/cymbal use all six; the cowbell uses a two-oscillator subset.
inline int32_t osc_metal(const uint32_t *phase, int count) {
    int32_t sum = 0;
    for (int k = 0; k < count; k++) sum += osc_square(phase[k], 512);
    return count > 0 ? sum / count : 0;
}
