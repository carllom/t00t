// t00t_opl_ctl_dump -- control-plane dump from t00t's own OPL primitives, as
// CSV. Column-for-column with tools/opl_ref/nuked_dump.cpp; a future
// control-plane diff script runs both and compares.
//
// Everything here calls the real device headers (src/engines/opl/patch.h,
// env_opl.h) directly, no reimplementation, so a divergence a future diff
// finds is a divergence in the engine, not in this tool.
//
// Domains: mult, ksl, tl, eg -- see tools/opl_ref/nuked_dump.cpp's header
// comment for what each one means; the two files are meant to be read
// together.
//
// Host-only tooling. No pico-sdk dependency.

#include "../../src/engines/opl/env_opl.h"
#include "../../src/engines/opl/patch.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static void dump_mult() {
    printf("# domain=mult cols=reg,doubled_ratio\n");
    for (int i = 0; i < 16; i++) printf("%d,%.0f\n", i, opl_mult_table[i] * 2.0f);
}

static void dump_ksl() {
    printf("# domain=ksl cols=table,index,value\n");
    for (int i = 0; i < 16; i++) printf("rom,%d,%u\n", i, OPL_KSL_ROM[i]);
    for (int i = 0; i < 4; i++) printf("shift,%d,%u\n", i, OPL_KSL_SHIFT[i]);
}

static void dump_tl() {
    printf("# domain=tl cols=tl_reg,db\n");
    for (int i = 0; i < 64; i++) printf("%d,%.4f\n", i, (double)i * 0.75);
}

static void dump_eg(int mult, int ksl, int tl, int ar, int dr, int sl, int rr,
                     int egt, int ksr, int midinote, int velocity, double gate, double dur) {
    OplOpParams p{};
    p.mult = (uint8_t)mult;
    p.ksl = (uint8_t)ksl;
    p.tl = (uint8_t)tl;
    p.ar = (uint8_t)ar;
    p.dr = (uint8_t)dr;
    p.sl = (uint8_t)sl;
    p.rr = (uint8_t)rr;
    p.egt = (bool)egt;
    p.ksr = (bool)ksr;
    p.ws = 0;

    int16_t amplitude = (int16_t)((velocity * 32767 + 63) / 127);  // inverse of env_opl.h's velocity recovery
    EnvOpl eg;
    env_opl_init(eg, p, (uint8_t)midinote, amplitude);

    printf("# domain=eg cols=time_s,level,db\n");
    uint32_t gate_frames = (uint32_t)(gate * SAMPLE_RATE);
    uint32_t total = (uint32_t)(dur * SAMPLE_RATE);
    bool released = false;
    for (uint32_t s = 0; s < total; s++) {
        if (!released && s >= gate_frames) { env_opl_release(eg); released = true; }
        int32_t level = env_opl_step_block(eg, 1);
        double t = (double)s / SAMPLE_RATE;
        double db = ((double)(level - (int32_t)EG_LEVEL_MAX) / (double)EG_LEVEL_ONE_OCTAVE) * 6.0206;
        printf("%.6f,%d,%.4f\n", t, level, db);
    }
}

static void usage() {
    printf(
        "t00t_opl_ctl_dump -- t00t's own OPL control-plane state as CSV\n"
        "\n"
        "  --domain WHICH    mult | ksl | tl | eg\n"
        "\n"
        "  eg: --mult N --ksl N --tl N --ar N --dr N --sl N --rr N --egt 0|1\n"
        "      --ksr 0|1 --note N --vel N --gate SEC --dur SEC\n");
}

int main(int argc, char **argv) {
    std::string domain;
    int mult = 1, ksl = 0, tl = 0, ar = 15, dr = 8, sl = 0, rr = 8, egt = 1, ksr = 0;
    int note = 57, velocity = 127;
    double gate = 1.0, dur = 3.0;

    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--domain") domain = next();
        else if (k == "--mult")   mult = atoi(next());
        else if (k == "--ksl")    ksl = atoi(next());
        else if (k == "--tl")     tl = atoi(next());
        else if (k == "--ar")     ar = atoi(next());
        else if (k == "--dr")     dr = atoi(next());
        else if (k == "--sl")     sl = atoi(next());
        else if (k == "--rr")     rr = atoi(next());
        else if (k == "--egt")    egt = atoi(next());
        else if (k == "--ksr")    ksr = atoi(next());
        else if (k == "--note")   note = atoi(next());
        else if (k == "--vel")    velocity = atoi(next());
        else if (k == "--gate")   gate = atof(next());
        else if (k == "--dur")    dur = atof(next());
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }
    if (domain.empty()) { usage(); return 2; }

    printf("# t00t_opl_ctl_dump\n");
    if      (domain == "mult") dump_mult();
    else if (domain == "ksl")  dump_ksl();
    else if (domain == "tl")   dump_tl();
    else if (domain == "eg")   dump_eg(mult, ksl, tl, ar, dr, sl, rr, egt, ksr, note, velocity, gate, dur);
    else { fprintf(stderr, "error: unknown --domain '%s'\n", domain.c_str()); usage(); return 2; }
    return 0;
}
