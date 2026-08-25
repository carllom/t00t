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

// 4-op full chain: op0 -> op1 -> op2 -> op3 (carrier). Classic FM
// electric-piano shape -- three modulators stacked in series so the deepest
// (op0) shapes the whole chain's harmonic content, while the modulator
// nearest the carrier (op2) has the fastest decay (dr 14, key-scaled so
// higher notes brighten and dull even quicker) so the attack's brightness
// peels away first, leaving a plainer sustained tone from op3 alone -- the
// same bright-pluck-into-mellow-sustain shape OPL_PATCH_BELL's 2-op chain
// uses, just with two extra stages of harmonic development in front of it.
// op0's waveform (ws 4, OPL3/4's double-frequency sine) gives the deepest
// modulator an extra buzz a plain sine wouldn't have. Light feedback (2) on
// op0 only, as with every algorithm here.
inline constexpr OplPatch OPL4_PATCH_CHAIN_EP = {
    "OPL4 CHAIN EP",
    {
        /* op0 (mod) */ { /*mult*/2, /*ksl*/0, /*tl*/20, /*ar*/15, /*dr*/12, /*sl*/4, /*rr*/9, /*egt*/true, /*ksr*/false, /*ws*/4 },
        /* op1 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/16, /*ar*/15, /*dr*/11, /*sl*/3, /*rr*/9, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op2 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/10, /*ar*/15, /*dr*/14, /*sl*/2, /*rr*/8, /*egt*/true, /*ksr*/true,  /*ws*/0 },
        /* op3 (car) */ { /*mult*/1, /*ksl*/0, /*tl*/0,  /*ar*/15, /*dr*/8,  /*sl*/1, /*rr*/7, /*egt*/true, /*ksr*/false, /*ws*/0 },
    },
    /* feedback */ 2,
    OPL_ALGO_4OP_CHAIN,
};

// 4-op dual FM pairs, summed: (op0 -> op1) + (op2 -> op3). Two independent
// 2-op voices layered rather than one deep chain -- OPL has no per-operator
// detune to beat the two pairs against each other, so the "dual" character
// here comes from timbral contrast instead: pair one (op0/op1) is a plain
// sine FM pair for a warm fundamental layer, pair two (op2/op3) modulates a
// square-wave operator (ws 6) into a log-sawtooth carrier (ws 7,
// OPL3/4's exponential-decay waveform) for a bright, buzzy second layer on
// top. KSL 1 on op3 tames that layer's top end so it doesn't dominate the
// blend on higher notes.
inline constexpr OplPatch OPL4_PATCH_DUAL_SAW = {
    "OPL4 DUAL SAW",
    {
        /* op0 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/18, /*ar*/15, /*dr*/10, /*sl*/3, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 (car) */ { /*mult*/1, /*ksl*/0, /*tl*/4,  /*ar*/13, /*dr*/9,  /*sl*/2, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op2 (mod) */ { /*mult*/2, /*ksl*/0, /*tl*/22, /*ar*/15, /*dr*/11, /*sl*/3, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/6 },
        /* op3 (car) */ { /*mult*/1, /*ksl*/1, /*tl*/6,  /*ar*/12, /*dr*/10, /*sl*/2, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/7 },
    },
    /* feedback */ 1,
    OPL_ALGO_4OP_DUAL_FM,
};

// 4-op additive-plus-chain: op0 (additive, carries directly) + (op1 -> op2
// -> op3, a 3-op FM chain also carrying via op3). op0 is a plain sine body
// note with moderate feedback (4) for growl -- the "sub" half of a brass
// sound. The op1->op2->op3 chain is the bright "edge" half: op2 (nearest
// the carrier) uses a square wave (ws 6) for extra buzz, and op3's own
// attack is a touch slower (ar 13, not 15) for the swell real brass has
// rather than an organ's instant-on. Two independent carriers (op0, op3)
// summed straight into the mix, unlike OPL4_PATCH_CHAIN_EP's single
// carrier at the end of one long chain.
inline constexpr OplPatch OPL4_PATCH_BRASS = {
    "OPL4 BRASS",
    {
        /* op0 (car) */ { /*mult*/1, /*ksl*/0, /*tl*/6,  /*ar*/14, /*dr*/9,  /*sl*/2, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/20, /*ar*/15, /*dr*/10, /*sl*/3, /*rr*/9, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op2 (mod) */ { /*mult*/2, /*ksl*/0, /*tl*/14, /*ar*/15, /*dr*/12, /*sl*/3, /*rr*/9, /*egt*/true, /*ksr*/true,  /*ws*/6 },
        /* op3 (car) */ { /*mult*/1, /*ksl*/1, /*tl*/2,  /*ar*/13, /*dr*/9,  /*sl*/2, /*rr*/8, /*egt*/true, /*ksr*/false, /*ws*/0 },
    },
    /* feedback */ 4,
    OPL_ALGO_4OP_ADD_CHAIN,
};

// 4-op additive/FM/additive: op0 (additive) + (op1 -> op2, an FM pair
// carrying via op2) + op3 (additive) -- three independent carriers summed,
// only op1 is a pure modulator. A drawbar-organ stack: op0 is the mult-1
// fundamental, op2 a mult-2 (octave) partial lightly reeded by op1 (ws 5,
// OPL3/4's rectified double-frequency sine, for a nasal edge), op3 a mult-3
// (octave-plus-a-fifth) partial in ws 2 (full-wave-rectified sine) for
// brightness on top -- the same additive-drawbar idea OPL_PATCH_ORGAN's 2-op
// pair uses, extended to three tunable partials instead of two. Near-zero
// decay (dr 1) and sl 0 on every operator hold each partial at full level
// for as long as the note is held, organ-style, rather than decaying like
// the other three 4-op patches above.
inline constexpr OplPatch OPL4_PATCH_ORGAN_PAD = {
    "OPL4 ORGAN PAD",
    {
        /* op0 (car) */ { /*mult*/1, /*ksl*/0, /*tl*/6,  /*ar*/15, /*dr*/1, /*sl*/0, /*rr*/6, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op1 (mod) */ { /*mult*/1, /*ksl*/0, /*tl*/24, /*ar*/15, /*dr*/1, /*sl*/0, /*rr*/6, /*egt*/true, /*ksr*/false, /*ws*/0 },
        /* op2 (car) */ { /*mult*/2, /*ksl*/1, /*tl*/12, /*ar*/15, /*dr*/1, /*sl*/0, /*rr*/6, /*egt*/true, /*ksr*/false, /*ws*/5 },
        /* op3 (car) */ { /*mult*/3, /*ksl*/2, /*tl*/18, /*ar*/15, /*dr*/1, /*sl*/0, /*rr*/6, /*egt*/true, /*ksr*/false, /*ws*/2 },
    },
    /* feedback */ 0,
    OPL_ALGO_4OP_ADD_FM_ADD,
};

inline constexpr const OplPatch *OPL_PATCHES[] = {
    &OPL_PATCH_LEAD, &OPL_PATCH_BELL, &OPL_PATCH_BASS, &OPL_PATCH_ORGAN, &OPL_PATCH_PERC,
    &OPL4_PATCH_CHAIN_EP, &OPL4_PATCH_DUAL_SAW, &OPL4_PATCH_BRASS, &OPL4_PATCH_ORGAN_PAD,
};
static constexpr uint32_t OPL_PATCH_COUNT = 9;
