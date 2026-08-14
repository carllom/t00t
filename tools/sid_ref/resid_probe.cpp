// resid_probe -- measures reSID's filter response so the t00t 6581 model can
// be fitted to it (module_chip.md §5.1).
//
// Why measure rather than read a table: reSID 1.0's 6581 filter is a
// transistor-level model -- op-amp transfer curves, a VCR gate-voltage
// solver, and 2^16-entry summer/mixer/gain tables (filter.cc). There is no
// "cutoff frequency in Hz" anywhere in it to copy. Its only 6581 constant
// resembling one is an 11-bit R-2R DAC feeding a nonlinear VCR, and w0 in
// filter.h is explicitly marked "temporarily used for MOS 8580 emulation".
//
// So the curve is obtained the same way it was obtained from real chips: put
// a signal in, look at what comes out. The output of this tool is raw audio;
// tools/fit_6581_filter.py turns it into src/chip/sid_tables.h and is the
// authority on the fitting. That two-step split is deliberate -- it keeps the
// numerics in one place with the rest of the analysis, and it makes the LUT
// regenerable rather than a magic array.
//
// module_chip.md §5.1 notes the 6581 cutoff curve varies between physical
// chips. What this tool pins down is specifically reSID 1.0-pre1's 6581,
// which is itself a model of one measured chip. The generated header must
// say so.
//
// The probe signal is voice 1's own noise waveform at maximum frequency, held
// at full sustain. Run 0 routes it around the filter and every other run
// routes it through, so dividing the two spectra leaves the filter and cancels
// the source, the mixer, the output stage and reSID's resampling response. The
// noise sequence is deterministic and identical in every run, which is what
// makes a plain FFT ratio a valid transfer function -- no cross-spectral
// estimation, no coherence to worry about.
//
// EXT IN ($D417 bit 3) was the obvious probe and is the wrong one: reSID's
// Filter::input() adds the *mixer's* op-amp DC level rather than the voice's,
// which parks the 6581's nonlinear VCR solver at an operating point where the
// cutoff stops responding. A voice sits at the DC level the model was fitted
// for and behaves.
//
// Host-only tooling; reSID is fetched, not vendored (see fetch_resid.sh).

#include "sid.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void usage() {
    fprintf(stderr,
        "usage: resid_probe --out <file.f32> --meta <file.json> [options]\n"
        "  --mode fc|res     sweep filter cutoff (default) or resonance\n"
        "  --fc-step <n>     cutoff sweep step in register units (default 16)\n"
        "  --fc <n>          fixed cutoff for --mode res (default 1024)\n"
        "  --res <n>         fixed resonance for --mode fc (default 0)\n"
        "  --fmode lp|bp|hp  filter mode bit to enable (default lp)\n"
        "  --model 6581|8580 chip model (default 6581)\n"
        "  --seconds <s>     probe length per grid point (default 1.0)\n"
        "  --rate <hz>       sample rate (default 44100)\n"
        "\n"
        "Writes float32 output[N] per run to --out, run 0 being the\n"
        "filter-bypassed reference. --meta describes the grid.\n");
}

