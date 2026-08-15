// resid_dump -- control-plane ground truth from reSID, as CSV. No audio.
//
// Verification is split into a control plane compared *exactly*, numerically,
// and a signal plane compared spectrally. Most of a SID voice is control-rate
// or purely combinational logic, so most of it lands on the exact side:
//
//   env    envelope counter trajectories -- the time, in master clock cycles,
//          at which the 8-bit counter first reaches each level, for all 16
//          rates x attack/decay/release. Scale-free: comparable between
//          reSID's 1 MHz cycle domain and t00t's 44.1 kHz sample domain.
//   lfsr   the 23-bit noise shift register sequence and its scattered 12-bit
//          output. Bit-exact; there is no tolerance to argue about.
//   wave   the 12-bit waveform output for every one of the 4096 accumulator
//          top-12 values, for each waveform selection and ring-mod state.
//          Pure combinational logic; ground truth for module_chip.md §4.2's
//          waveform deferral.
//   dac    reSID's own 8-bit (envelope) and 12-bit (waveform) R-2R DAC tables
//          (module_chip.md §3). Emitted so the cost of keeping them is
//          measurable rather than assumed.
//
// The domains are defined here and this file is the authority on them:
// tools/host_render/t00t_chip_dump.cpp must emit the same columns, and
// tools/sid_ctl_diff.py diffs them. Change one side and you must change both.
//
// Host-only tooling; reSID is fetched, not vendored (see fetch_resid.sh).

#include "dac.h"
#include "envelope.h"
#include "wave.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace reSID;

