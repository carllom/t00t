// t00t_ay_dump -- control-plane dump from t00t's own AY primitives, as CSV.
//
// Column-for-column with tools/ay_ref/ayumi_dump.cpp; tools/ay_ctl_diff.py
// diffs the two. See that file's header for the domain definitions this
// must match. Host-only; no device build involved.

#include "chip/ay_osc.h"
#include "chip/ay_envelope.h"

#include <cstdio>
#include <string>

static void dump_tone() {
    printf("# domain=tone cols=period,tick,out\n");
    static const int periods[] = { 1, 2, 3, 16, 100, 4095 };
    for (int period : periods) {
        AyTone tone;
        tone.init();
        tone.set_period((uint16_t)period);
        int ticks = period * 4 + 8;
        if (ticks > 4096) ticks = 4096;
        for (int t = 0; t < ticks; t++) {
            tone.advance(1);
            printf("%d,%d,%d\n", period, t, tone.out);
        }
    }
}

static void dump_noise() {
    printf("# domain=noise cols=index,out\n");
    AyNoise noise;
    noise.init();
    noise.set_period(1);
    for (int i = 0; i < 200000; i++) {
        noise.advance(1);
        printf("%d,%d\n", i, noise.out());
    }
}

static void dump_envelope() {
    printf("# domain=envelope cols=shape,tick,level\n");
    for (int shape = 0; shape < 16; shape++) {
        AyEnvelope env;
        env.init();
        env.set_period(1);
        env.set_shape((uint8_t)shape);
        for (int t = 0; t < 128; t++) {
            env.advance(1);
            printf("%d,%d,%d\n", shape, t, env.level);
        }
    }
}

static void dump_dac() {
    printf("# domain=dac cols=model,index,value\n");
    for (int i = 0; i < 32; i++) printf("ay,%d,%.10g\n", i, (double)AY_DAC_TABLE[i]);
    for (int i = 0; i < 32; i++) printf("ym,%d,%.10g\n", i, (double)YM_DAC_TABLE[i]);
}

static void usage() {
    fprintf(stderr, "t00t_ay_dump --domain tone|noise|envelope|dac\n");
}

int main(int argc, char **argv) {
    std::string domain;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--domain" && i + 1 < argc) domain = argv[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "t00t_ay_dump: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }

    printf("# t00t_ay_dump\n");
    if      (domain == "tone")     dump_tone();
    else if (domain == "noise")    dump_noise();
    else if (domain == "envelope") dump_envelope();
    else if (domain == "dac")      dump_dac();
    else { usage(); return 2; }
    return 0;
}
