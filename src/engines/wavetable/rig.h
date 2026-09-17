#pragma once

#include <cmath>
#include <cstdint>

#include "../../osc/wavetable.h"

// Wavetable/granular module measurement rig (module_wavetable.md): a
// stripped partial mixer -- N partials, fixed increments, fixed
// wave-positions, no envelope, no modulation, no MIDI, no partial-pool
// allocation. This produces the numbers module_wavetable.md's own
// Future/TODO names as the required next step ("the thing that actually
// decides MAX_PARTIALS"); it does not decide anything itself (this file is
// a separate, self-contained rig, not a modification of the real engine
// skeleton in this same directory).
//
// Every lever is a compile-time switch (Makefile/CMake), same convention as
// src/engines/fm/rig.h's FM_RIG_* / src/engines/chip/rig.h's CHIP_RIG_*
// levers -- the value under test is cycles per frame, and a runtime switch
// would put a branch inside the loop being measured:
//
//   WT_RIG_PARTIALS  -- partial count (default comfortably past
//                        module_wavetable.md's ~28-40 planning estimate)
//   WT_RIG_BLOCK     -- sub-block length: 8 / 16 / 32 / 64
//   WT_RIG_MODE      -- 0: nearest (phase nearest, wave nearest,
//                           wavetable_read_keyframe_nearest())
//                        1: bilinear (phase lerp x wave-position lerp,
//                           wavetable_read_bilinear()) -- default
//                        2: bilinear + window -- one more masked table read
//                           per sample, module_wavetable.md's "Window is a
//                           second masked table read", standing in for a
//                           grain's amplitude envelope without this rig
//                           needing real grain lifetime/allocation

#ifndef WT_RIG_PARTIALS
#define WT_RIG_PARTIALS 48
#endif
#ifndef WT_RIG_BLOCK
#define WT_RIG_BLOCK 16
#endif
#ifndef WT_RIG_MODE
#define WT_RIG_MODE 1
#endif

// The host build never defines the real pico-sdk macro; device builds pull
// it in from pico/platform.h before this header. Same fallback pattern as
// src/engines/fm/rig.h and src/engines/tracker/mixer.h.
#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

// Own keyframe table + index, independent of tables.h's real-engine data --
// this rig's wave content doesn't matter, only that reading it costs what a
// real one would (same WT_TABLE_SIZE/WT_INDEX_SIZE, osc/wavetable.h).
inline int16_t wt_rig_keyframes[4 * WT_TABLE_SIZE];
inline WaveTableIndexEntry wt_rig_index[WT_INDEX_SIZE];
inline WaveTable wt_rig_table;

// Window lookup for WT_RIG_MODE==2 -- a plain masked table read, same shape
// as a real grain's amplitude window, so its cost is representative even
// though this rig has no real grain lifetime/allocation.
inline int16_t wt_rig_window[WT_TABLE_SIZE];

inline void wt_rig_init_tables() {
    for (uint32_t k = 0; k < 4; k++) {
        for (uint32_t i = 0; i < WT_TABLE_SIZE; i++) {
            float phase = 2.0f * (float)M_PI * (float)i / (float)WT_TABLE_SIZE;
            wt_rig_keyframes[k * WT_TABLE_SIZE + i] =
                (int16_t)(32767.0f * sinf(phase * (float)(k + 1)) / (float)(k + 1));
        }
    }
    wavetable_build_linear_index(wt_rig_index, 4);
    wt_rig_table = { wt_rig_keyframes, 4, wt_rig_index };

    for (uint32_t i = 0; i < WT_TABLE_SIZE; i++) {
        float t = (float)i / (float)(WT_TABLE_SIZE - 1);
        wt_rig_window[i] = (int16_t)(32767.0f * sinf((float)M_PI * t));  // half-sine window
    }
}

struct WtRigPartial {
    uint32_t phase;      // Q0.32, osc/wavetable.h's format
    uint32_t inc;
    uint8_t  wave_pos;
    uint32_t win_phase;  // grain window position, WT_RIG_MODE==2 only -- reuses the same Q0.32 shape
    uint32_t win_inc;
};

// Fixed pitches spanning several octaves plus a spread of wave-positions
// and (for mode 2) window rates -- not a plain single tone, worth something
// for a by-eye/by-ear sanity check, same reasoning as fm_rig_init_voice().
inline void wt_rig_init_partial(WtRigPartial &p, float freq_hz, uint8_t wave_pos, float grain_hz) {
    p.phase = 0;
    p.inc = wavetable_phase_inc(freq_hz);
    p.wave_pos = wave_pos;
    p.win_phase = 0;
    p.win_inc = wavetable_phase_inc(grain_hz);
}

// Renders `num_partials` (<= WT_RIG_PARTIALS) partials, WT_RIG_BLOCK frames
// at a time, summed equally into both channels (no pan, no amplitude ramp --
// this rig doesn't need a stereo image or envelope, just a real per-sample
// cost). Sub-blocked the same shape a real engine's sub-block ramping would
// use, even though nothing here actually varies per sub-block yet.
inline void __not_in_flash_func(wt_rig_render_buffer)(WtRigPartial (&partials)[WT_RIG_PARTIALS],
                                                        uint32_t num_partials,
                                                        int32_t *dry_l, int32_t *dry_r, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) { dry_l[i] = 0; dry_r[i] = 0; }

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > WT_RIG_BLOCK) n = WT_RIG_BLOCK;

        for (uint32_t p = 0; p < num_partials; p++) {
            WtRigPartial &part = partials[p];
            uint32_t phase = part.phase;
            uint32_t win_phase = part.win_phase;
            for (uint32_t i = 0; i < n; i++) {
#if WT_RIG_MODE == 0
                int32_t s = wavetable_read_keyframe_nearest(
                    wt_rig_table, wt_rig_table.index[part.wave_pos].wave_a, phase);
#else
                int32_t s = wavetable_read_bilinear(wt_rig_table, phase, part.wave_pos);
#endif
#if WT_RIG_MODE == 2
                uint32_t widx = win_phase >> (32 - WT_TABLE_BITS);
                s = (s * (int32_t)wt_rig_window[widx]) >> 15;
                win_phase += part.win_inc;
#endif
                dry_l[done + i] += s;
                dry_r[done + i] += s;
                phase += part.inc;
            }
            part.phase = phase;
            part.win_phase = win_phase;
        }
        done += n;
    }
}
