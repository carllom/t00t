#pragma once

#include <cstdint>

// AY-3-8910/YM2149 tone + noise primitives.
//
// TOPOLOGY-FREE (module_chip.md §4's rule, applied to the second chip in this
// module): these know nothing about channels, voices or which of them
// share a noise generator. On real hardware all three tone channels share
// one noise generator; a future engine is free to give each
// dynamically-allocated voice its own noise generator instead, trading the
// real chip's shared-noise/shared-envelope character for full polyphonic
// independence -- a deliberate deviation, not an oversight.
//
// Ground truth is ayumi 1.0 (Peter Sovietov, MIT -- github.com/true-grue/
// ayumi, verified via GitHub's own API, not recalled), read in full and
// vendored unmodified at tools/ay_ref/ayumi/ for host-side validation
// (tools/ay_ctl_diff.py). ayumi's MIT license permits vendoring outright;
// its LICENSE file sits alongside it.
//
// ---------------------------------------------------------------------------
// Internal tick rate
//
// Real AY-3-8910 hardware clocks the tone generator's 12-bit period counter
// at (input clock)/16 and the envelope generator's 16-bit period counter at
// (input clock)/256 -- two different base dividers. ayumi.c does not encode
// either divider directly; instead every generator (tone, noise, envelope)
// is stepped once per call to its internal update_mixer(), at a single
// shared tick rate set by ayumi_configure()'s `step = clock_rate /
// (sample_rate * 8 * DECIMATE_FACTOR)` (DECIMATE_FACTOR = 8, ayumi.h) --
// working through what that implies for how many ticks land per output
// sample gives a shared internal tick rate of clock/8, with each
// generator's own period register compared directly against its own
// counter at that one rate.
//
// At tick rate clock/8, a tone channel toggling once every `period` ticks
// completes a full square-wave cycle (two toggles) every 16*period/clock
// seconds -- the datasheet's clock/16/period formula. The envelope
// generator's own divider chain does not reduce to this same derivation
// (see ay_envelope.h). ayumi's own arithmetic is the reference this file
// matches; tools/ay_ctl_diff.py's per-tick CSV diff against ayumi_dump
// verifies it.
// ---------------------------------------------------------------------------

static constexpr double AY_CLOCK_ZX = 1773400.0;    // ZX Spectrum / most clones
static constexpr double AY_CLOCK_MSX = 1789772.0;   // MSX (~NTSC colour clock)

// Q16 ticks-per-sample (clock/8 internal rate) for a given clock at a given
// sample rate. Q16 rather than sid_osc.h's Q24.8-on-the-accumulator shape:
// AY's counters are compared directly (no top-N-bits waveform read), so
// there is no accumulator to fold the fraction into -- the phase lives on
// its own.
constexpr uint32_t ay_tick_scale_q16(double clock_hz, double sample_rate) {
    return (uint32_t)(clock_hz / 8.0 / sample_rate * 65536.0 + 0.5);
}

// ---------------------------------------------------------------------------
// Tone generator -- one 12-bit period counter per channel, toggling a
// square-wave bit on overflow. period == 0 is silently clamped to 1
// (ayumi_set_tone: `(period == 0) | period`) rather than treated as a stall.
// ---------------------------------------------------------------------------
struct AyTone {
    uint16_t period;   // 12-bit, register units, clamped >= 1
    uint16_t counter;
    uint8_t  out;       // 0 or 1

    void init() { period = 1; counter = 0; out = 0; }

    void set_period(uint16_t reg12) {
        reg12 &= 0xfff;
        period = reg12 ? reg12 : 1;
    }

    // Advance by `ticks` internal clock/8 ticks (see file header). Small
    // (typically low single digits per sample at ZX-clock/44.1kHz), so a
    // bounded loop suffices rather than a closed-form skip-ahead.
    inline void advance(uint32_t ticks) {
        counter = (uint16_t)(counter + ticks);
        while (counter >= period) {
            counter = (uint16_t)(counter - period);
            out ^= 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Noise generator -- one shared 17-bit LFSR (there is one per AY chip, not
// one per channel; see the topology note above for what a t00t voice does
// with that). Feedback bit0 = bit0 XOR bit3 of the *pre-shift* register,
// fed into the vacated bit16 -- ayumi.c's `update_noise()`, itself matching
// the AY-3-8910's documented 17-bit/taps-0,3 polynomial. Ticks at half the
// tone rate: the period compare is against `noise_period << 1`.
// ---------------------------------------------------------------------------
struct AyNoise {
    uint32_t lfsr;      // 17-bit register, bits 0..16
    uint16_t period;    // 5-bit, register units, clamped >= 1
    uint16_t counter;

    // Seed 1, not 0 -- ayumi_configure's `ay->noise = 1`. An all-zero LFSR
    // with this feedback polynomial never leaves zero.
    void init() { lfsr = 1; period = 1; counter = 0; }

    void set_period(uint8_t reg5) {
        reg5 &= 0x1f;
        period = reg5 ? reg5 : 1;
    }

    inline void advance(uint32_t ticks) {
        counter = (uint16_t)(counter + ticks);
        uint16_t limit = (uint16_t)(period << 1);
        while (counter >= limit) {
            counter = (uint16_t)(counter - limit);
            uint32_t bit0x3 = (lfsr ^ (lfsr >> 3)) & 1u;
            lfsr = (lfsr >> 1) | (bit0x3 << 16);
        }
    }

    inline uint8_t out() const { return (uint8_t)(lfsr & 1u); }
};
