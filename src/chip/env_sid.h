#pragma once

#include <cstdint>

// EnvSID -- the SID envelope generator, at sample rate.
//
// TOPOLOGY-FREE (module_chip.md §4). Knows nothing above itself.
//
// module_chip.md §4.3: the SID envelope is a rate-counter design with a piecewise-
// exponential decay, that shape is a large part of the character, and it is
// "not substitutable with the existing EnvConfig ADSR -- this module needs a
// dedicated EnvSID type, for the same reason the FM module needs EnvDX".
//
// The hardware runs two cascaded counters at the master clock:
//
//   * a 15-bit rate counter compared against rate_counter_period[ADSR nibble],
//     which is what makes attack 0 take 9 cycles per step and attack 15 take
//     31251;
//   * an exponential counter that further divides the clock by 1, 2, 4, 8, 16
//     or 30 depending on where the 8-bit envelope counter currently sits,
//     which is what makes decay and release piecewise-exponential rather than
//     linear.
//
// The second counter counts *rate periods*, so the effective time between two
// envelope steps is exactly rate_period x exp_period cycles. That product is
// the whole model, and it is what lets this run per sample instead of per
// cycle without approximating anything: a Q24 phase accumulator ticking at
// (cycles per sample) / (rate_period x exp_period) steps per sample produces
// the same 8-bit staircase, quantised to the sample grid instead of the cycle
// grid. module_chip.md §4.3 calls for "precomputed per-sample increments that
// replicate the segment shape"; this is that, with the segment structure kept
// rather than fitted.
//
// Ground truth is reSID 1.0-pre1's EnvelopeGenerator, dumped as
// cycles-to-each-level by tools/sid_ref/resid_dump.cpp --domain env.

// ---------------------------------------------------------------------------
// Hardware tables, verbatim from reSID's envelope.cc, which derives them from
// the Programmer's Reference Guide and then corrects them against ENV3
// sampling on real chips. Cycles per envelope step, per 4-bit ADSR nibble.
// ---------------------------------------------------------------------------
static constexpr uint16_t ENV_SID_RATE_PERIOD[16] = {
    9,      //   2 ms
    32,     //   8 ms
    63,     //  16 ms
    95,     //  24 ms
    149,    //  38 ms
    220,    //  56 ms
    267,    //  68 ms
    313,    //  80 ms
    392,    // 100 ms
    977,    // 250 ms
    1954,   // 500 ms
    3126,   // 800 ms
    3907,   //   1 s
    11720,  //   3 s
    19532,  //   5 s
    31251,  //   8 s
};

// The exponential divider, and the envelope-counter values at which each
// segment takes effect. reSID loads the period when the counter *reaches*
// 255, 93, 54, 26, 14 and 6 respectively, so the step out of 93 is already
// the divided-by-2 one -- an off-by-one here shows up as a decay that is
// audibly the wrong shape but the right total length.
static constexpr uint8_t ENV_SID_EXP_PERIOD[6] = { 1, 2, 4, 8, 16, 30 };

inline uint8_t env_sid_exp_segment(uint8_t counter) {
    if (counter > 93) return 0;
    if (counter > 54) return 1;
    if (counter > 26) return 2;
    if (counter > 14) return 3;
    if (counter >  6) return 4;
    return 5;
}

// Sustain compares both nibbles of the envelope counter against the 4-bit
// sustain value, so the level is the nibble doubled: 0x0, 0x11, 0x22 ... 0xff.
inline uint8_t env_sid_sustain_level(uint8_t sustain_nibble) {
    return (uint8_t)((sustain_nibble & 0x0f) * 0x11);
}

// ---------------------------------------------------------------------------
// Per-sample increments, Q24, indexed [rate nibble][exponential segment].
//
// Q24 rather than the more obvious Q16 because the slowest combination
// (rate 15 x divide-by-30) is 1.56 steps per sample in Q16 units -- rounding
// that to 2 is a 28% rate error on the tail of an 8-second release. In Q24 the
// same entry is 399 and the error is under 0.3%. The table is 16 x 6 x 4 =
// 384 bytes of flash; the alternative is a runtime divide per segment change.
// ---------------------------------------------------------------------------
struct EnvSidRates {
    uint32_t inc[16][6];
};

constexpr EnvSidRates env_sid_make_rates(double clock_hz, double sample_rate) {
    EnvSidRates r = {};
    const double cycles_per_sample = clock_hz / sample_rate;
    for (int i = 0; i < 16; i++) {
        for (int e = 0; e < 6; e++) {
            double steps_per_sample =
                cycles_per_sample / ((double)ENV_SID_RATE_PERIOD[i] * (double)ENV_SID_EXP_PERIOD[e]);
            r.inc[i][e] = (uint32_t)(steps_per_sample * 16777216.0 + 0.5);
        }
    }
    return r;
}

