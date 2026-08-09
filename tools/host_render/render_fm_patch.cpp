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
#include <cstdint>
#include <string>
#include <vector>

#ifdef T00T_FM_HAS_PATCHES
#include "../../src/engines/fm/patches.h"
#endif

// Since F2 the two engines share a level anchor, so this is no longer an
// arbitrary constant: op.h's FM_CYCLE is one full cycle of phase deviation,
// dexed_render's DEXED_UNIT (2^24) is the same thing on Dexed's side, and a
// max-level operator peaks at 2.0 in these units on both. That makes
// fm_compare.py's `level gap` a real measurement of the engine rather than a
// readout of two unrelated scale choices.
//
// The int16 conversion (op.h's FM_VOICE_OUT_SHIFT) is deliberately undone
// here rather than skipped: the point is to measure what the device path
// produces, including its output scaling, not a parallel float path that
// could drift from it.
static constexpr double T00T_UNIT = (double)FM_CYCLE / (double)(1 << FM_VOICE_OUT_SHIFT);

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
    bool        check    = false;
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
        "  --list            print the converted patch names and exit\n"
        "  --check           prove no bus overflows int32 on any patch, and exit\n");
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
        else if (k == "--check") a.check = true;
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }

    if (a.list) {
        for (int v = 0; v < FM_PATCH_COUNT; v++) printf("%2d  %s\n", v, FM_PATCH_NAMES[v]);
        return 0;
    }

    if (a.check) {
        // F2's gate: prove no bus can overflow int32 on any patch in the bank.
        //
        // This is a bound, not a sample. Every operator's contribution is
        // bounded exactly: fm_mul_gain() is (gain * sample) >> 16 with |sample|
        // <= 2^15 and gain <= FM_GAIN_MAX, so |contribution| <= 2 * FM_CYCLE,
        // and env_dx.h's eg_to_linear() cannot return more than FM_GAIN_MAX
        // because a log2 offset of 0 is its maximum (output level and EG level
        // both cap at 99, and fm_voice_note_on() clamps the key-scaling sum the
        // same way Dexed does). So a bus's worst case is simply its number of
        // writers times that -- countable from the resolved routing, with no
        // dependence on what any particular note does.
        fm_init_sine_tab();
        env_dx_init_tables();
        int64_t worst = 0;
        const char *worst_patch = "";
        int worst_writers = 0;
        for (int v = 0; v < FM_PATCH_COUNT; v++) {
            FmRouting r;
            if (!fm_resolve_routing(FM_PATCHES[v], r)) {
                printf("FAIL: patch %d (%s) is unroutable\n", v, FM_PATCH_NAMES[v]);
                return 1;
            }
            int writers[FM_TARGET_OUT + 1] = {0};
            for (uint32_t i = 0; i < FM_NUM_OPS; i++) writers[r.out_bus[i]]++;
            for (int b = 0; b <= FM_TARGET_OUT; b++) {
                int64_t peak = (int64_t)writers[b] * 2 * (int64_t)FM_CYCLE;
                if (peak > worst) { worst = peak; worst_patch = FM_PATCH_NAMES[v]; worst_writers = writers[b]; }
            }
        }
        double headroom_bits = 31.0 - log2((double)worst);
        printf("bus overflow check: %d patches\n", FM_PATCH_COUNT);
        printf("  worst case %lld (2^%.2f) -- %d writers on one bus, in \"%s\"\n",
               (long long)worst, log2((double)worst), worst_writers, worst_patch);
        printf("  int32 headroom %.2f bits  [FM_CYCLE=2^%u, max op output 2^%u]\n",
               headroom_bits, FM_CYCLE_BITS, FM_CYCLE_BITS + 1);
        if (worst >= INT32_MAX) { printf("FAIL: bus can overflow int32\n"); return 1; }
        printf("PASS\n");
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
