// dexed_render -- ground-truth DX7 reference renderer for the FM module (fm2.md §3.1, F0-a).
//
// Drives Dexed's synthesis core (Source/msfa/, fetched by fetch_dexed.sh) as a
// standalone command-line renderer: load a DX7 32-voice .syx bank, pick a
// voice, play one note for `--gate` seconds, release it, render `--tail` more,
// write a mono PCM16 WAV.
//
// tools/host_render/render_fm_patch.cpp takes the identical CLI and renders the
// same note through t00t's own engine; tools/fm_compare.py scores one against
// the other. Keeping the two CLIs argument-for-argument identical is what makes
// the compare script a thin diff rather than a translation layer.
//
// Nothing here is ever linked into the device firmware -- host-only tooling.
//
// Build: make (see Makefile). Run: ./dexed_render --help

#include "msfa/synth.h"
#include "msfa/freqlut.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/lfo.h"
#include "msfa/pitchenv.h"
#include "msfa/env.h"
#include "msfa/porta.h"
#include "msfa/fm_core.h"
#include "msfa/dx7note.h"
#include "msfa/controllers.h"

#include "wav32.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Dexed's core is int32 throughout, with the operator output landing on the
// Q24-ish scale `Exp2::lookup` produces (fm_core.cc: gain = Exp2::lookup(
// level_in - 14<<24), output = (sin * gain) >> 24). Dividing by 2^24 puts one
// unity-amplitude carrier at 1.0, which is the natural unit to report in and
// keeps the float WAV's numbers interpretable rather than arbitrary.
//
// Measured for context (this rig, 96 renders: ROM1A/2A/4B x 32 voices x notes
// 36/48/60 at velocity 127): the hottest factory patch peaks at raw 144,096,955
// = 8.59 in these units. Nothing is clamped -- float WAV has no headroom limit,
// and fm_compare.py normalises. This constant exists so the reported dB numbers
// mean something, not to prevent clipping.
static constexpr double DEXED_UNIT = 16777216.0;  // 2^24

static constexpr double SR = 44100.0;

struct Args {
    std::string syx;
    int         voice      = 0;      // 0-based within the 32-voice bank
    int         note       = 48;     // C3 (MIDI 48)
    int         velocity   = 100;
    double      gate       = 2.0;    // seconds held
    double      tail       = 1.5;    // seconds rendered after key-up
    std::string out        = "ref.wav";
    std::string pcm16;                   // optional listenable companion
    bool        list       = false;
};

static void usage() {
    printf(
        "dexed_render -- DX7 reference renderer (Dexed msfa core)\n"
        "\n"
        "  --syx PATH        32-voice DX7 sysex bank (4096 or 4104 bytes)\n"
        "  --voice N         voice index within the bank, 0-31 (default 0)\n"
        "  --note N          MIDI note number (default 48 = C3)\n"
        "  --vel N           MIDI velocity 1-127 (default 100)\n"
        "  --gate SEC        seconds to hold the note (default 2.0)\n"
        "  --tail SEC        seconds to render after key-up (default 1.5)\n"
        "  --out PATH        float32 WAV, for analysis (default ref.wav)\n"
        "  --pcm16 PATH      also write a peak-normalised PCM16 WAV, for ears\n"
        "  --list            print the bank's 32 voice names and exit\n");
}

