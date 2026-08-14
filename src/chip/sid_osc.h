#pragma once

#include <cstdint>

// SID oscillator primitive -- accumulator, waveform logic, noise LFSR.
//
// TOPOLOGY-FREE (chip.md §4, hard rule from the first commit): this knows
// nothing about chips, voices, buses, allocation or adjacency. Sync and ring
// take their modulation from a value the caller supplies, so the same code
// serves the free-routing engine (per-voice sub-oscillator, §4.4) and the
// CHIP_STRICT harness's adjacency wiring (§11.1) without a second
// implementation. That is the only reason the strict harness buys anything.
//
// Ground truth for every function here is reSID 1.0-pre1, dumped by
// tools/sid_ref/resid_dump.cpp and diffed by tools/sid_ctl_diff.py. Where
// this file departs from chip.md, the reference is why -- each such place is
// commented, because the doc is otherwise the thing a reader would trust.

// ---------------------------------------------------------------------------
// Accumulator format
//
// chip.md §4.1: the SID's 24-bit accumulator held as Q24.8 in a uint32_t, i.e.
// shifted left 8. Bits 31..20 are then the top 12 the waveform logic sees, and
// bit 27 is the accumulator's bit 19 that clocks the noise register.
//
// The per-sample increment is freq_reg * (clock / sample_rate) * 256.
//
//   chip.md §4.1 gives "inc = freq_reg * 5805  (44.1 kHz, PAL)". 5805 is
//   1000000/44100*256 -- a nominal 1 MHz clock, not PAL. The PAL C64 clock is
//   985248 Hz, giving 5719. The difference is 1.5%, a quarter of a semitone,
//   which is well inside what a tuned instrument makes audible, so the
//   constants below are named after their clocks and the nominal-1 MHz value
//   is not offered at all.
// ---------------------------------------------------------------------------

static constexpr double SID_CLOCK_PAL  = 985248.0;
static constexpr double SID_CLOCK_NTSC = 1022730.0;

// Q8 cycles-per-sample for a given master clock at a given sample rate.
constexpr uint32_t sid_acc_scale_q8(double clock_hz, double sample_rate) {
    return (uint32_t)(clock_hz / sample_rate * 256.0 + 0.5);
}

// freq_reg (0..65535) -> Q24.8 accumulator increment per sample.
// Max value is 65535 * 5719 = 374.8M, comfortably inside uint32_t.
constexpr uint32_t sid_freq_to_inc(uint16_t freq_reg, uint32_t acc_scale_q8) {
    return (uint32_t)freq_reg * acc_scale_q8;
}

// ---------------------------------------------------------------------------
// Waveform selection bits, as they sit in the SID control register ($D404 etc.
// bits 7..4). Kept in register order because the control plane, the instrument
// tables and the strict harness all speak that language.
// ---------------------------------------------------------------------------
enum SidWave : uint8_t {
    SID_WAVE_NONE = 0x0,
    SID_WAVE_TRI  = 0x1,
    SID_WAVE_SAW  = 0x2,
    SID_WAVE_PULSE = 0x4,
    SID_WAVE_NOISE = 0x8,
};

// The waveform D/A's "zero" level -- what a silent voice sits at, subtracted
// before the envelope multiply.
//
// This is NOT 0x800 on a 6581. reSID's Voice::set_chip_model documents 0x380,
// measured on a MOS 6581R4AR: "The waveform output range is 0x000 to 0xfff, so
// the 'zero' level should ideally have been 0x800. In the measured chip, the
// waveform output 'zero' level was found to be 0x380." The asymmetry is why a
// 6581 voice has a DC step at gate time that an 8580 does not, and it costs
// one constant. chip.md does not mention it; §3's test puts it firmly on the
// signal-path side, so it is kept.
static constexpr int32_t SID_WAVE_ZERO_6581 = 0x380;
static constexpr int32_t SID_WAVE_ZERO_8580 = 0x800;

// ---------------------------------------------------------------------------
// Noise LFSR
//
// 23-bit, feedback bit0 = bit22 ^ bit17. TWO taps.
//
// chip.md §4.2 says "23-bit LFSR, taps 22/20/16/13/11/7/4/2". Those eight
// positions are not taps; they are (near) the *output* scatter. reSID's
// clock_shift_register() is unambiguous:
//     bit0 = ((shift_register >> 22) ^ (shift_register >> 17)) & 0x1
// and set_noise_output() scatters register bits 20,18,14,11,9,5,2,0 into
// output bits 11..4, with the low four output bits grounded. Built from the
// doc alone this would be a different sequence with a different spectrum and
// nothing but a listening test to catch it.
//
// Seed is 0x7ffffe, not 0x7fffff: reSID's WaveformGenerator::reset() notes
// "when reset is released the shift register is clocked once".
// ---------------------------------------------------------------------------
static constexpr uint32_t SID_LFSR_SEED = 0x7ffffeu;

