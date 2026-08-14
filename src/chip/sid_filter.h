#pragma once

#include <cstdint>
#include "sid_tables.h"

// SID filter primitive -- a two-integrator-loop SVF with a chip-specific
// cutoff table and an optional saturating feedback path.
//
// TOPOLOGY-FREE (module_chip.md §4). No bus index, no voice list, no routing. A
// caller hands it a summed input and gets a filtered sample back; whether that
// sum came from a filter bus (§5) or from the strict harness's one-shared-
// filter topology (§11.1) is not this file's business.
//
// module_chip.md §2's key claim -- "The SID filter *is* a two-integrator-loop SVF" --
// is confirmed by the reference: reSID's own 8580 path is
//     dVbp = w0*(Vhp>>4)>>16;  dVlp = w0*(Vbp>>4)>>16;
//     Vhp  = (Vbp*_1024_div_Q>>10) - Vlp - Vi;
// which is exactly src/filter.h's structure. So this reuses that structure
// rather than reimplementing it, per §2's reuse inventory.
//
// What is *not* reusable is reSID's 6581. That path is a transistor-level
// model: op-amp transfer curves, a VCR gate-voltage solver, and three
// 2^16-entry tables. It cannot run on an RP2350 and it does not need to --
// module_chip.md §5.1 asks for a cutoff LUT plus saturation, and the LUT is measured
// off reSID by tools/sid_ref/resid_probe.cpp and fitted by
// tools/fit_6581_filter.py. See sid_tables.h for the provenance, which
// §5.1 requires be named rather than presented as canonical.

// ---------------------------------------------------------------------------
// Mode mask.
//
// module_chip.md §5.1 change 1: FilterMode becomes a 3-bit *mask*, because the SID
// sums LP, BP and HP simultaneously -- $D418 bits 4/5/6 are independent
// enables, not a selector. The existing FilterMode enum in engine_base.h stays
// as it is for the subtractive engine; this is the chip module's own type.
// ---------------------------------------------------------------------------
enum SidFilterMode : uint8_t {
    SID_FILT_LP = 0x1,
    SID_FILT_BP = 0x2,
    SID_FILT_HP = 0x4,
};

enum SidFilterModel : uint8_t { SID_MODEL_6581 = 0, SID_MODEL_8580 = 1 };

// Cutoff register (11 bits, 0..2047) -> the SVF's Q15 half-frequency
// coefficient, via the model's own table. module_chip.md §5.1 change 2: "pluggable
// cutoff LUT instead of svf_compute_f_half's linear map, so 6581 vs 8580 is a
// table swap rather than a code fork."
inline int16_t sid_filter_f_half(uint8_t model, uint16_t fc) {
    if (fc > 2047) fc = 2047;
    return (int16_t)(model == SID_MODEL_8580 ? SID_8580_FC_Q15[fc] : SID_6581_FC_Q15[fc]);
}

// Resonance nibble (0..15) -> Q15 damping coefficient, same convention as
// src/filter.h's svf_compute_q: large means heavily damped, small means near
// self-oscillation.
inline int32_t sid_filter_q(uint8_t model, uint8_t res) {
    return (int32_t)(model == SID_MODEL_8580 ? SID_8580_RES_Q15[res & 0x0f]
                                             : SID_6581_RES_Q15[res & 0x0f]);
}

// ---------------------------------------------------------------------------
// Saturation.
//
// module_chip.md §5.1 change 3 and §3: the 6581's filter nonlinearity is signal-path,
// so it is kept. It is also what makes §5.2's shared-bus intermodulation a
// real tonal property rather than a claim -- voices summed before a *linear*
// filter do not intermodulate at all.
//
// A cubic soft clip is the cheap shape that has the right qualitative
// behaviour (odd symmetry, gentle onset, hard ceiling). It is applied to the
// summed input, which is where reSID's own nonlinearity lives -- in the
// summer's op-amp, ahead of the integrators, not inside the feedback loop.
// Applying it in the loop instead makes resonance level-dependent in a way the
// reference does not do.
//
// SID_FILT_SAT_SCALE is the input level at which clipping is fully in effect.
// It is expressed in the voice-output units defined by sid_voice.h, so a
// single voice at full envelope is well below it and three summed voices at
// full envelope are not -- which is the behaviour being modelled.
// ---------------------------------------------------------------------------
static constexpr int32_t SID_FILT_SAT_SCALE = SID_FILT_SAT_SCALE_6581;

