#pragma once

#include "../../osc/wavetable.h"
#include <cmath>
#include <cstdint>

// Built-in wavetable bank for the wavetable engine skeleton. No authored
// PPG-style ROM data exists in this repo (module_wavetable.md's
// Specifications: "no code exists" until this skeleton), so both tables
// here are procedurally generated at init time -- a band-limited-additive
// stand-in, not a claim of matching any real machine's actual waves.

static constexpr uint8_t WT_KEYFRAMES = 8;
static constexpr uint8_t WT_BANK_COUNT = 2;

inline int16_t wt_bank_data[WT_BANK_COUNT][WT_KEYFRAMES * WT_TABLE_SIZE];
inline WaveTableIndexEntry wt_bank_index[WT_BANK_COUNT][WT_INDEX_SIZE];
inline WaveTable WT_BANK[WT_BANK_COUNT];
inline const char *WT_BANK_NAMES[WT_BANK_COUNT] = { "SOFT MORPH", "BRIGHT MORPH" };

// Fills one keyframe table with a partial-sum sawtooth whose harmonic count
// rises linearly across keyframes 0..WT_KEYFRAMES-1 -- keyframe 0 is close
// to a pure sine, the last keyframe is the richest/brightest.
inline void wt_gen_harmonic_morph(int16_t *out, uint8_t max_harmonics) {
    for (uint32_t k = 0; k < WT_KEYFRAMES; k++) {
        uint32_t harmonics = 1 + (k * (max_harmonics - 1)) / (WT_KEYFRAMES - 1);
        for (uint32_t i = 0; i < WT_TABLE_SIZE; i++) {
            float phase = 2.0f * (float)M_PI * (float)i / (float)WT_TABLE_SIZE;
            float acc = 0.0f;
            for (uint32_t h = 1; h <= harmonics; h++) {
                acc += sinf(phase * (float)h) / (float)h;
            }
            acc /= 1.7f;  // partial-sum peak grows with harmonic count -- headroom
            if (acc > 1.0f) acc = 1.0f;
            if (acc < -1.0f) acc = -1.0f;
            out[k * WT_TABLE_SIZE + i] = (int16_t)(acc * 32767.0f);
        }
    }
}

inline void wavetable_bank_init() {
    wt_gen_harmonic_morph(wt_bank_data[0], 4);
    wavetable_build_linear_index(wt_bank_index[0], WT_KEYFRAMES);
    WT_BANK[0] = { wt_bank_data[0], WT_KEYFRAMES, wt_bank_index[0] };

    wt_gen_harmonic_morph(wt_bank_data[1], 16);
    wavetable_build_linear_index(wt_bank_index[1], WT_KEYFRAMES);
    WT_BANK[1] = { wt_bank_data[1], WT_KEYFRAMES, wt_bank_index[1] };
}
