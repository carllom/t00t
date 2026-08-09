// dexed_dump -- ground-truth control-plane trajectories from Dexed (fm2.md §3.1, F1).
//
// The exact half of the harness. Everything the DX7 does except the per-sample
// operator kernel runs at control rate and produces *numbers* -- level curves,
// rate curves, key scaling, velocity, LFO, pitch EG, algorithm routing. Those
// can be diffed against t00t's numbers directly, with no audio, no spectra and
// no perceptual judgement anywhere in the loop.
//
// This side emits CSV; tools/host_render/t00t_ctl_dump emits the same columns
// from t00t's own headers; tools/fm_ctl_diff.py runs both and compares.
//
// Shared domains (this is the whole design -- both sides must mean the same
// thing by every number):
//
//   eg       dB relative to a fixed full-scale reference, over time in seconds.
//            The reference is the same config on both sides: all rates 99, all
//            levels 99, output level 99, no key/rate scaling, no velocity
//            sensitivity, held. Using ONE shared anchor (rather than each
//            config's own maximum) is deliberate: it keeps relative level
//            errors -- a sustain stage landing 60 dB too low, say -- visible,
//            instead of normalising them away.
//   pitcheg  cents, over time in seconds.
//   lfo      unipolar 0..1, plus the delay ramp 0..1, over time in seconds.
//   algo     per (algorithm, operator): in bus, out bus, feedback flags, carrier.
//   the integer tables  raw DX7 units, exactly as each side computes them.
//
// Build: make (see Makefile). Run: ./dexed_dump --help

#include "msfa/synth.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/lfo.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/freqlut.h"
#include "msfa/fm_core.h"
#include "msfa/controllers.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Non-static free functions in dx7note.cc -- declared here rather than pulling
// in dx7note.h, which would drag in tuning/MTS for no reason.
int ScaleVelocity(int velocity, int sensitivity);
int ScaleRate(int midinote, int sensitivity);
int ScaleCurve(int group, int depth, int curve);
int ScaleLevel(int midinote, int break_pt, int left_depth, int right_depth,
               int left_curve, int right_curve);

static constexpr double SR = 44100.0;
static constexpr double DB_PER_OCTAVE = 6.020599913;

// FmCore::algorithms is a protected static, and there is no public accessor for
// the raw flag bytes (only isCarrier()). Deriving is the sanctioned way to read
// it -- no const_cast, no copy of the table to drift out of sync with the
// pinned source.
struct AlgoPeek : public FmCore {
    static int flags(int alg, int op) { return algorithms[alg].ops[op]; }
};

// Env::advance()'s own composition, evaluated for the reference config: all EG
// levels 99, output level 99. `actuallevel = (scaleoutlevel(L) >> 1) << 6) +
// outlevel_ - 4256`, with outlevel_ = scaleoutlevel(99) << 5. Recomputed here
// rather than hardcoded so it tracks the pinned Dexed source if that changes.
static int32_t reference_level_q24() {
    int outlevel = Env::scaleoutlevel(99) << 5;
    int actuallevel = ((Env::scaleoutlevel(99) >> 1) << 6) + outlevel - 4256;
    if (actuallevel < 16) actuallevel = 16;
    return (int32_t)actuallevel << 16;
}

static void parse4(const char *s, int out[4]) {
    if (sscanf(s, "%d,%d,%d,%d", &out[0], &out[1], &out[2], &out[3]) != 4) {
        fprintf(stderr, "error: expected four comma-separated values, got '%s'\n", s);
        exit(2);
    }
}

static void usage() {
    printf(
        "dexed_dump -- Dexed control-plane trajectories as CSV\n"
        "\n"
        "  --what WHICH      levels | scalerate | scalelevel | velocity\n"
        "                    | eg | pitcheg | lfo | algo\n"
        "\n"
        "  eg:       --rates 99,99,99,99 --levels 99,99,99,99\n"
        "            --outlevel 99 --ratescale 0 --gate 2.0 --dur 4.0\n"
        "  pitcheg:  --rates .. --levels .. --gate --dur\n"
        "  lfo:      --lfo speed,delay,pmd,amd,sync,waveform --dur\n");
}

