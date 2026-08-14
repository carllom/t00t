// ayumi_render -- the reference side of the AY-P0 comparison.
//
// Consumes the same register stream format as
// tools/host_render/render_ay.cpp (../ay_ref/ayreg.h) and renders it
// through ayumi's own public API -- its real oversampled/decimated/DC-
// filtered pipeline (ayumi_process/ayumi_remove_dc), not the raw per-tick
// state t00t_ay_dump/ayumi_dump compare. tools/sid_compare.py (reused
// unmodified) scores the difference against render_ay's output.
//
// Host-only tooling.

#include "ayreg.h"
#include "../host_render/wav32.h"
#include "ayumi/ayumi.h"

#include <cstdio>
#include <string>
#include <vector>

static void usage() {
    fprintf(stderr,
        "usage: ayumi_render --stream <file.ayreg> --out <file.wav> [options]\n"
        "  --stream <path>   register stream to render (required)\n"
        "  --out <path>      float32 analysis WAV (required)\n"
        "  --pcm16 <path>    additionally write a peak-normalised listening WAV\n"
        "  --rate <hz>       output sample rate (default 44100)\n");
}

// R0-R13 -> ayumi's public setter API. Same register layout render_ay.cpp's
// StrictAy::write_reg applies to t00t's own primitives. `period[0..2]` are
// the 3 tone channels' registers, `period[3]` doubles as the envelope's --
// all four are plain 12/16-bit register shadows the caller must keep across
// calls (ayumi.h exposes no getter for them).
static void write_reg(struct ayumi *ay, uint16_t period[4], uint8_t reg, uint8_t val) {
    switch (reg) {
        case 0x00: period[0] = (uint16_t)((period[0] & 0xf00) | val); ayumi_set_tone(ay, 0, period[0]); break;
        case 0x01: period[0] = (uint16_t)((period[0] & 0x0ff) | ((val & 0x0f) << 8)); ayumi_set_tone(ay, 0, period[0]); break;
        case 0x02: period[1] = (uint16_t)((period[1] & 0xf00) | val); ayumi_set_tone(ay, 1, period[1]); break;
        case 0x03: period[1] = (uint16_t)((period[1] & 0x0ff) | ((val & 0x0f) << 8)); ayumi_set_tone(ay, 1, period[1]); break;
        case 0x04: period[2] = (uint16_t)((period[2] & 0xf00) | val); ayumi_set_tone(ay, 2, period[2]); break;
        case 0x05: period[2] = (uint16_t)((period[2] & 0x0ff) | ((val & 0x0f) << 8)); ayumi_set_tone(ay, 2, period[2]); break;
        case 0x06: ayumi_set_noise(ay, val); break;
        case 0x07:
            for (int i = 0; i < 3; i++)
                ayumi_set_mixer(ay, i, (val >> i) & 1, (val >> (i + 3)) & 1, ay->channels[i].e_on);
            break;
        case 0x08: ayumi_set_volume(ay, 0, val & 0x0f); ay->channels[0].e_on = (val >> 4) & 1; break;
        case 0x09: ayumi_set_volume(ay, 1, val & 0x0f); ay->channels[1].e_on = (val >> 4) & 1; break;
        case 0x0a: ayumi_set_volume(ay, 2, val & 0x0f); ay->channels[2].e_on = (val >> 4) & 1; break;
        case 0x0b: period[3] = (uint16_t)((period[3] & 0xff00) | val); ayumi_set_envelope(ay, period[3]); break;
        case 0x0c: period[3] = (uint16_t)((period[3] & 0x00ff) | ((uint16_t)val << 8)); ayumi_set_envelope(ay, period[3]); break;
        case 0x0d: ayumi_set_envelope_shape(ay, val); break;
        default: break;
    }
}

int main(int argc, char **argv) {
    std::string stream_path, out_path, pcm16_path;
    double sample_rate = 44100.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "ayumi_render: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--stream") stream_path = next("--stream");
        else if (a == "--out")    out_path    = next("--out");
        else if (a == "--pcm16")  pcm16_path  = next("--pcm16");
        else if (a == "--rate")   sample_rate = atof(next("--rate").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "ayumi_render: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (stream_path.empty() || out_path.empty()) { usage(); return 2; }

    AyRegStream st;
    std::string err;
    if (!ayreg_parse(stream_path, st, err)) {
        fprintf(stderr, "ayumi_render: %s\n", err.c_str());
        return 1;
    }

    struct ayumi ay;
    ayumi_configure(&ay, st.is_ym ? 1 : 0, st.clock_hz, (int)sample_rate);
    for (int i = 0; i < 3; i++) ayumi_set_pan(&ay, i, 0.5, 0);
    uint16_t period[4] = {1, 1, 1, 1};

    std::vector<float> samples;
    double frame_rate = st.frame_rate();
    samples.reserve((size_t)(st.frames * sample_rate / frame_rate) + 1024);

    double samples_per_frame = sample_rate / frame_rate;
    double sample_pos = 0.0;
    size_t wi = 0;

    for (uint32_t frame = 0; frame < st.frames; frame++) {
        while (wi < st.writes.size() && st.writes[wi].frame == frame) {
            write_reg(&ay, period, st.writes[wi].reg, st.writes[wi].value);
            wi++;
        }
        double next_pos = sample_pos + samples_per_frame;
        uint32_t n = (uint32_t)next_pos - (uint32_t)sample_pos;
        for (uint32_t i = 0; i < n; i++) {
            ayumi_process(&ay);
            ayumi_remove_dc(&ay);
            samples.push_back((float)ay.left);
        }
        sample_pos = next_pos;
    }

    if (!write_wav_f32(out_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "ayumi_render: cannot write %s\n", out_path.c_str());
        return 1;
    }
    if (!pcm16_path.empty() && !write_wav_pcm16_normalized(pcm16_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "ayumi_render: cannot write %s\n", pcm16_path.c_str());
        return 1;
    }

    fprintf(stderr, "ayumi_render: %s -> %s  (%zu samples, %.3f s, %s, %.4f Hz frames)\n",
            stream_path.c_str(), out_path.c_str(), samples.size(),
            samples.size() / sample_rate, st.is_ym ? "ym2149" : "ay8910", frame_rate);
    return 0;
}
