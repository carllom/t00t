// t00t_ctl_dump -- t00t's control-plane trajectories as CSV (fm2.md §3.1, F1).
//
// The t00t side of the exact harness. Emits the same columns, in the same
// units, as tools/fm_ref/dexed_dump; tools/fm_ctl_diff.py runs both and
// compares. See dexed_dump.cpp's header for the shared-domain definitions --
// they are the whole design, and any change to them has to happen on both
// sides at once.
//
// Everything here calls the real device headers (env_dx.h, lfo.h, pitch_eg.h)
// with no reimplementation, so a divergence found by fm_ctl_diff.py is a
// divergence in the engine and not in this tool.
//
// The `algo` test has no entry here on purpose: t00t's 32-algorithm table lives
// in tools/syx2patch.py, not in C++ (patch.h only sees the already-decoded
// per-operator `mod_target`). fm_ctl_diff.py imports it from there directly.
//
// Build: make -f Makefile.fm t00t_ctl_dump

// op.h rather than env_dx.h/lfo.h/pitch_eg.h individually: it pulls in all
// three, and it is where FM_BLOCK lives. Taking the control-block size from the
// real header (instead of restating 16 here) means a -DT00T_FM_BLOCK=8/32 build
// dumps at the block size it actually runs, which is exactly the kind of thing
// this harness exists to keep honest.
#include "../../src/engines/fm/op.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr double DB_PER_OCTAVE = 6.020599913;

// env_dx.h works in Q8 octaves (EG_LOG2_ONE = 256 units per octave). Dexed's
// `outlevel` microsteps are the same size -- one unit of Env::advance()'s
// `actuallevel` is 1/256 octave, since `targetlevel_ = actuallevel << 16` lands
// in a Q24-per-octave field. So the two sides' integer level units are already
// 1:1 and need no rescaling; only the *anchor* has to be made common, which is
// what reference_log2() does.
//
// t00t's anchor: output level 99, EG level 99, no key scaling, no velocity
// sensitivity. env_dx_init_level_table() defines DX7_LEVEL_TO_LOG2[99] == 0 and
// fm_voice_note_on()'s static_log2 is 0 for that config, so the reference is
// simply 0 -- stated explicitly rather than assumed, so the two sides' anchor
// definitions sit side by side and can be checked against each other.
static constexpr int32_t reference_log2() { return 0; }

static void parse4(const char *s, uint8_t out[4]) {
    int v[4];
    if (sscanf(s, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) != 4) {
        fprintf(stderr, "error: expected four comma-separated values, got '%s'\n", s);
        exit(2);
    }
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)v[i];
}

static void usage() {
    printf(
        "t00t_ctl_dump -- t00t control-plane trajectories as CSV\n"
        "\n"
        "  --what WHICH      levels | scalerate | scalelevel | velocity\n"
        "                    | eg | pitcheg | lfo\n"
        "                    (algo lives in tools/syx2patch.py -- see header)\n"
        "\n"
        "  eg:       --rates 99,99,99,99 --levels 99,99,99,99\n"
        "            --outlevel 99 --ratescale 0 --gate 2.0 --dur 4.0\n"
        "  pitcheg:  --rates .. --levels .. --gate --dur\n"
        "  lfo:      --lfo speed,delay,pmd,amd,sync,waveform --dur\n");
}

