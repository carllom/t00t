// render_ay -- the t00t side of the AY-P0 comparison (module_chip.md, AY_STRICT).
//
// Consumes a register stream (see ../ay_ref/ayreg.h) and renders it through
// src/chip/ay_osc.h / ay_envelope.h at 44.1 kHz, in the strict hardware
// topology: exactly 3 tone channels sharing one noise generator and one
// envelope generator (see ay_osc.h's header comment for why this is a
// *validation-only* topology, not necessarily what a future engine gives
// each dynamically-allocated voice).
//
//   ./render_ay --stream streams/tone_a4.ayreg --out out/test.wav
//
// tools/ay_ref/ayumi_render.cpp takes the identical CLI and renders the
// same stream through ayumi; tools/sid_compare.py (reused unmodified --
// it's generic over any two float32 WAVs) scores the difference.
//
// Host-only tooling, never linked into the device firmware.

#include "../ay_ref/ayreg.h"
#include "wav32.h"

#include "chip/ay_osc.h"
#include "chip/ay_envelope.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The strict 3-channel / 1-noise / 1-envelope topology.
struct StrictAy {
    AyTone     tone[3];
    AyNoise    noise;
    AyEnvelope envelope;
    AyModel    model;

    uint8_t t_off[3] = {1, 1, 1};   // registers default to both generators
    uint8_t n_off[3] = {1, 1, 1};   // disabled -- silence until R7 says otherwise
    uint8_t e_on[3]  = {0, 0, 0};
    uint8_t volume[3] = {0, 0, 0};

    void init(AyModel m) {
        model = m;
        for (int i = 0; i < 3; i++) {
            tone[i].init();
            t_off[i] = 1; n_off[i] = 1; e_on[i] = 0; volume[i] = 0;
        }
        noise.init();
        envelope.init();
    }

    // Apply one R0-R13 register write. Layout is the AY-3-8910's own
    // (matching ayumi's t_off/n_off/e_on naming 1:1, cross-checked against
    // its setter API): R0-R5 tone period fine/coarse x3, R6 noise period,
    // R7 mixer (bit i = A/B/C tone disable, bit i+3 = A/B/C noise disable),
    // R8-R10 volume (bits 0-3) + envelope-enable (bit 4), R11/R12 envelope
    // period fine/coarse, R13 envelope shape (any write restarts it).
    void write_reg(uint8_t reg, uint8_t val) {
        switch (reg) {
            case 0x00: tone[0].set_period((uint16_t)((tone[0].period & 0xf00) | val)); break;
            case 0x01: tone[0].set_period((uint16_t)((tone[0].period & 0x0ff) | ((val & 0x0f) << 8))); break;
            case 0x02: tone[1].set_period((uint16_t)((tone[1].period & 0xf00) | val)); break;
            case 0x03: tone[1].set_period((uint16_t)((tone[1].period & 0x0ff) | ((val & 0x0f) << 8))); break;
            case 0x04: tone[2].set_period((uint16_t)((tone[2].period & 0xf00) | val)); break;
            case 0x05: tone[2].set_period((uint16_t)((tone[2].period & 0x0ff) | ((val & 0x0f) << 8))); break;
            case 0x06: noise.set_period(val); break;
            case 0x07:
                for (int i = 0; i < 3; i++) {
                    t_off[i] = (uint8_t)((val >> i) & 1);
                    n_off[i] = (uint8_t)((val >> (i + 3)) & 1);
                }
                break;
            case 0x08: volume[0] = val & 0x0f; e_on[0] = (val >> 4) & 1; break;
            case 0x09: volume[1] = val & 0x0f; e_on[1] = (val >> 4) & 1; break;
            case 0x0a: volume[2] = val & 0x0f; e_on[2] = (val >> 4) & 1; break;
            case 0x0b: envelope.set_period((uint16_t)((envelope.period & 0xff00) | val)); break;
            case 0x0c: envelope.set_period((uint16_t)((envelope.period & 0x00ff) | ((uint16_t)val << 8))); break;
            case 0x0d: envelope.set_shape(val); break;
            default: break;
        }
    }

