#pragma once

#include "tract.h"
#include <cstdint>

// Packed, flash-resident phoneme table entry (#32, speech.md "Host Tooling").
// Byte-quantized so a 64-phoneme table stays negligible in flash: F is Hz
// (fits every formant this engine will ever target), B/fric_B/nasal_B are
// Hz/4 (0-1020 Hz representable in a uint8_t -- the bandwidths this engine
// uses sit in the tens-to-low-hundreds range, comfortably inside that), and
// av/af/an are 0-255 mapping onto FormantTarget's 0.0-1.0 excitation mix.
// duration_ms and flags are read by nothing yet (P3's segment sequencer,
// unbuilt) -- reserved the same way #30 reserved the SUSTAINABLE bit ahead
// of a CSV existing at all.
//
// This is speech.md's "Generated data" PhonemeDef struct, extended with
// fric_F/fric_B/nasal_F/nasal_B: that struct predates #29's parallel
// fricative/nasal branches, which need a per-phoneme fricative/nasal pole
// (e.g. /s/ vs /sh/'s fric_F is their entire distinguishing feature) that
// the original 16-byte sketch had no field for. Still negligible in flash
// (sizeof(PhonemeDef) below) at any phoneme count this engine will reach.
//
// Field order (uint16_t block before the uint8_t block) avoids alignment
// padding -- sizeof(PhonemeDef) is exactly 26 bytes, not 26 rounded up.
struct PhonemeDef {
    uint16_t F[SPEECH_FORMANTS];   // Hz
    uint16_t fric_F;                // Hz
    uint16_t nasal_F;               // Hz
    uint8_t  B[SPEECH_FORMANTS];   // Hz / 4, range 0-1020 Hz
    uint8_t  fric_B;                 // Hz / 4
    uint8_t  nasal_B;                // Hz / 4
    uint8_t  av, af, an;              // 0-255, maps onto FormantTarget's 0.0-1.0
    uint8_t  duration_ms;
    uint8_t  flags;
};

// speech.md "Plosives" / "Singing mode (#30 decision)": bit meanings fixed
// by the doc, not by this generator -- speechgen.py validates CSV flag
// names against these same constants so a typo fails the build instead of
// silently clearing a bit.
static constexpr uint8_t PHONEME_FLAG_PLOSIVE         = 0x01;
static constexpr uint8_t PHONEME_FLAG_STOP_CLOSURE     = 0x02;
static constexpr uint8_t PHONEME_FLAG_TRANSITION_FAST  = 0x04;
static constexpr uint8_t PHONEME_FLAG_SUSTAINABLE      = 0x08;  // #30, unread until P4

// Expands a packed table entry into the float FormantTarget tract_retrigger()/
// tract_set_target() actually consume. Every PHONEME_TARGETS[] access needs
// this -- the packed form only exists to be small in flash, nothing renders
// from it directly. duration_ms/flags aren't part of a render target (they're
// P3 sequencer data, not per-sample DSP state) so they don't appear in the
// unpacked result; read them straight off the PhonemeDef if you need them.
inline FormantTarget phoneme_unpack(const PhonemeDef &d) {
    FormantTarget t;
    for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) {
        t.F[i] = (float)d.F[i];
        t.B[i] = (float)d.B[i] * 4.0f;
    }
    t.fric_F = (float)d.fric_F;
    t.fric_B = (float)d.fric_B * 4.0f;
    t.nasal_F = (float)d.nasal_F;
    t.nasal_B = (float)d.nasal_B * 4.0f;
    t.av = (float)d.av * (1.0f / 255.0f);
    t.af = (float)d.af * (1.0f / 255.0f);
    t.an = (float)d.an * (1.0f / 255.0f);
    return t;
}
