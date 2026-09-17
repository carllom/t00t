#pragma once

#include "../audio_common.h"
#include <cstdint>

// Shared wavetable read primitive (module_wavetable.md's Decision Record
// entry 2): a PPG-style single-cycle wavetable, read along two axes --
// ordinary oscillator phase, and a wave-position axis selecting where along
// a small set of authored keyframe waves the read lands. Engine-agnostic,
// like the rest of osc/ -- added here so the format and interpolation math
// can be proven (and heard) before any engine's partial pool depends on it,
// the same "shared component first" precedent res2p.h set for the speech
// module's resonator (engine.md's Host DSP Tooling section).

// Wave size must be a power of two for free masking. 64 samples matches
// real PPG Wave 2.2/2.3 ROM waveforms exactly (tools/ppg/README.md's "ROM
// wavetable format": 64 x 8-bit unsigned PCM per waveform) and keeps a
// keyframe small (128 bytes at Q15) -- short, aliasing single-cycle waves
// are part of the intended PPG-style sound (module_wavetable.md's Decision
// Record entry 5), not a hi-fi table size compromise.
static constexpr uint32_t WT_TABLE_BITS = 6;
static constexpr uint32_t WT_TABLE_SIZE = 1u << WT_TABLE_BITS;
static constexpr uint32_t WT_TABLE_MASK = WT_TABLE_SIZE - 1;

// Interpolation fraction width for the phase axis.
static constexpr uint32_t WT_PHASE_FRAC_BITS = 8;
static constexpr uint32_t WT_PHASE_SHIFT = 32 - WT_TABLE_BITS - WT_PHASE_FRAC_BITS;

// Wave-position axis resolution -- module_wavetable.md's Decision Record
// entry 7: a 64-entry blend index over a small set of authored keyframes,
// rather than one full wave per index entry.
static constexpr uint32_t WT_INDEX_SIZE = 64;
static constexpr uint32_t WT_INDEX_MASK = WT_INDEX_SIZE - 1;

// One wave-position index entry: blend between two stored keyframe waves.
// `blend` is Q8 (0 = pure wave_a, 255 = pure wave_b).
struct WaveTableIndexEntry {
    uint8_t wave_a;
    uint8_t wave_b;
    uint8_t blend;
};

// A wavetable: `num_keyframes` authored waves (each WT_TABLE_SIZE samples,
// Q15, laid out back to back in `keyframes`) plus a WT_INDEX_SIZE-entry
// blend index mapping a wave-position value to a (wave_a, wave_b, blend)
// triple.
struct WaveTable {
    const int16_t *keyframes;
    uint8_t num_keyframes;
    const WaveTableIndexEntry *index;
};

// Q0.32 phase increment for a wavetable partial at a given frequency --
// module_wavetable.md's Decision Record entry 4: a natural power-of-two
// accumulator, distinct from the tracker's Q18.14 grain/PCM format, so
// wrapping needs no mask and tuning resolution is bounded only by float
// precision. Matches the double-promoted-multiply shape already used by
// src/engines/fm/rig.h's fm_rig_phase_inc(), not a hand-rolled variant.
inline uint32_t wavetable_phase_inc(float freq_hz) {
    return (uint32_t)(((double)freq_hz / (double)SAMPLE_RATE) * 4294967296.0);
}

// Nearest-neighbour read of one specific keyframe wave -- no wave-position
// blend, no phase interpolation. Used by the measurement rig's cheapest
// mode (rig.h) and available to a real engine that wants it.
inline int32_t wavetable_read_keyframe_nearest(const WaveTable &wt, uint8_t wave, uint32_t phase) {
    uint32_t idx = phase >> (32 - WT_TABLE_BITS);
    return wt.keyframes[(uint32_t)wave * WT_TABLE_SIZE + idx];
}

// Full bilinear read: phase-axis lerp between two adjacent samples, crossed
// with wave-position-axis lerp between two keyframes selected by `wt.index`.
// Four taps, three lerps -- module_wavetable.md's ~45 c/f planning estimate
// for this kernel. `wave_pos` indexes `wt.index` directly (0..WT_INDEX_SIZE-1,
// masked here so a caller's raw CC-derived value never needs its own clamp).
inline int32_t wavetable_read_bilinear(const WaveTable &wt, uint32_t phase, uint8_t wave_pos) {
    uint32_t shifted = phase >> WT_PHASE_SHIFT;
    uint32_t idx  = (shifted >> WT_PHASE_FRAC_BITS) & WT_TABLE_MASK;
    uint32_t frac = shifted & ((1u << WT_PHASE_FRAC_BITS) - 1);
    uint32_t idx1 = (idx + 1) & WT_TABLE_MASK;

    const WaveTableIndexEntry &e = wt.index[wave_pos & WT_INDEX_MASK];

    auto sample_at = [&](uint32_t i) -> int32_t {
        int32_t a = wt.keyframes[(uint32_t)e.wave_a * WT_TABLE_SIZE + i];
        if (e.wave_a == e.wave_b) return a;
        int32_t b = wt.keyframes[(uint32_t)e.wave_b * WT_TABLE_SIZE + i];
        return a + (((b - a) * (int32_t)e.blend) >> 8);
    };

    int32_t s0 = sample_at(idx);
    int32_t s1 = sample_at(idx1);
    return s0 + (((s1 - s0) * (int32_t)frac) >> WT_PHASE_FRAC_BITS);
}

// Builds a WT_INDEX_SIZE-entry index spanning keyframes [0, num_keyframes-1]
// evenly -- the default index shape when no authored wave-sequence curve is
// supplied (module_wavetable.md's Decision Record entry 7: the blend is
// free at read time regardless of how the index itself was built).
inline void wavetable_build_linear_index(WaveTableIndexEntry *index, uint8_t num_keyframes) {
    for (uint32_t i = 0; i < WT_INDEX_SIZE; i++) {
        float pos = (float)i * (float)(num_keyframes - 1) / (float)(WT_INDEX_SIZE - 1);
        uint8_t a = (uint8_t)pos;
        uint8_t b = (uint8_t)((a + 1 < num_keyframes) ? a + 1 : a);
        float frac = pos - (float)a;
        index[i] = { a, b, (uint8_t)(frac * 255.0f) };
    }
}