    // Advance `ticks` shared internal clock/8 ticks, then mix one sample.
    float tick(uint32_t ticks) {
        for (int i = 0; i < 3; i++) tone[i].advance(ticks);
        noise.advance(ticks);
        envelope.advance(ticks);

        float sum = 0.0f;
        uint8_t n = noise.out();
        for (int i = 0; i < 3; i++) {
            sum += ay_mix(model, tone[i].out, t_off[i], n, n_off[i], e_on[i] != 0, volume[i], envelope.level);
        }
        // 0.5x, matching ayumi_render's ayumi_set_pan(0.5, eqp=false) mono
        // convention (pan_left = pan_right = 0.5, summed across 3 channels)
        // rather than an arbitrary different scale -- keeps the two
        // renderers' output levels directly comparable for
        // tools/sid_compare.py rather than differing by a constant offset
        // for no reason.
        return sum * 0.5f;
    }
};

static void usage() {
    fprintf(stderr,
        "usage: render_ay --stream <file.ayreg> --out <file.wav> [options]\n"
        "  --stream <path>   register stream to render (required)\n"
        "  --out <path>      float32 analysis WAV (required)\n"
        "  --pcm16 <path>    additionally write a peak-normalised listening WAV\n"
        "  --rate <hz>       output sample rate (default 44100)\n");
}

int main(int argc, char **argv) {
    std::string stream_path, out_path, pcm16_path;
    double sample_rate = 44100.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "render_ay: %s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--stream") stream_path = next("--stream");
        else if (a == "--out")    out_path    = next("--out");
        else if (a == "--pcm16")  pcm16_path  = next("--pcm16");
        else if (a == "--rate")   sample_rate = atof(next("--rate").c_str());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { fprintf(stderr, "render_ay: unknown argument '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (stream_path.empty() || out_path.empty()) { usage(); return 2; }

    AyRegStream st;
    std::string err;
    if (!ayreg_parse(stream_path, st, err)) {
        fprintf(stderr, "render_ay: %s\n", err.c_str());
        return 1;
    }

    StrictAy ay;
    ay.init(st.is_ym ? AY_MODEL_YM2149 : AY_MODEL_AY8910);
    uint32_t tick_scale_q16 = ay_tick_scale_q16(st.clock_hz, sample_rate);
    uint32_t tick_phase = 0;

    std::vector<float> samples;
    double frame_rate = st.frame_rate();
    samples.reserve((size_t)(st.frames * sample_rate / frame_rate) + 1024);

    double samples_per_frame = sample_rate / frame_rate;
    double sample_pos = 0.0;
    size_t wi = 0;

    for (uint32_t frame = 0; frame < st.frames; frame++) {
        while (wi < st.writes.size() && st.writes[wi].frame == frame) {
            ay.write_reg(st.writes[wi].reg, st.writes[wi].value);
            wi++;
        }
        double next_pos = sample_pos + samples_per_frame;
        uint32_t n = (uint32_t)next_pos - (uint32_t)sample_pos;
        for (uint32_t i = 0; i < n; i++) {
            tick_phase += tick_scale_q16;
            uint32_t ticks = tick_phase >> 16;
            tick_phase &= 0xffffu;
            samples.push_back(ay.tick(ticks));
        }
        sample_pos = next_pos;
    }

    if (!write_wav_f32(out_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "render_ay: cannot write %s\n", out_path.c_str());
        return 1;
    }
    if (!pcm16_path.empty() && !write_wav_pcm16_normalized(pcm16_path, samples, (uint32_t)sample_rate, 1)) {
        fprintf(stderr, "render_ay: cannot write %s\n", pcm16_path.c_str());
        return 1;
    }

    fprintf(stderr, "render_ay: %s -> %s  (%zu samples, %.3f s, %s, %.4f Hz frames)\n",
            stream_path.c_str(), out_path.c_str(), samples.size(),
            samples.size() / sample_rate, st.is_ym ? "ym2149" : "ay8910", frame_rate);
    return 0;
}