int main(int argc, char **argv) {
    std::string out_path, meta_path, mode = "fc", fmode = "lp";
    int fc_step = 16, fc_fixed = 1024, res_fixed = 0;
    bool model_8580 = false;
    double seconds = 1.0, rate = 44100.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "resid_probe: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--out")     out_path  = next("--out");
        else if (a == "--meta")    meta_path = next("--meta");
        else if (a == "--mode")    mode      = next("--mode");
        else if (a == "--fmode")   fmode     = next("--fmode");
        else if (a == "--fc-step") fc_step   = atoi(next("--fc-step").c_str());
        else if (a == "--fc")      fc_fixed  = atoi(next("--fc").c_str());
        else if (a == "--res")     res_fixed = atoi(next("--res").c_str());
        else if (a == "--seconds") seconds   = atof(next("--seconds").c_str());
        else if (a == "--rate")    rate      = atof(next("--rate").c_str());
        else if (a == "--model")   model_8580 = (next("--model") == "8580");
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "resid_probe: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (out_path.empty() || meta_path.empty()) { usage(); return 2; }
    if (fc_step < 1) fc_step = 1;

    const int mode_bit = (fmode == "bp") ? 0x20 : (fmode == "hp") ? 0x40 : 0x10;
    const int N = (int)(seconds * rate);
    const double clock_hz = 985248.0;  // PAL

    // Grid. Run 0 is always the filter-bypassed reference.
    struct Run { int fc; int res; bool bypass; };
    std::vector<Run> runs;
    runs.push_back({0, 0, true});
    if (mode == "fc") {
        for (int fc = 0; fc < 2048; fc += fc_step) runs.push_back({fc, res_fixed, false});
        if (runs.back().fc != 2047) runs.push_back({2047, res_fixed, false});
    } else if (mode == "res") {
        for (int res = 0; res < 16; res++) runs.push_back({fc_fixed, res, false});
    } else {
        fprintf(stderr, "resid_probe: --mode must be fc or res\n");
        return 2;
    }

    FILE *fo = fopen(out_path.c_str(), "wb");
    if (!fo) { fprintf(stderr, "resid_probe: cannot write %s\n", out_path.c_str()); return 1; }

    std::vector<float> out_buf(N);
    std::vector<short> pcm(N);

    for (size_t r = 0; r < runs.size(); r++) {
        const Run &run = runs[r];

        reSID::SID sid;
        sid.set_chip_model(model_8580 ? reSID::MOS8580 : reSID::MOS6581);
        sid.enable_filter(true);
        // The board's passive output network is a separate stage in t00t
        // (module_chip.md §10) and is fitted separately; leaving it in here would put
        // its rolloff inside the number this tool exists to produce.
        sid.enable_external_filter(false);
        if (!sid.set_sampling_parameters(clock_hz, reSID::SAMPLE_RESAMPLE, rate)) {
            fprintf(stderr, "resid_probe: reSID rejected the sampling parameters\n");
            fclose(fo);
            return 1;
        }
        sid.reset();

        // Voice 1: noise at maximum frequency, full sustain, gate on. Attack
        // and decay 0 so it is at the sustain level within 2 ms; sustain 15 so
        // the envelope contributes no shape at all to the measurement.
        sid.write(0x00, 0xff);  // FREQ_LO
        sid.write(0x01, 0xff);  // FREQ_HI
        sid.write(0x05, 0x00);  // ATTACK/DECAY = 0/0
        sid.write(0x06, 0xf0);  // SUSTAIN/RELEASE = 15/0
        sid.write(0x04, 0x81);  // noise + gate

        sid.write(0x15, (reSID::reg8)(run.fc & 0x07));         // FC_LO (3 bits)
        sid.write(0x16, (reSID::reg8)((run.fc >> 3) & 0xff));  // FC_HI (8 bits)
        // FILT1 is bit 0. Bypassed runs route voice 1 straight to the mixer.
        sid.write(0x17, (reSID::reg8)((run.res << 4) | (run.bypass ? 0x00 : 0x01)));
        // MODE_VOL: the filter mode under test plus max volume. On a bypassed
        // run the mode bits gate only filter outputs, so they are harmless.
        sid.write(0x18, (reSID::reg8)(mode_bit | 0x0f));

        // Let the envelope reach sustain and the filter settle before
        // measuring; 20 ms is many time constants even at the lowest cutoff.
        // The noise sequence is deterministic, so every run is skipping the
        // same samples and the recorded windows stay sample-aligned -- which
        // is what makes the plain FFT ratio valid.
        const int settle = (int)(0.02 * rate);
        short sbuf[256];
        int produced = 0;
        while (produced < settle + N) {
            reSID::cycle_count delta_t = (reSID::cycle_count)(clock_hz / rate * 64.0);
            while (delta_t > 0) {
                int k = sid.clock(delta_t, sbuf, (int)(sizeof(sbuf) / sizeof(sbuf[0])));
                for (int i = 0; i < k && produced < settle + N; i++, produced++) {
                    if (produced >= settle) pcm[produced - settle] = sbuf[i];
                }
                if (k == 0) break;
            }
        }
        for (int n = 0; n < N; n++) out_buf[n] = (float)pcm[n] / 32768.0f;

        fwrite(out_buf.data(), sizeof(float), N, fo);
        fprintf(stderr, "\rresid_probe: run %zu/%zu (fc=%d res=%d%s)   ",
                r + 1, runs.size(), run.fc, run.res, run.bypass ? " bypass" : "");
    }
    fclose(fo);
    fprintf(stderr, "\n");

    FILE *fm = fopen(meta_path.c_str(), "w");
    if (!fm) { fprintf(stderr, "resid_probe: cannot write %s\n", meta_path.c_str()); return 1; }
    fprintf(fm, "{\n");
    fprintf(fm, "  \"tool\": \"resid_probe\",\n");
    fprintf(fm, "  \"model\": \"%s\",\n", model_8580 ? "8580" : "6581");
    fprintf(fm, "  \"sweep\": \"%s\",\n", mode.c_str());
    fprintf(fm, "  \"filter_mode\": \"%s\",\n", fmode.c_str());
    fprintf(fm, "  \"sample_rate\": %.1f,\n", rate);
    fprintf(fm, "  \"clock_hz\": %.1f,\n", clock_hz);
    fprintf(fm, "  \"samples_per_run\": %d,\n", N);
    fprintf(fm, "  \"runs\": [\n");
    for (size_t r = 0; r < runs.size(); r++) {
        fprintf(fm, "    {\"fc\": %d, \"res\": %d, \"bypass\": %s}%s\n",
                runs[r].fc, runs[r].res, runs[r].bypass ? "true" : "false",
                r + 1 < runs.size() ? "," : "");
    }
    fprintf(fm, "  ]\n}\n");
    fclose(fm);

    fprintf(stderr, "resid_probe: %zu runs x %d samples -> %s (meta: %s)\n",
            runs.size(), N, out_path.c_str(), meta_path.c_str());
    return 0;
}
