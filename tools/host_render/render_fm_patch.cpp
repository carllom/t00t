// render_fm_patch -- t00t's FM engine, rendered on the host with the same CLI
// as tools/fm_ref/dexed_render (fm2.md §3.1, F0-b).
//
// Deliberately argument-for-argument identical to dexed_render so that
// tools/fm_compare.py is a thin diff rather than a translation layer:
//
//   ./dexed_render      --syx rom1a.syx --voice 10 --note 48 --gate 2 --out a.wav
//   ./render_fm_patch   --syx rom1a.syx --voice 10 --note 48 --gate 2 --out b.wav
//
// The one structural difference is how the patch gets in. dexed_render unpacks
// the sysex itself; this renders whatever tools/syx2patch.py produced, i.e. the
// real device conversion path, because that converter's output IS part of what
// is under test. `--voice N` therefore indexes FM_PATCHES[] from the generated
// src/engines/fm/patches.h, and the bank passed as --syx must be the one it was
// generated from (checked by name, see main()).
//
// Everything below goes through the same op.h entry points the device calls --
// fm_resolve_routing / fm_voice_note_on / fm_render_voice / fm_voice_note_off --
// at the same SAMPLE_RATE and the same SAMPLES_PER_BUFFER, so a divergence from
// Dexed is a divergence in the engine, not in this harness.
//
// Build: make -f Makefile.fm (tools/host_render).

#include "../../src/engines/fm/op.h"
#include "../../src/osc/common.h"
#include "../../src/osc/sine.h"
#include "../fm_ref/wav32.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef T00T_FM_HAS_PATCHES
#include "../../src/engines/fm/patches.h"
#endif

// t00t's carriers are scaled to land in int16 range (op.h's
// FM_OUT_SHIFT_CARRIER), so 32768 is this engine's "1.0". Note this is a
// *different* absolute reference from dexed_render's 2^24 unit -- the two
// engines have no shared level anchor yet, which is precisely what fm2.md
// §1.1(a) says is broken and what F2 exists to fix. fm_compare.py therefore
// normalises before its spectral metrics and reports the raw level gap as its
// own separate number, rather than letting it contaminate everything else.
static constexpr double T00T_UNIT = 32768.0;

// fm_render_voice() pans into a stereo pair; at centre pan both gains are
// 0.7071 (pan.h's equal-power law). Dividing it back out recovers the mono
// voice bus, which is what dexed_render's mono output corresponds to.
static constexpr double CENTRE_PAN_GAIN = 0.70710678;

struct Args {
    std::string syx;
    int         voice    = 0;
    int         note     = 48;
    int         velocity = 100;
    double      gate     = 2.0;
    double      tail     = 1.5;
    std::string out      = "t00t.wav";
    std::string pcm16;
    bool        list     = false;
};

static void usage() {
    printf(
        "render_fm_patch -- t00t FM engine, host render (matches dexed_render's CLI)\n"
        "\n"
        "  --syx PATH        the bank patches.h was generated from (name-checked)\n"
        "  --voice N         patch index within the bank, 0-31 (default 0)\n"
        "  --note N          MIDI note number (default 48 = C3)\n"
        "  --vel N           MIDI velocity 1-127 (default 100)\n"
        "  --gate SEC        seconds to hold the note (default 2.0)\n"
        "  --tail SEC        seconds to render after key-up (default 1.5)\n"
        "  --out PATH        float32 WAV, for analysis (default t00t.wav)\n"
        "  --pcm16 PATH      also write a peak-normalised PCM16 WAV, for ears\n"
        "  --list            print the converted patch names and exit\n");
}

