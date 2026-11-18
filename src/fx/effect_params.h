#pragma once

#include <cstdint>

// Effect selector. A CC picks the type (CC74; CC76 on subtractive, where
// CC74 is cutoff); the same three knobs (CC72/73/75) then drive whichever
// effect is active. The values go out in Patch-platform stored Multis and
// the SysEx descriptor dump, so they are permanent: append only.
enum EffectType : uint8_t { FX_OFF, FX_DELAY, FX_REVERB, FX_PHASER, FX_FLANGER, FX_CHORUS, FX_BITCRUSHER, FX_OVERDRIVE, FX_COUNT };

// Global effect parameters. Written by Core 0, read by Core 1. The three params
// are raw 0..127 controller values; each effect maps them to its own scale.
struct EffectParams {
    uint8_t type;   // EffectType (FX type CC)
    uint8_t mix;    // CC73: wet/dry — 0 = dry, 127 = full wet
    uint8_t p1;     // CC72: delay feedback / reverb room size / phaser+flanger+chorus rate /
                    //       bitcrusher bit depth / overdrive drive
    uint8_t p2;     // CC75: delay time  / reverb damping / phaser+flanger+chorus depth /
                    //       bitcrusher sample rate / overdrive tone
};
