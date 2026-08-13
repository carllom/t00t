// t00t_chip_dump -- control-plane ground truth from src/chip/, as CSV.
//
// The t00t side of tools/sid_ref/resid_dump.cpp, which is "the authority on
// the domains" (see its own header comment) -- this file mirrors its four
// domains (env, lfsr, wave, dac) and CSV column layout exactly, column for
// column, so tools/sid_ctl_diff.py can diff them without either side needing
// to know about the other's engine.
//
// Host-only tooling, never linked into the device firmware.

#include "audio_common.h"   // SAMPLE_RATE
#include "chip/env_sid.h"
#include "chip/sid_osc.h"
#include "chip/sid_tables.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// env: cycles-to-level trajectories, same procedure as resid_dump.cpp's
// dump_env but sample-driven (EnvSid ticks once per 44.1 kHz sample, not
// once per master-clock cycle) -- resid_dump.cpp's own comment explains why
// "cycles" is the right shared unit: it is the same question on both clocks,
// where "level at sample N" is not. Measured in samples, then converted.
//
// Envelope timing does not depend on 6581 vs 8580 -- env_sid.h has no model
// parameter at all, matching real hardware (only the DAC curve differs by
// chip). The --model flag is accepted for CLI parity and otherwise unused
// here, same as resid_dump.cpp effectively is: reSID's EnvelopeGenerator
// takes a chip model but its rate tables do not vary by it either.
// ---------------------------------------------------------------------------
static void dump_env() {
    printf("# domain=env cols=stage,rate,level,cycles\n");

    const double cycles_per_sample = SID_CLOCK_PAL / SAMPLE_RATE;
    EnvSidRates rates = env_sid_make_rates(SID_CLOCK_PAL, SAMPLE_RATE);

    struct Stage { const char *name; int mode; };
    const Stage stages[] = { {"attack", 0}, {"decay", 1}, {"release", 2} };

    for (const Stage &s : stages) {
        for (int rate = 0; rate < 16; rate++) {
            EnvSid env;
            env.init();
            switch (s.mode) {
                case 0:  env.attack = (uint8_t)rate; env.decay = 0; env.sustain = 0; env.release = 0; break;
                case 1:  env.attack = 0; env.decay = (uint8_t)rate; env.sustain = 0; env.release = 0; break;
                default: env.attack = 0; env.decay = 0; env.sustain = 0; env.release = (uint8_t)rate; break;
            }

            long long sample = 0;
            const long long limit = (long long)(40.0 * SAMPLE_RATE);  // 40 s: 8 s is the slowest rate

            // Decay/release are only reachable through a completed attack --
            // prime with the fastest attack (rate 0) first, exactly as
            // resid_dump.cpp does, for the same reason: measuring from
            // gate-on directly would time the attack and call it a decay.
            env.gate_on();
            if (s.mode != 0) {
                while (env.counter != 255 && sample < limit) { env.tick(rates); sample++; }
            }
            if (s.mode == 2) env.gate_off();

            sample = 0;
            if (s.mode == 0) {
                int next = 1;
                printf("attack,%d,0,0\n", rate);
                while (next <= 255 && sample < limit) {
                    env.tick(rates); sample++;
                    while (next <= 255 && env.counter >= (uint8_t)next) {
                        printf("attack,%d,%d,%lld\n", rate, next,
                               (long long)llround((double)sample * cycles_per_sample));
                        next++;
                    }
                }
            } else {
                int next = 254;
                printf("%s,%d,255,0\n", s.name, rate);
                while (next >= 0 && sample < limit) {
                    env.tick(rates); sample++;
                    while (next >= 0 && env.counter <= (uint8_t)next) {
                        printf("%s,%d,%d,%lld\n", s.name, rate, next,
                               (long long)llround((double)sample * cycles_per_sample));
                        next--;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// lfsr: the noise shift register sequence. sid_lfsr_step()/sid_lfsr_output()
// are already exactly "one shift" and "the scattered 12-bit output" with no
// clock-rate dependence, so this needs no oscillator simulation at all --
// SID_LFSR_SEED is already sid_osc.h's own "clocked once after reset" value
// (0x7ffffe, matching resid_dump.cpp's own comment on the same point), so
// index 0 needs no extra step to line up with the reference's convention of
// emitting the post-reset state before any shift.
// ---------------------------------------------------------------------------
static void dump_lfsr(int count) {
    printf("# domain=lfsr cols=index,noise_out12\n");
    uint32_t sr = SID_LFSR_SEED;
    for (int i = 0; i < count; i++) {
        printf("%d,%d\n", i, (int)sid_lfsr_output(sr));
        sr = sid_lfsr_step(sr);
    }
}

// ---------------------------------------------------------------------------
// wave: the 12-bit waveform output over a full accumulator cycle, for
// waveforms 1-7 (TRI/SAW/PULSE bit combinations) x ring 0/1. Purely
// combinational (sid_combined_and()), so no oscillator state is needed
// either -- just top12 and a ring MSB-flip value.
//
// pw=0, matching resid_dump.cpp's wg: it never writes the pulse-width
// register, so it stays at reSID's post-reset default (0). ring_msb_flip is
// constant per sweep (0x800 for ring=1, 0 for ring=0): resid_dump.cpp holds
// its ring/sync source at freq=0 with a TEST-zeroed accumulator, so the
// source's MSB is low for the entire sweep -- ring_flip_for_dest()'s "low
// source MSB -> flip" case, unconditionally.
//
// resid_dump.cpp reads this domain through `wg.readOSC() << 4` too -- the
// same 8-high-bits-only OSC3 register dump_lfsr's comment already flags, not
// the full internal 12-bit value. Mask the low nibble to match, or every
// pure waveform reads as "differs by up to 15" for a reason that has nothing
// to do with the waveform logic being wrong.
// ---------------------------------------------------------------------------
static void dump_wave() {
    printf("# domain=wave cols=waveform,ring,acc_top12,out12\n");
    for (int waveform = 1; waveform <= 7; waveform++) {
        for (int ring = 0; ring < 2; ring++) {
            uint16_t ring_flip = ring ? 0x800 : 0x000;
            for (int i = 0; i < 4096; i++) {
                uint16_t top12 = (uint16_t)((i + 1) & 0xfff);
                uint16_t out = sid_combined_and((uint8_t)waveform, top12, /*pw=*/0, ring_flip);
                printf("%d,%d,%d,%d\n", waveform, ring, top12, (int)(out & 0xff0));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// dac: the R-2R DAC tables as compiled into the firmware -- dumped from
// sid_tables.h's actual arrays, not regenerated, so a stale/edited/truncated
// table shows up here (same reasoning as resid_dump.cpp's own comment).
// 8580's 12-bit waveform DAC has no t00t table at all (sid_voice.h uses the
// identity for it -- see output_and_tick_env()'s `model ? wave : ...`), so
// that is what gets emitted here too, matching the actual code path exactly.
// ---------------------------------------------------------------------------
static void dump_dac(bool model_8580) {
    printf("# domain=dac cols=bits,index,value\n");
    for (int i = 0; i < 256; i++) {
        int v = model_8580 ? SID_ENV_DAC_8580[i] : SID_ENV_DAC_6581[i];
        printf("8,%d,%d\n", i, v);
    }
    for (int i = 0; i < 4096; i++) {
        int v = model_8580 ? i : SID_WAVE_DAC_6581[i];
        printf("12,%d,%d\n", i, v);
    }
}

static void usage() {
    fprintf(stderr,
        "usage: t00t_chip_dump --domain env|lfsr|wave|dac [--model 6581|8580] [--count n]\n"
        "  writes CSV to stdout; see tools/sid_ref/resid_dump.cpp for what each domain means\n");
}

int main(int argc, char **argv) {
    std::string domain;
    bool model_8580 = false;
    int count = 4096;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "t00t_chip_dump: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--domain") domain = next("--domain");
        else if (a == "--model")  model_8580 = (next("--model") == "8580");
        else if (a == "--count")  count = atoi(next("--count").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "t00t_chip_dump: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }

    printf("# t00t_chip_dump model=%s\n", model_8580 ? "8580" : "6581");
    if      (domain == "env")  dump_env();
    else if (domain == "lfsr") dump_lfsr(count);
    else if (domain == "wave") dump_wave();
    else if (domain == "dac")  dump_dac(model_8580);
    else { usage(); return 2; }
    return 0;
}
