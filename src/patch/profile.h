#pragma once

#include <cstdint>

// Cook-time measurement for the patch platform, compiled in only with
// T00T_PATCH_PROFILE (`make PATCH_PROFILE=1`). Counts Core 0 clock cycles
// with the M33's DWT cycle counter around cooking a Patch, loading one
// (decode + cook + publish) and editing one parameter (cook + publish), and
// keeps the last and the largest count of each in a global the debug probe
// can read. Without the flag every hook is empty.

struct PatchProfileCounter {
    volatile uint32_t last;
    volatile uint32_t max;
};

struct PatchProfile {
    PatchProfileCounter cook;
    PatchProfileCounter load;
    PatchProfileCounter edit;
};

#if defined(T00T_PATCH_PROFILE) && T00T_PATCH_PROFILE

inline PatchProfile patch_profile;

namespace patch_profile_detail {
inline volatile uint32_t &dwt_ctrl()   { return *reinterpret_cast<volatile uint32_t *>(0xE0001000u); }
inline volatile uint32_t &dwt_cyccnt() { return *reinterpret_cast<volatile uint32_t *>(0xE0001004u); }
inline volatile uint32_t &demcr()      { return *reinterpret_cast<volatile uint32_t *>(0xE000EDFCu); }
}  // namespace patch_profile_detail

inline uint32_t patch_profile_now() {
    using namespace patch_profile_detail;
    if (!(dwt_ctrl() & 1u)) {
        demcr() |= 1u << 24;  // TRCENA: enable the DWT
        dwt_cyccnt() = 0;
        dwt_ctrl() |= 1u;     // CYCCNTENA
    }
    return dwt_cyccnt();
}

inline void patch_profile_record(PatchProfileCounter &c, uint32_t start) {
    uint32_t cycles = patch_profile_now() - start;
    c.last = cycles;
    if (cycles > c.max) c.max = cycles;
}

#define PATCH_PROFILE_START(var) const uint32_t var = patch_profile_now()
#define PATCH_PROFILE_RECORD(counter, var) patch_profile_record(patch_profile.counter, var)

#else

#define PATCH_PROFILE_START(var) ((void)0)
#define PATCH_PROFILE_RECORD(counter, var) ((void)0)

#endif