// DX7 packed (128 bytes/voice, as stored in a bank) -> unpacked (156 bytes, the
// "voice edit buffer" layout Dx7Note::init consumes). Same bit layout as
// tools/syx2patch.py's unpack_voice(), which is itself a checked port of
// Dexed's Cartridge::unpackProgram -- the two are cross-checks on each other.
// Operators are stored OP6-first in both formats, so no reordering happens here.
static void unpack_voice(const uint8_t *packed, uint8_t out[156]) {
    for (int op = 0; op < 6; op++) {
        const uint8_t *p = packed + op * 17;
        uint8_t *o = out + op * 21;
        for (int i = 0; i < 8; i++) o[i] = p[i];        // R1-R4, L1-L4
        o[8]  = p[8];                                    // break point
        o[9]  = p[9];                                    // left depth
        o[10] = p[10];                                   // right depth
        o[11] = p[11] & 3;                               // left curve
        o[12] = (p[11] >> 2) & 3;                        // right curve
        o[13] = p[12] & 7;                               // rate scaling
        o[14] = p[13] & 3;                               // amp mod sensitivity
        o[15] = (p[13] >> 2) & 7;                        // key velocity sensitivity
        o[16] = p[14];                                   // output level
        o[17] = p[15] & 1;                               // osc mode (0 ratio, 1 fixed)
        o[18] = (p[15] >> 1) & 31;                       // freq coarse
        o[19] = p[16];                                   // freq fine
        o[20] = (p[12] >> 3) & 15;                       // detune
    }
    for (int i = 0; i < 8; i++) out[126 + i] = packed[102 + i];  // pitch EG R1-4, L1-4
    out[134] = packed[110] & 31;                         // algorithm
    out[135] = packed[111] & 7;                          // feedback
    out[136] = (packed[111] >> 3) & 1;                   // osc key sync
    out[137] = packed[112];                              // LFO speed
    out[138] = packed[113];                              // LFO delay
    out[139] = packed[114];                              // LFO pitch mod depth
    out[140] = packed[115];                              // LFO amp mod depth
    out[141] = packed[116] & 1;                          // LFO key sync
    out[142] = (packed[116] >> 1) & 7;                   // LFO waveform
    out[143] = (packed[116] >> 4) & 7;                   // LFO pitch mod sensitivity
    out[144] = packed[117];                              // transpose
    for (int i = 0; i < 10; i++) out[145 + i] = packed[118 + i];  // name
    out[155] = 0x3f;                                     // all six operators enabled
}

// Accepts either a bare 4096-byte bank or the 4104-byte sysex bulk dump
// (F0 43 09 20 00 <4096 bytes> <checksum> F7).
static bool load_bank(const std::string &path, std::vector<uint8_t> &bank) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path.c_str()); return false; }
    std::vector<uint8_t> raw;
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) raw.insert(raw.end(), chunk, chunk + n);
    fclose(f);

    if (raw.size() == 4096) {
        bank = raw;
    } else if (raw.size() == 4104 && raw[0] == 0xF0 && raw.back() == 0xF7) {
        bank.assign(raw.begin() + 6, raw.begin() + 6 + 4096);
    } else {
        fprintf(stderr, "error: %s is %zu bytes -- expected a 4096-byte bank or a "
                        "4104-byte sysex bulk dump\n", path.c_str(), raw.size());
        return false;
    }
    return true;
}

