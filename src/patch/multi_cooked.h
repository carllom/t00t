#pragma once

#include "common.h"
#include "fx/effect_params.h"
#include <cstdint>

// The cooked form of a Multi (multi.h): what Core 1 reads once per buffer
// from the Multi slot to mix the Parts and run the shared FX.

// Unity gain in Q15 is 32768; a 0..127 level maps 127 to exactly unity.
constexpr int32_t level_q15(int32_t v127) { return (v127 * 32768 + 63) / 127; }

// One Part's cooked mix.
struct PartMixCooked {
    int32_t volume_q15;  // 32768 = unity
    int32_t pan_q15;     // -32768 = left, 0 = centre, 32256 = right
    int32_t send_q15;    // FX send, 32768 = all of the Part
};

template <uint32_t N>
struct MultiCookedT {
    EffectParams fx;
    int32_t master_q15;  // 32768 = unity
    PartMixCooked part[N];
};

// Cook a Multi's own settings and its Parts' values into `c`.
template <uint32_t N>
inline void multi_cook(const int32_t (&common)[COMMON_P_COUNT], const int32_t (&part)[N][PART_P_COUNT],
                       MultiCookedT<N> &c) {
    c.fx = EffectParams{ (uint8_t)common[COMMON_P_FX_TYPE], (uint8_t)common[COMMON_P_FX_MIX],
                         (uint8_t)common[COMMON_P_FX_P1], (uint8_t)common[COMMON_P_FX_P2] };
    c.master_q15 = level_q15(common[COMMON_P_MASTER]);
    for (uint32_t p = 0; p < N; p++) {
        c.part[p].volume_q15 = level_q15(part[p][PART_P_VOLUME]);
        c.part[p].pan_q15 = (part[p][PART_P_PAN] - 64) * 512;
        c.part[p].send_q15 = level_q15(part[p][PART_P_SEND]);
    }
}

// The cooked descriptor defaults (FX and mix), which is what Core 1 mixes
// with until the first Multi arrives.
template <uint32_t N>
inline MultiCookedT<N> multi_cooked_default() {
    int32_t common[COMMON_P_COUNT];
    int32_t part[N][PART_P_COUNT];
    for (uint32_t i = 0; i < COMMON_P_COUNT; i++) common[i] = kCommonDescriptors[i].def;
    for (uint32_t p = 0; p < N; p++) {
        for (uint32_t i = 0; i < PART_P_COUNT; i++) part[p][i] = kPartDescriptors[i].def;
    }
    MultiCookedT<N> c{};
    multi_cook(common, part, c);
    return c;
}