int main(int argc, char **argv) {
    std::string what;
    int rates[4] = {99, 99, 99, 99};
    int levels[4] = {99, 99, 99, 99};
    int outlevel = 99, ratescale = 0;
    double gate = 2.0, dur = 4.0;
    int lfo_p[6] = {35, 0, 0, 0, 1, 0};  // speed, delay, pmd, amd, sync, waveform

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

    Exp2::init();
    Sin::init();
    Freqlut::init(SR);
    Lfo::init(SR);
    PitchEnv::init(SR);
    Env::init_sr(SR);

    if (what == "levels") {
        // The 0-99 output-level / EG-level curve, in raw scaleoutlevel units.
        printf("level,scaleoutlevel\n");
        for (int l = 0; l < 100; l++) printf("%d,%d\n", l, Env::scaleoutlevel(l));

    } else if (what == "scalerate") {
        printf("midinote,sensitivity,qrate_delta\n");
        for (int n = 0; n < 128; n++)
            for (int s = 0; s < 8; s++)
                printf("%d,%d,%d\n", n, s, ScaleRate(n, s));

    } else if (what == "velocity") {
        printf("velocity,sensitivity,outlevel_delta\n");
        for (int v = 0; v < 128; v++)
            for (int s = 0; s < 8; s++)
                printf("%d,%d,%d\n", v, s, ScaleVelocity(v, s));

    } else if (what == "scalelevel") {
        // Sampled rather than exhaustive: the full cross product is 128 notes x
        // 100 breakpoints x 100 depths x 4 curves, which no diff needs. This
        // grid still crosses the breakpoint in both directions on every curve.
        printf("midinote,breakpoint,ldepth,rdepth,lcurve,rcurve,level_delta\n");
        for (int n = 0; n < 128; n += 3)
            for (int bp = 0; bp < 100; bp += 11)
                for (int d = 0; d < 100; d += 13)
                    for (int lc = 0; lc < 4; lc++)
                        for (int rc = 0; rc < 4; rc++)
                            printf("%d,%d,%d,%d,%d,%d,%d\n", n, bp, d, d, lc, rc,
                                   ScaleLevel(n, bp, d, d, lc, rc));

    } else if (what == "eg") {
        // One operator's amplitude EG, sampled at Dexed's own native control
        // rate (one getsample() per N=64 samples), emitted as dB below the
        // shared reference. fm_ctl_diff.py resamples both sides onto a common
        // grid, so the two engines' different block sizes cost nothing here.
        Env env;
        int ol = (Env::scaleoutlevel(outlevel) << 5);
        env.init(rates, levels, ol, ratescale);
        env.keydown(true);

        const int32_t ref = reference_level_q24();
        printf("time_s,db\n");
        bool released = false;
        uint32_t total = (uint32_t)(dur * SR);
        for (uint32_t s = 0; s < total; s += N) {
            double t = (double)s / SR;
            if (!released && t >= gate) { env.keydown(false); released = true; }
            int32_t lvl = env.getsample();
            double db = ((double)(lvl - ref) / (double)(1 << 24)) * DB_PER_OCTAVE;
            printf("%.6f,%.4f\n", t, db);
        }

    } else if (what == "pitcheg") {
        PitchEnv pe;
        pe.set(rates, levels);
        pe.keydown(true);
        printf("time_s,cents\n");
        bool released = false;
        uint32_t total = (uint32_t)(dur * SR);
        for (uint32_t s = 0; s < total; s += N) {
            double t = (double)s / SR;
            if (!released && t >= gate) { pe.keydown(false); released = true; }
            // Q24 per octave -> cents.
            double cents = (double)pe.getsample() / (double)(1 << 24) * 1200.0;
            printf("%.6f,%.4f\n", t, cents);
        }

    } else if (what == "lfo") {
        Lfo lfo;
        uint8_t p[6];
        for (int i = 0; i < 6; i++) p[i] = (uint8_t)lfo_p[i];
        lfo.reset(p);
        lfo.keydown();
        printf("time_s,value,delay\n");
        uint32_t total = (uint32_t)(dur * SR);
        for (uint32_t s = 0; s < total; s += N) {
            // Both are 0..1 in Q24 (lfo.h). getsample() must be called before
            // getdelay() to match the order Dx7Note::compute() uses.
            double v = (double)lfo.getsample() / (double)(1 << 24);
            double d = (double)lfo.getdelay() / (double)(1 << 24);
            printf("%.6f,%.6f,%.6f\n", (double)s / SR, v, d);
        }

    } else if (what == "algo") {
        // FmCore's own 32-algorithm table, decoded into the same per-operator
        // description t00t's fm_resolve_routing() produces. Operator index is
        // DX7 hardware processing order (0 = OP6 ... 5 = OP1), matching both
        // Dexed's table and syx2patch.py's unpack order.
        printf("algorithm,op,in_bus,out_bus,fb_in,fb_out,carrier\n");
        for (int alg = 0; alg < 32; alg++) {
            for (int op = 0; op < 6; op++) {
                int flags = AlgoPeek::flags(alg, op);
                printf("%d,%d,%d,%d,%d,%d,%d\n", alg, op,
                       (flags >> 4) & 3, flags & 3,
                       (flags & FB_IN) ? 1 : 0, (flags & FB_OUT) ? 1 : 0,
                       FmCore::isCarrier(alg, op) ? 1 : 0);
            }
        }

    } else {
        fprintf(stderr, "error: unknown --what '%s'\n", what.c_str());
        usage();
        return 2;
    }
    return 0;
}