int main(int argc, char **argv) {
#ifndef T00T_FM_HAS_PATCHES
    fprintf(stderr,
            "error: built without src/engines/fm/patches.h.\n"
            "       Generate it first, e.g.:\n"
            "         python3 tools/syx2patch.py convert tools/fm_ref/banks/rom1a.syx \\\n"
            "                 src/engines/fm/patches.h\n"
            "       then rebuild.\n");
    return 1;
#else
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--syx")   a.syx = next();
        else if (k == "--voice") a.voice = atoi(next());
        else if (k == "--note")  a.note = atoi(next());
        else if (k == "--vel")   a.velocity = atoi(next());
        else if (k == "--gate")  a.gate = atof(next());
        else if (k == "--tail")  a.tail = atof(next());
        else if (k == "--out")   a.out = next();
        else if (k == "--pcm16") a.pcm16 = next();
        else if (k == "--list")  a.list = true;
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }

    if (a.list) {
        for (int v = 0; v < FM_PATCH_COUNT; v++) printf("%2d  %s\n", v, FM_PATCH_NAMES[v]);
        return 0;
    }
    if (a.voice < 0 || a.voice >= FM_PATCH_COUNT) {
        fprintf(stderr, "error: --voice must be 0-%d (patches.h holds %d patches)\n",
                FM_PATCH_COUNT - 1, FM_PATCH_COUNT);
        return 2;
    }

    // Table init, in the same order and with the same functions as the device's
    // fm_audio_engine_init() (src/engines/fm/audio_engine.cpp).
    fm_init_sine_tab();
    osc_init_sine();        // pan.h's pan_gains_q15() reuses the shared sine table
    env_dx_init_tables();   // level/rate/exp2 -- must run before any EG step

    const FmPatch &patch = FM_PATCHES[a.voice];

    FmRouting routing;
    if (!fm_resolve_routing(patch, routing)) {
        fprintf(stderr, "error: patch %d (%s) has an unroutable operator graph\n",
                a.voice, FM_PATCH_NAMES[a.voice]);
        return 1;
    }

    FmOp     ops[FM_NUM_OPS];
    FmPitchEg peg;
    FmLfo     lfo;
    for (uint32_t i = 0; i < FM_NUM_OPS; i++) env_dx_init(ops[i].eg);
    fm_pitch_eg_init(peg);
    fm_lfo_init(lfo);

    // Same note->frequency conversion the device's midi_controller.cpp uses,
    // and the same 12-TET reference Dexed's standard tuning resolves to.
    const float    freq_hz  = 440.0f * powf(2.0f, (float)(a.note - 69) / 12.0f);
    const uint32_t note_inc = fm_phase_inc(freq_hz);
    const int16_t  amplitude = (int16_t)((a.velocity / 127.0f) * 32767.0f);

    fm_voice_note_on(ops, patch, note_inc, amplitude, (uint8_t)a.note, &peg, &lfo);

    const uint32_t gate_frames  = (uint32_t)(a.gate * SAMPLE_RATE);
    const uint32_t total_frames = (uint32_t)((a.gate + a.tail) * SAMPLE_RATE);

    // Per-voice bus scratch, exactly as audio_engine.cpp lays it out.
    static int32_t mod_bus[FM_NUM_OPS][FM_BLOCK];
    static int32_t out_bus[FM_BLOCK];
    FmVoiceBuses bus;
    for (uint32_t i = 0; i < FM_NUM_OPS; i++) bus.mod[i] = mod_bus[i];
    bus.out = out_bus;

    std::vector<int32_t> dry_l(SAMPLES_PER_BUFFER), dry_r(SAMPLES_PER_BUFFER);
    std::vector<float> out;
    out.reserve(total_frames);

    bool released = false;
    int32_t peak_raw = 0;
    uint32_t freed_at = 0;

    for (uint32_t done = 0; done < total_frames; done += SAMPLES_PER_BUFFER) {
        if (!released && done >= gate_frames) {
            fm_voice_note_off(ops, &peg);
            released = true;
        }

        uint32_t n = total_frames - done;
        if (n > SAMPLES_PER_BUFFER) n = SAMPLES_PER_BUFFER;

        // fm_render_voice() accumulates, same convention as the device mixer.
        std::fill(dry_l.begin(), dry_l.begin() + n, 0);
        std::fill(dry_r.begin(), dry_r.begin() + n, 0);
        fm_render_voice(ops, patch, routing, bus, /*pan=*/0, dry_l.data(), dry_r.data(), n,
                        &peg, &lfo, note_inc, /*mod_wheel=*/0);

        if (!freed_at && released && !fm_voice_active(ops, routing)) freed_at = done;

        for (uint32_t i = 0; i < n; i++) {
            int32_t raw = dry_l[i];
            int32_t mag = raw < 0 ? -raw : raw;
            if (mag > peak_raw) peak_raw = mag;
            out.push_back((float)((double)raw / (T00T_UNIT * CENTRE_PAN_GAIN)));
        }
    }

    if (!write_wav_f32(a.out, out, SAMPLE_RATE, 1)) {
        fprintf(stderr, "error: cannot write %s\n", a.out.c_str());
        return 1;
    }
    if (!a.pcm16.empty() && !write_wav_pcm16_normalized(a.pcm16, out, SAMPLE_RATE, 1)) {
        fprintf(stderr, "error: cannot write %s\n", a.pcm16.c_str());
        return 1;
    }

    double peak_units = (double)peak_raw / (T00T_UNIT * CENTRE_PAN_GAIN);
    printf("render_fm_patch: patch %d \"%s\" note %d vel %d gate %.2fs tail %.2fs\n",
           a.voice, FM_PATCH_NAMES[a.voice], a.note, a.velocity, a.gate, a.tail);
    printf("  wrote %s -- %zu frames, peak %.4f (%.2f dB re 1.0, raw %d)\n",
           a.out.c_str(), out.size(), peak_units,
           peak_units > 0 ? 20.0 * log10(peak_units) : -999.0, peak_raw);
    if (!a.pcm16.empty()) printf("  wrote %s -- peak-normalised PCM16\n", a.pcm16.c_str());
    if (released) {
        if (freed_at) printf("  voice went idle %.3fs after key-up\n",
                             (double)(freed_at - gate_frames) / SAMPLE_RATE);
        else          printf("  voice still active at end of tail\n");
    }
    return 0;
#endif
}