inline uint32_t sid_lfsr_step(uint32_t sr) {
    uint32_t bit0 = ((sr >> 22) ^ (sr >> 17)) & 0x1u;
    return ((sr << 1) | bit0) & 0x7fffffu;
}

// 12-bit noise output: the scattered register bits, low four bits grounded.
inline uint16_t sid_lfsr_output(uint32_t sr) {
    return (uint16_t)(((sr & 0x100000u) >> 9) |
                      ((sr & 0x040000u) >> 8) |
                      ((sr & 0x004000u) >> 5) |
                      ((sr & 0x000800u) >> 3) |
                      ((sr & 0x000200u) >> 2) |
                      ((sr & 0x000020u) << 1) |
                      ((sr & 0x000004u) << 3) |
                      ((sr & 0x000001u) << 4));
}

// How many times the noise register clocks over one accumulator step.
//
// chip.md §4.2: "Noise needs a clock count per sample, not a single clock: at
// high frequencies more than one bit-19 transition occurs per 44.1 kHz sample."
// Correct, and the bound is tight -- at the maximum frequency register the
// accumulator advances 0x165B0000 in Q24.8 against a 0x10000000 shift period,
// so the count is never more than 2. The mask keeps that true across the
// uint32 wrap rather than relying on it.
inline uint32_t sid_noise_clocks(uint32_t acc_before, uint32_t acc_after) {
    // Bit 19 of the 24-bit accumulator is bit 27 here. It rises once per
    // 0x1000_0000 of advance, at phase 0x0800_0000 -- so shifting the origin
    // by that phase turns "count the rising edges" into a subtraction.
    return (((acc_after - 0x08000000u) >> 28) - ((acc_before - 0x08000000u) >> 28)) & 0xfu;
}

// ---------------------------------------------------------------------------
// Hard sync
//
// chip.md §4.1: "Hard sync is plain uint32_t wrap detection -- carry out of bit
// 31 *is* the 24-bit accumulator overflow." The rate is right and the event is
// not. reSID syncs on the source's MSB *rising* -- accumulator bit 23, which
// is bit 31 here -- so the reset lands half an accumulator cycle away from
// where wrap detection would put it. Sync is a phase-reset effect and its
// whole character is which phase it resets from, so this matters and is the
// same one-instruction test either way.
// ---------------------------------------------------------------------------
inline bool sid_msb_rising(uint32_t acc_before, uint32_t acc_after) {
    return (~acc_before & acc_after & 0x80000000u) != 0;
}

// ---------------------------------------------------------------------------
// Waveform generation from the top 12 accumulator bits.
//
// ring_msb_flip is the MSB substitution for ring modulation: pass 0x800 when
// the modulation source's MSB is LOW, 0 otherwise. reSID's index is
//     (accumulator ^ (~sync_source->accumulator & ring_msb_mask)) >> 12
// so an inverted source MSB flips the destination's, and ring_msb_mask is
// itself zero unless triangle is selected *without* sawtooth -- i.e. ring is a
// no-op for waveforms 2, 3, 6 and 7. Both facts are in the reference dump and
// both are checked by the diff.
// ---------------------------------------------------------------------------

inline uint16_t sid_tri(uint16_t top12) {
    // MSB folds the lower 11 bits, then they are left-shifted for full
    // amplitude at half resolution. Bit 0 is always zero.
    uint16_t msb = (uint16_t)(top12 & 0x800);
    return (uint16_t)(((top12 ^ (msb ? 0xfff : 0x000)) << 1) & 0xffe);
}

inline uint16_t sid_saw(uint16_t top12) { return top12; }

inline uint16_t sid_pulse(uint16_t top12, uint16_t pw) {
    return (uint16_t)(top12 >= pw ? 0xfff : 0x000);
}

// Combined waveforms.
//
// chip.md §13.5 settles this as "AND first (P1), LUTs later (P6)", on the
// grounds that AND "gets to roughly TinySID grade and is one instruction".
// F0 measured what that costs against reSID's sampled tables, over all 4096
// phases (tools/sid_ctl_diff.py, domain `wave`):
//
//     saw+tri    mean |err| 1012 / 4095   max 2720
//     pulse+tri  mean |err| 1423 / 4095   max 3840
//     pulse+saw  mean |err| 1971 / 4095   max 4080
//     all three  mean |err| 1020 / 4095   max 2720
//
// That is 25-48% of full scale, not a shading. The AND is kept as the default
// because the decision to defer was made on cost, not on accuracy, and F0's
// job is to price it rather than to relitigate it -- but "roughly TinySID
// grade" should be read as "audibly a different waveform", and the P6 LUT is
// the fix, not a polish item.
inline uint16_t sid_combined_and(uint8_t waveform, uint16_t top12, uint16_t pw, uint16_t ring_msb_flip) {
    uint16_t out = 0xfff;
    if (waveform & SID_WAVE_TRI)   out = (uint16_t)(out & sid_tri((uint16_t)(top12 ^ ring_msb_flip)));
    if (waveform & SID_WAVE_SAW)   out = (uint16_t)(out & sid_saw(top12));
    if (waveform & SID_WAVE_PULSE) out = (uint16_t)(out & sid_pulse(top12, pw));
    return out;
}

