#pragma once

#include "../../osc/wavetable.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

// Built-in wavetable bank for the wavetable engine. When `ppg_waves.h` has
// been generated locally (`tools/ppg/convert_ppg_waves.py`, gitignored --
// derived from third-party PPG Wave 2.3 ROM content, see tools/ppg/README.md)
// this uses the real factory waveforms/wavetables verbatim, matching the
// FM module's own patches.h-presence gate (`T00T_WT_HAS_PPG_WAVES`, set by
// CMakeLists.txt's EXISTS check). Otherwise it falls back to a small,
// procedurally-generated stand-in so the engine still builds and plays.

#if defined(T00T_WT_HAS_PPG_WAVES) && T00T_WT_HAS_PPG_WAVES

#include "ppg_waves.h"

static constexpr uint8_t WT_BANK_COUNT = PPG_WAVETABLE_COUNT;
inline WaveTable WT_BANK[WT_BANK_COUNT];
inline char wt_bank_name_buf[WT_BANK_COUNT][8];
inline const char *WT_BANK_NAMES[WT_BANK_COUNT];

// No per-table names survive the ROM extraction (tools/ppg/README.md) --
// the index number is all that's known, so that's all this names them.
inline void wavetable_bank_init() {
    for (uint32_t i = 0; i < WT_BANK_COUNT; i++) {
        WT_BANK[i] = { ppg_wave_data, (uint8_t)PPG_WAVE_COUNT, ppg_wavetable_index[i] };
        snprintf(wt_bank_name_buf[i], sizeof(wt_bank_name_buf[i]), "PPG%02u", i);
        WT_BANK_NAMES[i] = wt_bank_name_buf[i];
    }
}

#else

// No authored PPG-style ROM data available -- procedurally generated at
// init time, a band-limited-additive stand-in, not a claim of matching any
// real machine's actual waves. Run `python3 tools/ppg/convert_ppg_waves.py`
// (see tools/ppg/README.md) to use the real factory data instead.

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

#endif  // T00T_WT_HAS_PPG_WAVES
