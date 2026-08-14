// resid_render -- the reference side of the F0 comparison (chip.md §11.1).
//
// Consumes a register stream (see sidreg.h) and renders it through reSID at
// 44.1 kHz. tools/host_render/render_sid.cpp takes the identical CLI and
// renders the same stream through t00t's own primitives; tools/sid_compare.py
// scores the difference.
//
//   ./resid_render --stream streams/saw_sweep.sidreg --out out/ref.wav
//
// Host-only tooling. reSID is GPL-2 and is fetched, not vendored -- see
// fetch_resid.sh. Nothing here is ever linked into the device firmware.

#include "sidreg.h"
#include "../host_render/wav32.h"

#include "sid.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void usage() {
    fprintf(stderr,
        "usage: resid_render --stream <file.sidreg> --out <file.wav> [options]\n"
        "  --stream <path>   register stream to render (required)\n"
        "  --out <path>      float32 analysis WAV (required)\n"
        "  --pcm16 <path>    additionally write a peak-normalised listening WAV\n"
        "  --rate <hz>       output sample rate (default 44100)\n"
        "  --fast            SAMPLE_FAST instead of SAMPLE_RESAMPLE (much faster,\n"
        "                    aliases; for smoke tests only, never for a scorecard)\n");
}

int main(int argc, char **argv) {
    std::string stream_path, out_path, pcm16_path;
    double sample_rate = 44100.0;
    bool fast = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "resid_render: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--stream") stream_path = next("--stream");
        else if (a == "--out")    out_path    = next("--out");
        else if (a == "--pcm16")  pcm16_path  = next("--pcm16");
        else if (a == "--rate")   sample_rate = atof(next("--rate").c_str());
        else if (a == "--fast")   fast = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "resid_render: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (stream_path.empty() || out_path.empty()) { usage(); return 2; }

    SidRegStream st;
    std::string err;
    if (!sidreg_parse(stream_path, st, err)) {
        fprintf(stderr, "resid_render: %s\n", err.c_str());
        return 1;
    }

    reSID::SID sid;
    sid.set_chip_model(st.model_8580 ? reSID::MOS8580 : reSID::MOS6581);
    sid.enable_filter(true);
    // The external filter is the C64 *board's* passive output network, not the
    // chip's -- chip.md §10 calls it out separately as a near-zero-cost one-pole
    // that is "genuinely part of the SID sound". It is deliberately left ON
    // here so the reference includes it: the t00t side models it too, and
    // taking it out of the reference would make the comparison measure a stage
    // neither engine is supposed to omit.
    sid.enable_external_filter(true);
    if (!sid.set_sampling_parameters(st.clock_hz,
                                     fast ? reSID::SAMPLE_FAST : reSID::SAMPLE_RESAMPLE,
                                     sample_rate)) {
        fprintf(stderr, "resid_render: reSID rejected the sampling parameters\n");
        return 1;
    }
    sid.reset();

    std::vector<float> samples;
    samples.reserve((size_t)(st.frames * sample_rate / st.frame_rate()) + 1024);

    short buf[4096];
    size_t wi = 0;
    for (uint32_t frame = 0; frame < st.frames; frame++) {
        while (wi < st.writes.size() && st.writes[wi].frame == frame) {
            sid.write(st.writes[wi].reg, st.writes[wi].value);
            wi++;
        }
        reSID::cycle_count delta_t = (reSID::cycle_count)st.frame_cycles;
        while (delta_t > 0) {
            int n = sid.clock(delta_t, buf, (int)(sizeof(buf) / sizeof(buf[0])));
            for (int i = 0; i < n; i++) samples.push_back((float)buf[i] / 32768.0f);
            if (n == 0 && delta_t > 0) {
                // clock() consumed the remaining cycles without producing a
                // sample -- normal at the tail of a frame.
                break;
            }
        }
    }

    if (!write_wav_f32(out_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "resid_render: cannot write %s\n", out_path.c_str());
        return 1;
    }
    if (!pcm16_path.empty() && !write_wav_pcm16_normalized(pcm16_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "resid_render: cannot write %s\n", pcm16_path.c_str());
        return 1;
    }

    fprintf(stderr, "resid_render: %s -> %s  (%zu samples, %.3f s, %s, %.4f Hz frames)\n",
            stream_path.c_str(), out_path.c_str(), samples.size(),
            samples.size() / sample_rate, st.model_8580 ? "8580" : "6581", st.frame_rate());
    return 0;
}
