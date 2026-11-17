#pragma once

// Minimal stand-in for the ARM C Language Extensions header, providing only
// __ssat (signed saturation to a bit width), which the device DSP code uses
// and AArch64/x86 host compilers don't offer.

#include <cstdint>

static inline int32_t __ssat(int32_t x, uint32_t bits) {
    const int32_t hi = (int32_t)((1u << (bits - 1)) - 1u);
    const int32_t lo = -hi - 1;
    return x > hi ? hi : (x < lo ? lo : x);
}