static std::string voice_name(const uint8_t *packed) {
    std::string s;
    for (int i = 0; i < 10; i++) s += (char)(packed[118 + i] & 0x7F);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int main(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--syx")       a.syx = next();
        else if (k == "--voice")     a.voice = atoi(next());
        else if (k == "--note")      a.note = atoi(next());
        else if (k == "--vel")       a.velocity = atoi(next());
        else if (k == "--gate")      a.gate = atof(next());
        else if (k == "--tail")      a.tail = atof(next());
        else if (k == "--out")       a.out = next();
        else if (k == "--pcm16")     a.pcm16 = next();
        else if (k == "--list")      a.list = true;
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }
    if (a.syx.empty()) { fprintf(stderr, "error: --syx is required\n"); usage(); return 2; }

    std::vector<uint8_t> bank;
    if (!load_bank(a.syx, bank)) return 1;

    if (a.list) {
        for (int v = 0; v < 32; v++) printf("%2d  %s\n", v, voice_name(&bank[v * 128]).c_str());
        return 0;
    }
    if (a.voice < 0 || a.voice > 31) { fprintf(stderr, "error: --voice must be 0-31\n"); return 2; }

    uint8_t patch[156];
    unpack_voice(&bank[a.voice * 128], patch);

    // Static table init, in the order Dexed's own plugin does it. All of these
    // are sample-rate dependent except Sin/Exp2.
    Freqlut::init(SR);
    Exp2::init();
    Sin::init();
    Lfo::init(SR);
    PitchEnv::init(SR);
    Env::init_sr(SR);
    Porta::init_sr(SR);

    FmCore core;
    Controllers ctrls;
    ctrls.core = &core;
    ctrls.values_[kControllerPitch]        = 0x2000;  // centre bend
    ctrls.values_[kControllerPitchRangeUp] = 2;
    ctrls.values_[kControllerPitchRangeDn] = 2;
    ctrls.values_[kControllerPitchStep]    = 0;
    ctrls.masterTune   = 0;
    ctrls.modwheel_cc  = 0;
    ctrls.breath_cc    = 0;
    ctrls.foot_cc      = 0;
    ctrls.aftertouch_cc = 0;
    ctrls.mpeEnabled   = false;
    ctrls.refresh();

    Lfo lfo;
    lfo.reset(patch + 137);

    Dx7Note note(createStandardTuning(), nullptr);
    note.init(patch, a.note, a.velocity, /*channel=*/1, &ctrls);
    lfo.keydown();

    const uint32_t gate_frames  = (uint32_t)(a.gate * SR);
    const uint32_t total_frames = (uint32_t)((a.gate + a.tail) * SR);

    std::vector<float> out;
    out.reserve(total_frames);

    int32_t buf[N];
    bool released = false;
    int32_t peak_raw = 0;

    // Dexed advances in fixed N=64-sample blocks (synth.h's LG_N); the note's
    // whole control plane -- every EG, the pitch EG, the LFO -- steps exactly
    // once per block. That block size is Dexed's, not t00t's (which uses
    // FM_BLOCK=16), and is one of the deliberate deviations listed in fm2.md §3.2.
    for (uint32_t done = 0; done < total_frames; done += N) {
        if (!released && done >= gate_frames) { note.keyup(); released = true; }

        int32_t lfo_val   = lfo.getsample();
        int32_t lfo_delay = lfo.getdelay();
        memset(buf, 0, sizeof buf);          // compute() accumulates
        note.compute(buf, lfo_val, lfo_delay, &ctrls);

        for (int i = 0; i < N && out.size() < total_frames; i++) {
            int32_t raw = buf[i];
            int32_t mag = raw < 0 ? -raw : raw;
            if (mag > peak_raw) peak_raw = mag;
            out.push_back((float)((double)raw / DEXED_UNIT));
        }
    }

    if (!write_wav_f32(a.out, out, (uint32_t)SR, 1)) {
        fprintf(stderr, "error: cannot write %s\n", a.out.c_str());
        return 1;
    }
    if (!a.pcm16.empty() && !write_wav_pcm16_normalized(a.pcm16, out, (uint32_t)SR, 1)) {
        fprintf(stderr, "error: cannot write %s\n", a.pcm16.c_str());
        return 1;
    }

    double peak_units = (double)peak_raw / DEXED_UNIT;
    printf("dexed_render: %s voice %d \"%s\" note %d vel %d gate %.2fs tail %.2fs\n",
           a.syx.c_str(), a.voice, voice_name(&bank[a.voice * 128]).c_str(),
           a.note, a.velocity, a.gate, a.tail);
    printf("  wrote %s -- %zu frames, peak %.4f (%.2f dB re 1.0, raw %d)\n",
           a.out.c_str(), out.size(), peak_units,
           peak_units > 0 ? 20.0 * log10(peak_units) : -999.0, peak_raw);
    if (!a.pcm16.empty()) printf("  wrote %s -- peak-normalised PCM16\n", a.pcm16.c_str());
    return 0;
}