// ---------------------------------------------------------------------------
// The oscillator itself.
//
// One sample per tick(). State is deliberately flat and small: an engine that
// wants 32 of these pays 20 bytes each.
// ---------------------------------------------------------------------------
struct SidOsc {
    uint32_t acc;       // Q24.8 accumulator
    uint32_t inc;       // Q24.8 per-sample increment
    uint32_t lfsr;      // 23-bit noise register
    uint16_t pw;        // 12-bit pulse width
    uint8_t  waveform;  // SidWave bits, control register bits 7..4
    uint8_t  test;      // TEST bit: holds the accumulator at zero, pulse high

    void init() {
        acc = 0;
        inc = 0;
        lfsr = SID_LFSR_SEED;
        pw = 0;
        waveform = 0;
        test = 0;
    }

    // ---- The three phases of a sample -------------------------------------
    //
    // Split rather than fused, because both users need the split and for the
    // same reason: reSID clocks every oscillator, *then* synchronises, *then*
    // reads the waveform, and a sync source's edge must be visible to its
    // destination within the same sample. CHIP_STRICT needs that across three
    // adjacency-wired voices; the free engine needs it between a voice and its
    // own sub-oscillator (chip.md §4.4). Fusing them would force one of the two
    // to apply sync a sample late, which on a sync lead is not a subtlety --
    // it is the difference between a locked timbre and a beating one.
    //
    // A modulator that is only a sync source calls advance() and nothing else,
    // which is where chip.md §4.4's "5-8 c/f versus 45-65 for a whole voice"
    // comes from.

    // Advance the accumulator and the noise register. Returns true if the MSB
    // rose on this step -- the hard sync event.
    inline bool advance() {
        if (test) {
            // TEST holds the accumulator at zero and the shift register at its
            // reset value. Modelling the slow SRAM rise reSID emulates
            // (0x7fffff after ~0x8000 cycles) buys nothing here: nothing reads
            // the register back.
            acc = 0;
            lfsr = SID_LFSR_SEED;
            return false;
        }
        uint32_t before = acc;
        acc += inc;
        uint32_t clocks = sid_noise_clocks(before, acc);
        while (clocks--) lfsr = sid_lfsr_step(lfsr);
        return sid_msb_rising(before, acc);
    }

    // Hard sync: the source's MSB rose, so this oscillator restarts.
    inline void sync() { acc = 0; }

    // This oscillator's accumulator MSB, for a destination's ring modulation.
    inline bool msb() const { return (acc & 0x80000000u) != 0; }

    // The ring-mod MSB substitution a *destination* should be given, from this
    // oscillator used as the source. reSID's index is
    // (acc ^ (~src_acc & ring_msb_mask)) >> 12, so a low source MSB flips the
    // destination's and a high one leaves it alone.
    inline uint16_t ring_flip_for_dest() const { return msb() ? 0x000 : 0x800; }

    // The 12-bit waveform output for the current accumulator.
    inline uint16_t output(uint16_t ring_msb_flip) const {
        if (test) return (waveform & SID_WAVE_PULSE) ? 0xfff : sample_waveform(0, ring_msb_flip);
        return sample_waveform((uint16_t)(acc >> 20), ring_msb_flip);
    }

    // advance + sync + output, for callers with nothing to interleave.
    inline uint16_t tick(bool sync_reset, uint16_t ring_msb_flip) {
        advance();
        if (sync_reset) sync();
        return output(ring_msb_flip);
    }

    inline uint16_t sample_waveform(uint16_t top12, uint16_t ring_msb_flip) const {
        if (waveform == 0) return 0;
        if (waveform & SID_WAVE_NOISE) {
            // Noise combined with anything else short-circuits the register on
            // real hardware and decays to silence. Returning the plain noise
            // output is the deliberate simplification; the combination is not
            // musically used except as a one-frame transient, which the frame
            // VM produces with waveform changes rather than by holding this
            // state.
            return sid_lfsr_output(lfsr);
        }
        return sid_combined_and(waveform, top12, pw, ring_msb_flip);
    }
};