// ---------------------------------------------------------------------------

enum EnvSidState : uint8_t { ENV_SID_ATTACK, ENV_SID_DECAY, ENV_SID_RELEASE };

struct EnvSid {
    uint32_t phase;      // Q24 fractional envelope step
    uint8_t  counter;    // the 8-bit envelope counter -- the chip's own unit
    uint8_t  state;
    uint8_t  attack;     // ADSR nibbles, kept as the control-plane interface
    uint8_t  decay;
    uint8_t  sustain;
    uint8_t  release;
    uint8_t  gate;

    void init() {
        phase = 0;
        counter = 0;
        state = ENV_SID_RELEASE;
        attack = decay = sustain = release = 0;
        gate = 0;
    }

    void set_adsr(uint8_t ad, uint8_t sr) {
        attack  = (uint8_t)(ad >> 4);
        decay   = (uint8_t)(ad & 0x0f);
        sustain = (uint8_t)(sr >> 4);
        release = (uint8_t)(sr & 0x0f);
    }

    // Gate transitions. Note that neither resets the counter: gating on from a
    // partly-released note attacks from wherever it got to, which is what the
    // hardware does and what makes legato lines behave.
    void gate_on()  { if (!gate) { gate = 1; state = ENV_SID_ATTACK; } }
    void gate_off() { if (gate)  { gate = 0; state = ENV_SID_RELEASE; } }

    // Hard restart.
    //
    // module_chip.md §4.3 and §13.10: on hardware this exists to dodge the 6581 ADSR
    // delay bug and costs 1-2 frames (20-40 ms) of pre-gate, "a direct hit to
    // this project's latency priority". Since the ADSR bug is not modelled,
    // there is nothing to dodge, and the settled decision is an instantaneous
    // reset at zero latency. An imported GoatTracker instrument carrying a
    // hard-restart ADSR value simply lands here.
    void hard_restart() {
        counter = 0;
        phase = 0;
        state = ENV_SID_ATTACK;
        gate = 1;
    }

    // Advance one sample; returns the 8-bit envelope counter.
    //
    // The step loop runs at most three times (the fastest rate is 2.48 steps
    // per sample at 44.1 kHz), and zero times for everything slower than about
    // 8 ms -- which is every rate a held note spends its life in.
    inline uint8_t tick(const EnvSidRates &rates) {
        uint8_t rate_index;
        switch (state) {
            case ENV_SID_ATTACK:  rate_index = attack;  break;
            case ENV_SID_DECAY:   rate_index = decay;   break;
            default:              rate_index = release; break;
        }

        // Attack ignores the exponential counter entirely -- reSID steps the
        // envelope "if (state == ATTACK || ++exponential_counter == period)".
        // The rise is linear; only the falls are piecewise-exponential.
        uint8_t seg = (state == ENV_SID_ATTACK) ? 0 : env_sid_exp_segment(counter);
        phase += rates.inc[rate_index][seg];

        while (phase >= (1u << 24)) {
            phase -= (1u << 24);
            step(rates, seg);
            uint8_t new_seg = (state == ENV_SID_ATTACK) ? 0 : env_sid_exp_segment(counter);
            if (new_seg != seg) {
                // Crossing a segment boundary mid-sample changes the rate for
                // the remaining fraction. Rescaling the residual phase keeps
                // the boundary sample-accurate instead of letting the old rate
                // run to the end of the sample.
                if (rates.inc[rate_index][seg] != 0) {
                    phase = (uint32_t)(((uint64_t)phase * rates.inc[rate_index][new_seg]) /
                                       rates.inc[rate_index][seg]);
                }
                seg = new_seg;
            }
        }
        return counter;
    }

private:
    inline void step(const EnvSidRates &rates, uint8_t) {
        switch (state) {
            case ENV_SID_ATTACK:
                if (counter < 255) counter++;
                if (counter == 255) { state = ENV_SID_DECAY; phase = 0; }
                break;
            case ENV_SID_DECAY: {
                uint8_t sus = env_sid_sustain_level(sustain);
                if (counter > sus) counter--;
                // At the sustain level the counter simply stops; there is no
                // separate sustain state on the chip, which is why a sustain
                // value raised after the fact does not make the envelope rise.
                break;
            }
            default:
                if (counter > 0) counter--;
                break;
        }
    }
};