// ---------------------------------------------------------------------------
// env: cycles-to-level trajectories.
//
// Emitted as (stage, rate, level, cycles) rather than (time, level) because
// the two engines run on different clocks. "How many master-clock cycles until
// the envelope first reads 128" is the same question at 985248 Hz and at
// 44.1 kHz; "what is the level at sample 700" is not.
//
// Attack sweeps 0 -> 255 with the given attack rate. Decay sweeps 255 -> 0
// with sustain forced to 0 so the decay runs the whole way, exercising every
// exponential segment (periods 1, 2, 4, 8, 16, 30 at counter values 255, 93,
// 54, 26, 14, 6). Release does the same from the release state, which is the
// same curve reached by a different door -- dumped separately anyway, because
// the two get there through different state-machine paths and a bug in one
// need not show in the other.
// ---------------------------------------------------------------------------
static void dump_env(bool model_8580) {
    printf("# domain=env cols=stage,rate,level,cycles\n");

    struct Stage { const char *name; int mode; };
    const Stage stages[] = { {"attack", 0}, {"decay", 1}, {"release", 2} };

    for (const Stage &s : stages) {
        for (int rate = 0; rate < 16; rate++) {
            EnvelopeGenerator env;
            env.set_chip_model(model_8580 ? MOS8580 : MOS6581);
            env.reset();

            // ADSR nibbles. The rate under test goes in the stage's own
            // nibble; the others are set to 0 (fastest) so they contribute no
            // time to the phase being measured.
            reg8 attack_decay, sustain_release;
            switch (s.mode) {
                case 0:  attack_decay = (reg8)(rate << 4); sustain_release = 0x00; break;
                case 1:  attack_decay = (reg8)rate;        sustain_release = 0x00; break;
                default: attack_decay = 0x00;              sustain_release = (reg8)rate; break;
            }
            env.writeATTACK_DECAY(attack_decay);
            env.writeSUSTAIN_RELEASE(sustain_release);

            long long cycle = 0;
            const long long limit = 40LL * 985248LL;  // 40 s: 8 s is the slowest rate

            // Decay and release are only reachable through a completed attack,
            // so both are primed by gating on and running the (rate 0) attack
            // to 255 first. Measuring from gate-on instead would time the
            // attack and call it a decay -- and because the counter starts at
            // 0, every "has it fallen to level k yet" test passes immediately
            // and the whole trajectory collapses onto cycle 0.
            env.writeCONTROL_REG(0x01);  // gate on
            if (s.mode != 0) {
                while (env.readENV() != 255 && cycle < limit) { env.clock(); cycle++; }
            }
            if (s.mode == 2) {
                env.writeCONTROL_REG(0x00);  // gate off -> RELEASE
            }

            cycle = 0;
            if (s.mode == 0) {
                int next = 1;
                printf("attack,%d,0,0\n", rate);
                while (next <= 255 && cycle < limit) {
                    env.clock(); cycle++;
                    while (next <= 255 && env.readENV() >= (reg8)next) {
                        printf("attack,%d,%d,%lld\n", rate, next, cycle);
                        next++;
                    }
                }
            } else {
                // Decay/release fall from 255 to 0.
                int next = 254;
                printf("%s,%d,255,0\n", s.name, rate);
                while (next >= 0 && cycle < limit) {
                    env.clock(); cycle++;
                    while (next >= 0 && env.readENV() <= (reg8)next) {
                        printf("%s,%d,%d,%lld\n", s.name, rate, next, cycle);
                        next--;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// lfsr: the noise shift register sequence.
//
// Driven at maximum frequency (0xffff), which clocks the register once per
// 0x100000/0xffff = 16 cycles. Emits the 23-bit register state and the 12-bit
// scattered output after each shift, from the post-reset state 0x7fffff.
//
// This is the domain where module_chip.md §4.2 lists the wrong bit positions:
// it lists taps 22/20/16/13/11/7/4/2, but reSID's clock_shift_register() is
// bit0 = bit22 ^ bit17 -- two taps. The eight positions in module_chip.md's
// list are the *output* scatter (register bits 20,18,14,11,9,5,2,0 -> output
// bits 11..4), not the feedback taps. An implementation built from the doc
// alone would produce a different sequence with a different spectrum, and
// this dump is what catches that in one diff.
// ---------------------------------------------------------------------------
static void dump_lfsr(bool model_8580, int count) {
    // Only the scattered output is emitted, not the register itself: the
    // register is protected in reSID and unreadable on real hardware, whereas
    // the output is exactly what OSC3 exposes and exactly what the DAC sees.
    // If the outputs agree for thousands of shifts the registers agree too.
    printf("# domain=lfsr cols=index,noise_out12\n");
    // Post-reset state is 0x7ffffe, not 0x7fffff: reSID's WaveformGenerator::
    // reset() comments "when reset is released the shift register is clocked
    // once". t00t must seed from the same value or every sample differs.

    WaveformGenerator wg, dummy;
    wg.set_chip_model(model_8580 ? MOS8580 : MOS6581);
    dummy.set_chip_model(model_8580 ? MOS8580 : MOS6581);
    wg.set_sync_source(&dummy);
    dummy.set_sync_source(&wg);
    wg.reset();
    dummy.reset();

    // freq = 0x8000 makes 32 cycles advance the accumulator by exactly
    // 0x100000, i.e. exactly one bit-19 rise and so exactly one shift, with no
    // drift. 0xffff would give 16.0002 cycles per shift, which silently loses
    // a shift every few thousand samples and would make a correct
    // implementation fail the diff far down the sequence.
    //
    // The power-on accumulator (0x555555, see below) does not matter here:
    // over any 32-cycle window the count of bit-19 rises is exactly one
    // whatever the starting phase.
    wg.writeFREQ_LO(0x00);
    wg.writeFREQ_HI(0x80);
    wg.writeCONTROL_REG(0x81);  // noise, gate on

    // reSID's post-reset shift register is 0x7fffff and its output follows
    // from that; emit index 0 before any shift so both sides agree on the
    // starting point rather than on the first transition.
    for (int i = 0; i < count; i++) {
        wg.set_waveform_output();
        // readOSC() is the raw 8 high bits; output() applies the DAC. The
        // sequence wants the raw 12-bit value, which is what set_waveform_output
        // leaves in place -- recovered here as readOSC()<<4 plus the low bits,
        // which for noise are always zero (the low 4 waveform bits are
        // grounded, see wave.h's noise diagram).
        printf("%d,%d\n", i, (int)wg.readOSC() << 4);
        for (int c = 0; c < 32; c++) wg.clock();
    }
}

// ---------------------------------------------------------------------------
// wave: the 12-bit waveform output over a full accumulator cycle.
//
// freq = 0x1000 advances the 24-bit accumulator by exactly one top-12 step
// per cycle, so cycle c has accumulator top-12 = c & 0xfff with no need to
// read the accumulator at all -- *provided* the accumulator starts at zero,
// which it does not. reSID models the real chip's power-on state
// ("Accumulator's even bits are high on powerup", wave.cc's constructor) as
// 0x555555, and reset() explicitly leaves it alone. A TEST-bit pulse is the
// only public way to zero it, so that is what the sweep opens with; without
// it every row is offset by 0x555 and the "sawtooth" reads as a ramp starting
// two-thirds of the way up.
//
// The ring-mod source is held still (freq 0, TEST-pulsed to zero) so its MSB
// is low for the entire sweep. Low is the only interesting state: reSID's
// index is (acc ^ (~src_acc & ring_msb_mask)) >> 12, so a *high* source MSB
// leaves the index untouched and ring=1 degenerates to ring=0. Sweeping a
// moving source would therefore spend half the rows re-measuring the ring=0
// pass and put a discontinuity in the middle of the other half.
//
// ring_msb_mask is also zero unless sawtooth is deselected, so ring is
// expected to be a no-op for waveforms 2, 3, 6 and 7 -- that "no-op" is part
// of what the diff checks, not an omission from it.
//
// Noise (waveform 8) is excluded: its output is a function of the shift
// register, not the accumulator, and it has its own domain above. Waveform 0
// (no waveform selected, floating DAC) is excluded for the same reason -- it
// is a decay of the previous value over time, not a function of phase.
// ---------------------------------------------------------------------------
static void dump_wave(bool model_8580) {
    printf("# domain=wave cols=waveform,ring,acc_top12,out12\n");

    for (int waveform = 1; waveform <= 7; waveform++) {
        for (int ring = 0; ring < 2; ring++) {
            WaveformGenerator wg, src;
            wg.set_chip_model(model_8580 ? MOS8580 : MOS6581);
            src.set_chip_model(model_8580 ? MOS8580 : MOS6581);
            wg.set_sync_source(&src);
            src.set_sync_source(&wg);
            wg.reset();
            src.reset();

            wg.writeFREQ_LO(0x00); wg.writeFREQ_HI(0x10);   // 0x1000
            src.writeFREQ_LO(0x00); src.writeFREQ_HI(0x00); // held still

            // TEST pulse: the rising edge zeroes the accumulator, and writing
            // the real control register immediately after clears TEST again
            // without any cycle having been clocked in between.
            const reg8 control = (reg8)((waveform << 4) | (ring ? 0x04 : 0x00) | 0x01);
            wg.writeCONTROL_REG(0x08);  wg.writeCONTROL_REG(control);
            src.writeCONTROL_REG(0x08); src.writeCONTROL_REG(0x21);  // saw, gate on

            // Cycle c has wg accumulator top-12 = c & 0xfff, so c = 1..4096
            // covers all 4096 phases exactly once.
            for (int i = 0; i < 4096; i++) {
                wg.clock();
                src.clock();
                wg.set_waveform_output();
                printf("%d,%d,%d,%d\n", waveform, ring, (i + 1) & 0xfff, (int)wg.readOSC() << 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// dac: reSID's R-2R ladder models, emitted from reSID's own dac.h.
//
// This is the *reference* side only. t00t derives its own tables independently
// by nodal analysis of the same ladder (tools/fit_6581_filter.py's r2r_vbit),
// so the diff between them is a real conformance test rather than a table
// compared against a copy of itself.
//
// 6581 has 2R/R = 2.20 and a missing bit-0 termination; 8580 has 2.00 and
// correct termination. The 6581's is not merely imprecise but non-monotonic --
// 19 descending steps at 8 bits, 347 at 12 -- and that is a large part of why
// a quiet 6581 note sounds dirty rather than merely quiet (module_chip.md §3).
// ---------------------------------------------------------------------------
static void dump_dac(bool model_8580) {
    printf("# domain=dac cols=bits,index,value\n");
    const double ratio = model_8580 ? 2.00 : 2.20;
    const bool   term  = model_8580;

    DAC<8> d8(ratio, term);
    for (int i = 0; i < 256; i++) printf("8,%d,%d\n", i, (int)d8[i]);

    DAC<12> d12(ratio, term);
    for (int i = 0; i < 4096; i++) printf("12,%d,%d\n", i, (int)d12[i]);
}

static void usage() {
    fprintf(stderr,
        "usage: resid_dump --domain env|lfsr|wave|dac [--model 6581|8580] [--count n]\n"
        "  writes CSV to stdout; see the header comment for what each domain means\n");
}

int main(int argc, char **argv) {
    std::string domain;
    bool model_8580 = false;
    int count = 4096;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "resid_dump: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--domain") domain = next("--domain");
        else if (a == "--model")  model_8580 = (next("--model") == "8580");
        else if (a == "--count")  count = atoi(next("--count").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "resid_dump: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }

    printf("# resid_dump model=%s\n", model_8580 ? "8580" : "6581");
    if      (domain == "env")  dump_env(model_8580);
    else if (domain == "lfsr") dump_lfsr(model_8580, count);
    else if (domain == "wave") dump_wave(model_8580);
    else if (domain == "dac")  dump_dac(model_8580);
    else { usage(); return 2; }
    return 0;
}
