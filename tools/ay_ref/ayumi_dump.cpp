// ayumi_dump -- control-plane ground truth from ayumi, as CSV. No audio.
//
// Mirrors tools/sid_ref/resid_dump.cpp's role and domain split exactly
// (module_chip.md §11.1's control-plane/signal-plane distinction, applied to the
// second chip in this module): most of an AY-3-8910/YM2149 voice is
// control-rate or purely combinational logic, so it compares exactly,
// numerically, against tools/host_render/t00t_ay_dump.cpp -- no audio
// rendering, no tolerance to argue about except where the domain itself
// says otherwise.
//
//   tone      the toggle sequence for a period, over enough ticks to see
//             several full cycles. Bit-exact.
//   noise     the shared LFSR's raw output-bit sequence, seeded like
//             ayumi_configure's ay->noise = 1. Bit-exact.
//   envelope  the (shape, tick) -> level trajectory for all 16 shapes, long
//             enough to see wraparound on the continuing ones. Bit-exact --
//             this is a ramp counter, not reSID's piecewise-exponential
//             envelope, so there is no continuous-approximation tolerance
//             to allow here the way tools/sid_ref/resid_dump.cpp's env
//             domain needs one.
//   dac       ayumi.c's own AY_dac_table/YM_dac_table, verbatim -- the
//             reference tools/ay_ctl_diff.py checks src/chip/ay_envelope.h's
//             independently-re-typed copy against.
//
// Host-only tooling. ayumi.c is vendored (not fetched) at
// tools/ay_ref/ayumi/ -- its MIT license permits that outright, unlike
// reSID's GPL-2 (tools/sid_ref/fetch_resid.sh's gitignored checkout).
//
// update_tone/update_noise/update_envelope are file-local statics in
// ayumi.c, so this file #includes it directly rather than linking against
// a separately-compiled object -- the same trick tools/host_render/
// render_sid.cpp already uses on src/chip/sid_voice.h under CHIP_STRICT, to
// reach state a public API wouldn't expose.

#include "ayumi/ayumi.c"

#include <cstdio>
#include <cstring>
#include <string>

static void dump_tone() {
    printf("# domain=tone cols=period,tick,out\n");
    static const int periods[] = { 1, 2, 3, 16, 100, 4095 };
    struct ayumi ay;
    for (int period : periods) {
        ayumi_configure(&ay, 0, 1773400.0, 44100);
        ayumi_set_tone(&ay, 0, period);
        ay.channels[0].tone_counter = 0;
        ay.channels[0].tone = 0;
        int ticks = period * 4 + 8;
        if (ticks > 4096) ticks = 4096;
        for (int t = 0; t < ticks; t++) {
            int out = update_tone(&ay, 0);
            printf("%d,%d,%d\n", period, t, out);
        }
    }
}

static void dump_noise() {
    printf("# domain=noise cols=index,out\n");
    struct ayumi ay;
    ayumi_configure(&ay, 0, 1773400.0, 44100);
    ayumi_set_noise(&ay, 1);   // period 1: one LFSR shift per two ticks (update_noise's own <<1)
    ay.noise = 1;
    ay.noise_counter = 0;
    for (int i = 0; i < 200000; i++) {
        int out = update_noise(&ay);
        printf("%d,%d\n", i, out);
    }
}

static void dump_envelope() {
    printf("# domain=envelope cols=shape,tick,level\n");
    struct ayumi ay;
    for (int shape = 0; shape < 16; shape++) {
        ayumi_configure(&ay, 0, 1773400.0, 44100);
        ayumi_set_envelope(&ay, 1);
        ayumi_set_envelope_shape(&ay, shape);
        for (int t = 0; t < 128; t++) {
            int level = update_envelope(&ay);
            printf("%d,%d,%d\n", shape, t, level);
        }
    }
}

static void dump_dac() {
    printf("# domain=dac cols=model,index,value\n");
    for (int i = 0; i < 32; i++) printf("ay,%d,%.10g\n", i, AY_dac_table[i]);
    for (int i = 0; i < 32; i++) printf("ym,%d,%.10g\n", i, YM_dac_table[i]);
}

static void usage() {
    fprintf(stderr, "ayumi_dump --domain tone|noise|envelope|dac\n");
}

int main(int argc, char **argv) {
    std::string domain;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--domain" && i + 1 < argc) domain = argv[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "ayumi_dump: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }

    printf("# ayumi_dump\n");
    if      (domain == "tone")     dump_tone();
    else if (domain == "noise")    dump_noise();
    else if (domain == "envelope") dump_envelope();
    else if (domain == "dac")      dump_dac();
    else { usage(); return 2; }
    return 0;
}
