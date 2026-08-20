// nuked_dump -- ground-truth control-plane state from Nuked-OPL3, as CSV.
//
// tools/host_render/t00t_opl_ctl_dump.cpp emits the same columns from t00t's
// own headers; a future control-plane diff script runs both and compares.
// This tool and its t00t-side counterpart are the infrastructure for that
// comparison, not the comparison itself -- there is no diff script yet.
//
// Domains:
//   mult      Nuked's own frequency-multiplier table (mt[]), doubled-integer
//             form -- exact.
//   ksl       Nuked's own kslrom[]/kslshift[] tables -- exact.
//   tl        the TL register's own linear 0.75 dB/step scale -- exact by
//             hardware definition, dumped for completeness/symmetry.
//   eg        one operator's envelope trajectory (attack/decay/sustain/
//             release), driven through the real register interface
//             (OPL3_WriteReg) and read back from the chip's own live state
//             (slot.eg_rout/eg_out) -- not a table lookup, a real simulation.
//
// mt[]/kslrom[]/kslshift[] are file-local statics in nuked/opl3.c, so this
// file #includes it directly rather than linking against a separately
// compiled object, to reach state the public API (opl3.h) doesn't expose.
//
// Build: make (see Makefile). Run: ./nuked_dump --help

#include "opl3.c"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr double SR = 44100.0;
static constexpr double OPL_CLOCK_OVER_72 = 49716.0;
static constexpr double EG_ROUT_DB_PER_UNIT = 0.1875;  // TL's 0.75 dB/step, composed at 4x resolution (eg_out = eg_rout + (tl<<2) + ...)

static int velocity_adjusted_tl(int tl, int velocity) {
    double vel_db = (double)(127 - velocity) * 0.375;
    int adjusted = tl + (int)lround(vel_db / 0.75);
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 63) adjusted = 63;
    return adjusted;
}

static void freq_to_fnum_block(double freq_hz, uint16_t &fnum, uint8_t &block) {
    for (int b = 0; b <= 7; b++) {
        double f = freq_hz * (double)(1u << (20 - b)) / OPL_CLOCK_OVER_72;
        if (f <= 1023.0) { block = (uint8_t)b; fnum = (uint16_t)lround(f); return; }
    }
    block = 7;
    fnum = 1023;
}

static void dump_mult() {
    printf("# domain=mult cols=reg,doubled_ratio\n");
    for (int i = 0; i < 16; i++) printf("%d,%u\n", i, mt[i]);
}

static void dump_ksl() {
    printf("# domain=ksl cols=table,index,value\n");
    for (int i = 0; i < 16; i++) printf("rom,%d,%u\n", i, kslrom[i]);
    for (int i = 0; i < 4; i++) printf("shift,%d,%u\n", i, kslshift[i]);
}

static void dump_tl() {
    printf("# domain=tl cols=tl_reg,db\n");
    for (int i = 0; i < 64; i++) printf("%d,%.4f\n", i, (double)i * 0.75);
}

static void dump_eg(int mult, int ksl, int tl, int ar, int dr, int sl, int rr,
                     int egt, int ksr, int midinote, int velocity, double gate, double dur) {
    opl3_chip chip;
    OPL3_Reset(&chip, (uint32_t)SR);

    // Operator 2 (slot offset 0x03) is channel 0's carrier slot in additive
    // mode -- writing only this slot and forcing con=1 lets one operator's
    // envelope be read in isolation, unaffected by anything in operator 1's
    // (unwritten, default-silent) slot.
    int tl_adj = velocity_adjusted_tl(tl, velocity);
    uint8_t r20 = (uint8_t)(((egt & 1) << 5) | ((ksr & 1) << 4) | (mult & 0x0F));
    OPL3_WriteReg(&chip, 0x23, r20);
    OPL3_WriteReg(&chip, 0x43, (uint8_t)(((ksl & 3) << 6) | (tl_adj & 0x3F)));
    OPL3_WriteReg(&chip, 0x63, (uint8_t)(((ar & 0x0F) << 4) | (dr & 0x0F)));
    OPL3_WriteReg(&chip, 0x83, (uint8_t)(((sl & 0x0F) << 4) | (rr & 0x0F)));
    OPL3_WriteReg(&chip, 0xE3, 0x00);
    OPL3_WriteReg(&chip, 0xC0, 0x01);  // additive (con=1): slot 3 carries directly

    double freq_hz = 440.0 * pow(2.0, (double)(midinote - 69) / 12.0);
    uint16_t fnum;
    uint8_t block;
    freq_to_fnum_block(freq_hz, fnum, block);
    OPL3_WriteReg(&chip, 0xA0, (uint8_t)(fnum & 0xFF));
    OPL3_WriteReg(&chip, 0xB0, (uint8_t)(0x20 | ((block & 7) << 2) | ((fnum >> 8) & 3)));

    printf("# domain=eg cols=time_s,eg_rout,eg_out,db\n");
    uint32_t gate_frames = (uint32_t)(gate * SR);
    uint32_t total = (uint32_t)(dur * SR);
    int16_t buf[2];
    bool released = false;
    for (uint32_t s = 0; s < total; s++) {
        if (!released && s >= gate_frames) {
            OPL3_WriteReg(&chip, 0xB0, (uint8_t)(((block & 7) << 2) | ((fnum >> 8) & 3)));  // key-off
            released = true;
        }
        OPL3_GenerateStream(&chip, buf, 1);
        double t = (double)s / SR;
        uint16_t rout = chip.slot[3].eg_rout;
        uint16_t out = chip.slot[3].eg_out;
        printf("%.6f,%u,%u,%.4f\n", t, rout, out, (double)out * EG_ROUT_DB_PER_UNIT);
    }
}

static void usage() {
    printf(
        "nuked_dump -- Nuked-OPL3 control-plane state as CSV\n"
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

    printf("# nuked_dump\n");
    if      (domain == "mult") dump_mult();
    else if (domain == "ksl")  dump_ksl();
    else if (domain == "tl")   dump_tl();
    else if (domain == "eg")   dump_eg(mult, ksl, tl, ar, dr, sl, rr, egt, ksr, note, velocity, gate, dur);
    else { fprintf(stderr, "error: unknown --domain '%s'\n", domain.c_str()); usage(); return 2; }
    return 0;
}