int main(int argc, char **argv) {
    std::string what;
    uint8_t rates[4] = {99, 99, 99, 99};
    uint8_t levels[4] = {99, 99, 99, 99};
    int outlevel = 99, ratescale = 0;
    double gate = 2.0, dur = 4.0;
    int lfo_p[6] = {35, 0, 0, 0, 1, 0};

    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--what")      what = next();
        else if (k == "--rates")     parse4(next(), rates);
        else if (k == "--levels")    parse4(next(), levels);
        else if (k == "--outlevel")  outlevel = atoi(next());
        else if (k == "--ratescale") ratescale = atoi(next());
        else if (k == "--gate")      gate = atof(next());
        else if (k == "--dur")       dur = atof(next());
        else if (k == "--lfo") {
            if (sscanf(next(), "%d,%d,%d,%d,%d,%d", &lfo_p[0], &lfo_p[1], &lfo_p[2],
                       &lfo_p[3], &lfo_p[4], &lfo_p[5]) != 6) {
                fprintf(stderr, "error: --lfo wants speed,delay,pmd,amd,sync,waveform\n");
                return 2;
            }
        }
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); return 2; }
    }
    if (what.empty()) { usage(); return 2; }

    env_dx_init_tables();

    if (what == "levels") {
        printf("level,scaleoutlevel\n");
        for (int l = 0; l < 100; l++) printf("%d,%d\n", l, dx7_scaleoutlevel(l));

    } else if (what == "scalerate") {
        printf("midinote,sensitivity,qrate_delta\n");
        for (int n = 0; n < 128; n++)
            for (int s = 0; s < 8; s++)
                printf("%d,%d,%d\n", n, s, dx7_scale_rate(n, s));

    } else if (what == "velocity") {
        // Dexed's ScaleVelocity returns an outlevel delta in microsteps;
        // env_dx.h's eg_vel_sensitivity_log2() returns Q8 octaves. Those units
        // are identical (see reference_log2()'s comment), so the values compare
        // directly. Velocity 0-127 maps to the Q15 amplitude VoiceParams
        // carries, the same way midi_controller.cpp/render_fm_patch.cpp do.
        printf("velocity,sensitivity,outlevel_delta\n");
        for (int v = 0; v < 128; v++) {
            int16_t amp = (int16_t)((v / 127.0f) * 32767.0f);
            for (int s = 0; s < 8; s++)
                printf("%d,%d,%d\n", v, s, eg_vel_sensitivity_log2((uint8_t)s, amp));
        }

    } else if (what == "scalelevel") {
        printf("midinote,breakpoint,ldepth,rdepth,lcurve,rcurve,level_delta\n");
        for (int n = 0; n < 128; n += 3)
            for (int bp = 0; bp < 100; bp += 11)
                for (int d = 0; d < 100; d += 13)
                    for (int lc = 0; lc < 4; lc++)
                        for (int rc = 0; rc < 4; rc++)
                            printf("%d,%d,%d,%d,%d,%d,%d\n", n, bp, d, d, lc, rc,
                                   dx7_scale_level(n, bp, d, d, lc, rc));

    } else if (what == "eg") {
        FmOpParams p{};
        for (int i = 0; i < 4; i++) { p.eg_rate[i] = rates[i]; p.eg_level[i] = levels[i]; }
        p.output_level = (uint8_t)outlevel;

        EnvDX eg;
        env_dx_init(eg);
        env_dx_trigger(eg);

        // fm_voice_note_on()'s own static_log2, for the no-key-scaling,
        // no-velocity-sensitivity case this test isolates.
        const int32_t static_log2 =
            (dx7_scaleoutlevel(outlevel) - dx7_scaleoutlevel(99)) * 32;

        printf("time_s,db\n");
        bool released = false;
        uint32_t total = (uint32_t)(dur * SAMPLE_RATE);
        for (uint32_t s = 0; s < total; s += FM_BLOCK) {
            double t = (double)s / SAMPLE_RATE;
            if (!released && t >= gate) { env_dx_release(eg); released = true; }
            int32_t start, end;
            env_dx_step_block(eg, p, FM_BLOCK, start, end, ratescale);
            // `end`, not `start`: Dexed's Env::getsample() returns the level
            // *after* advancing its block, and fm_core.cc uses that value as
            // the block's `gain2` (its end-of-block gain). Emitting `start`
            // here would report t00t one control block behind Dexed and show up
            // as a spurious ~100 dB spike at t=0 on every EG case.
            double db = ((double)(static_log2 + end - reference_log2()) / 256.0) * DB_PER_OCTAVE;
            printf("%.6f,%.4f\n", t, db);
        }

    } else if (what == "pitcheg") {
        FmPitchEgParams p{};
        for (int i = 0; i < 4; i++) { p.rate[i] = rates[i]; p.level[i] = levels[i]; }

        FmPitchEg peg;
        fm_pitch_eg_init(peg);
        fm_pitch_eg_trigger(peg, p);

        printf("time_s,cents\n");
        bool released = false;
        uint32_t total = (uint32_t)(dur * SAMPLE_RATE);
        for (uint32_t s = 0; s < total; s += FM_BLOCK) {
            double t = (double)s / SAMPLE_RATE;
            if (!released && t >= gate) { fm_pitch_eg_release(peg); released = true; }
            printf("%.6f,%.4f\n", t, (double)fm_pitch_eg_step_block(peg, p, FM_BLOCK));
        }

    } else if (what == "lfo") {
        FmLfoParams p{};
        p.rate = (uint8_t)lfo_p[0];
        p.delay = (uint8_t)lfo_p[1];
        p.pmd = (uint8_t)lfo_p[2];
        p.amd = (uint8_t)lfo_p[3];
        p.key_sync = lfo_p[4] != 0;
        p.waveform = (uint8_t)lfo_p[5];
        p.pms = 7;

        FmLfo lfo;
        fm_lfo_init(lfo);
        fm_lfo_trigger(lfo, p.key_sync);

        printf("time_s,value,delay\n");
        uint32_t total = (uint32_t)(dur * SAMPLE_RATE);
        for (uint32_t s = 0; s < total; s += FM_BLOCK) {
            float pitch_cents, amp_atten;
            fm_lfo_step_block(lfo, p, FM_BLOCK, /*mod_wheel_frac=*/1.0f, pitch_cents, amp_atten);
            // Raw unipolar waveform + delay ramp, matching Dexed's getsample()/
            // getdelay() pair rather than t00t's already-scaled cents output --
            // the depth/sensitivity scaling on top is a separate question from
            // whether the oscillator itself matches.
            double value = (p.waveform == 5)
                ? (double)lfo.sh_value
                : (double)fm_lfo_waveform_unipolar(lfo.phase, p.waveform);
            printf("%.6f,%.6f,%.6f\n", (double)s / SAMPLE_RATE, value,
                   (double)lfo.delay_progress);
        }

    } else {
        fprintf(stderr, "error: unknown --what '%s'\n", what.c_str());
        usage();
        return 2;
    }
    return 0;
}
