#pragma once

#include "patch.h"

// A small set of hand-authored OPL2 test patches -- this pass's stand-in for
// a real bank converter (out of scope here; a future ticket may add one the
// same way tools/syx2patch.py arrived for FM well after its own first
// hardcoded patch). Checked in directly (not generated/gitignored), always
// compiled in, selectable by index over MIDI.
//
// TL is 0 = loudest, 63 = quietest (0.75 dB/step); on a modulator, a lower
// TL means a deeper modulation index (brighter/harsher), not just "louder."

// FM chain: op0 (modulator, TL 14, mult 1) drives op1 (carrier, TL 0, mult
// 1) -- a plain 1:1 pair, the simplest FM timbre this engine can play.
// Moderate feedback (3) roughens the modulator's own waveform before it
// reaches op1. Sustain mode on both operators: held notes stay at their
// sustain level rather than dying out on their own.
inline constexpr OplPatch OPL_PATCH_LEAD = {
    "OPL LEAD",
    {
        /* op0 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/14, /*ar*/15, /*dr*/10, /*sl*/3,  /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 (car) */ { /*mult*/1, /*ksl*/0, /*tl*/0,  /*ar*/15, /*dr*/9,  /*sl*/2,  /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
    },
    /* feedback */ 3,
    OPL_ALGO_FM,
};

// FM chain, percussive: op0 (mult 7, an inharmonic ratio against op1's own
// pitch) modulates op1 through heavy feedback (5) for a hard, metallic edge.
// Both operators are percussive (egt=false), so a held note dies away on its
// own rather than sustaining -- a bell, not an organ. op0's decay rate is
// higher (faster) than op1's so the metallic brightness fades out first,
// leaving a longer, plainer ring from op1 alone, same shape a real bell's
// strike-to-ring transition has.
inline constexpr OplPatch OPL_PATCH_BELL = {
    "OPL BELL",
    {
        /* op0 (mod) */ { /*mult*/7, /*ksl*/1, /*tl*/10, /*ar*/15, /*dr*/3, /*sl*/15, /*rr*/9, /*egt*/false, /*ksr*/true, /*ws*/2 },
        /* op1 (car) */ { /*mult*/1, /*ksl*/1, /*tl*/2,  /*ar*/15, /*dr*/1, /*sl*/15, /*rr*/8, /*egt*/false, /*ksr*/true, /*ws*/0 },
    },
    /* feedback */ 5,
    OPL_ALGO_FM,
};

// FM chain, sub-octave carrier: op1 (mult 0 == 0.5x, patch.h's
// opl_mult_table) sits an octave below the played note; op0 (mult 1)
// modulates it lightly (TL 16) for a bit of edge without losing low-end
// weight. Fast decay/release keeps it tight rather than boomy.
inline constexpr OplPatch OPL_PATCH_BASS = {
    "OPL BASS",
    {
        /* op0 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/16, /*ar*/15, /*dr*/13, /*sl*/3, /*rr*/11, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 (car) */ { /*mult*/0, /*ksl*/0, /*tl*/0,  /*ar*/15, /*dr*/14, /*sl*/1, /*rr*/12, /*egt*/true, /*ksr*/false, /*ws*/0 },
    },
    /* feedback */ 1,
    OPL_ALGO_FM,
};

// Additive: op0 (mult 1) and op1 (mult 2, an octave-plus-a-fifth-ish
// overtone at TL 16, quieter than the fundamental) both carry straight to
// the output with no modulation between them -- two independent partials
// rather than one FM pair. A slightly slower attack (12) and a half-sine
// second partial (ws 1) give it a reedy, organ-like swell.
inline constexpr OplPatch OPL_PATCH_ORGAN = {
    "OPL ORGAN",
    {
        /* op0 */ { /*mult*/1, /*ksl*/0, /*tl*/8,  /*ar*/12, /*dr*/4, /*sl*/1, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 */ { /*mult*/2, /*ksl*/0, /*tl*/16, /*ar*/12, /*dr*/4, /*sl*/1, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/1 },
    },
    /* feedback */ 0,
    OPL_ALGO_ADD,
};

// Additive, percussive: both operators fire together at inharmonic ratios
// (mult 8 and 10) with an instant attack and a decay straight to silence
// (egt=false, sl=15's "no sustain" quirk on both) -- a short percussive hit
// rather than a held tone, but with a real, audible body (order-100ms decay,
// op0 shorter than op1) rather than an instant click. Rectified/quarter-cycle
// waveforms (ws 2/3) add extra high-frequency content a plain sine pair
// wouldn't have.
inline constexpr OplPatch OPL_PATCH_PERC = {
    "OPL PERC",
    {
        /* op0 */ { /*mult*/8,  /*ksl*/2, /*tl*/6,  /*ar*/15, /*dr*/5, /*sl*/15, /*rr*/9, /*egt*/false, /*ksr*/true, /*ws*/2 },
        /* op1 */ { /*mult*/11, /*ksl*/2, /*tl*/10, /*ar*/15, /*dr*/2, /*sl*/15, /*rr*/9, /*egt*/false, /*ksr*/true, /*ws*/3 },
    },
    /* feedback */ 2,
    OPL_ALGO_ADD,
};

inline constexpr const OplPatch *OPL_PATCHES[] = {
    &OPL_PATCH_LEAD, &OPL_PATCH_BELL, &OPL_PATCH_BASS, &OPL_PATCH_ORGAN, &OPL_PATCH_PERC,
};
static constexpr uint32_t OPL_PATCH_COUNT = 5;
