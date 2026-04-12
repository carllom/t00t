#pragma once

#include <cstdint>

// LFSR noise oscillator: 16-bit Galois LFSR.
// Phase is ignored for spectrum, but the LFSR state must be persistent.
// The caller passes a reference to the LFSR state so it persists across calls.
// Taps: bits 16, 14, 13, 11 (polynomial 0xB400).
inline int32_t osc_noise(uint16_t &lfsr) {
    uint16_t bit = lfsr & 1u;
    lfsr >>= 1;
    if (bit) {
        lfsr ^= 0xB400u;
    }
    // Map 16-bit LFSR state to [-32767..32767]
    return (int32_t)lfsr - 32768;
}