inline int32_t sid_filter_saturate(int32_t x) {
    // y = x - x^3/3 on [-1, 1], clamped outside. In fixed point with
    // s = SID_FILT_SAT_SCALE: y = x - x*x*x/(3*s*s), clamped to +-2s/3.
    //
    // s is a compile-time constant, so x/s is done as a Q31 reciprocal
    // multiply rather than two runtime int64_t/int32_t divides. Cortex-M33
    // has no hardware 64-bit divide, so each divide there was a software
    // __aeabi_ldivmod call; with this called once per filter bus per sample
    // (CHIP_RIG_SAT default on), that measured as ~200 c/f across 4 buses
    // on the P0 rig (module_chip.md P0 gate) -- most of the "filter bus cost" being
    // ~3.5x module_chip.md §9's 50-75 c/f/bus estimate. Verified against the
    // original divide-based formula across the full input range: max |err|
    // 180 out of a ~870k peak (0.02%), only right at the clamp edge -- this
    // curve is a cheap qualitative approximation already, not a reSID-fitted
    // one, so that's within its existing tolerance.
    constexpr int32_t lim = SID_FILT_SAT_SCALE;
    constexpr int64_t SAT_INV_LIM_Q31 = (((int64_t)1 << 31) + lim / 2) / lim;
    if (x >= lim)  return (2 * lim) / 3;
    if (x <= -lim) return -(2 * lim) / 3;
    int32_t xq = (int32_t)((int64_t)x * SAT_INV_LIM_Q31);   // x/lim, Q31
    int64_t xq2 = ((int64_t)xq * xq) >> 31;                  // (x/lim)^2, Q31
    // x*(x/lim)^2 is bounded by lim (|x|<lim, (x/lim)^2<1), so it fits
    // int32_t -- narrow before dividing by 3 so that division is the
    // hardware 32-bit sdiv this target has, not another software 64-bit one.
    int32_t numer = (int32_t)(((int64_t)x * xq2) >> 31);
    return x - numer / 3;
}

// ---------------------------------------------------------------------------
// The filter.
//
// Structurally src/filter.h's SVFilter with two changes: the mode selector is
// a mask that sums outputs, and the input can be saturated first. Kept as its
// own type rather than as flags on SVFilter because the subtractive engine's
// filter is on the hot path of a different engine and should not grow a branch
// for this one's benefit; module_chip.md §5.1 notes all three changes "backport
// usefully", and that backport is a separate decision from this module.
// ---------------------------------------------------------------------------
struct SidFilter {
    int32_t lp;
    int32_t bp;

    void init() { lp = 0; bp = 0; }

    // One sample. F_half and Q come from the tables above; mode_mask is any
    // combination of SID_FILT_*; saturate applies the 6581 nonlinearity.
    inline int32_t tick(int32_t input, int16_t F_half, int32_t Q_q15,
                        uint8_t mode_mask, bool saturate) {
        if (saturate) input = sid_filter_saturate(input);

        int32_t hp = 0;
        // Two-pass integration, as in src/filter.h: it is what keeps the SVF
        // stable at high cutoff and low Q, and the fitted tables in
        // sid_tables.h were measured against this exact topology. Changing the
        // pass count silently invalidates them.
        for (int pass = 0; pass < 2; pass++) {
            hp = input - lp - (int32_t)(((int64_t)Q_q15 * bp) >> 15);
            bp += (int32_t)(((int64_t)F_half * hp) >> 15);
            lp += (int32_t)(((int64_t)F_half * bp) >> 15);
        }

        int32_t out = 0;
        if (mode_mask & SID_FILT_LP) out += lp;
        if (mode_mask & SID_FILT_BP) out += bp;
        if (mode_mask & SID_FILT_HP) out += hp;
        return out;
    }
};

// ---------------------------------------------------------------------------
// The C64 board's output network.
//
// module_chip.md §10's "free bonus": the passive RC network between the SID and the AV
// connector is a one-pole low-pass and "is genuinely part of 'the SID sound'".
// It sits before the speaker simulation stage and after the chip. reSID models
// it as ExternalFilter, a high-pass at ~16 Hz and a low-pass at ~16 kHz; the
// high-pass matters only for DC removal, which is why it is here rather than
// deferred with the rest of §10 -- the 6581's 0x380 wave zero puts a real DC
// step on every gate, and without this it lands in the mix.
// ---------------------------------------------------------------------------
// The two states are held in Q16, not in output units.
//
// This is not tidiness. The two corner frequencies are three decades apart
// (15.9 Hz and 15.9 kHz), so the high-pass coefficient is 75 out of 32768 --
// and with the state in output units, `(lp - hp) * 75 >> 15` truncates to zero
// for any difference below 437. The integrator then simply stops, holding
// whatever DC it had charged to, forever.
//
// Measured, before the fix: on the saw_c3 stream the attack, decay and sustain
// tracked reSID within 0.4 dB, and then the release tail settled onto a
// constant -51.5 dBFS floor instead of reaching silence -- 188 dB of envelope
// error, on the simplest stream in the corpus, from a filter that is not even
// part of the chip. reSID hit the same wall and says so: its
// ExternalFilterCoefficients comments "at least 27 bits of accuracy. This is
// crucial since w0lp and w0hp are so far apart."
struct SidBoardFilter {
    int64_t lp_q16;
    int64_t hp_q16;   // the DC the output is measured against

    void init() { lp_q16 = 0; hp_q16 = 0; }

    inline int32_t tick(int32_t x) {
        lp_q16 += (((int64_t)x * 65536 - lp_q16) * SID_BOARD_LP_Q15) >> 15;
        hp_q16 += ((lp_q16 - hp_q16) * SID_BOARD_HP_Q15) >> 15;
        return (int32_t)((lp_q16 - hp_q16) >> 16);
    }
};
