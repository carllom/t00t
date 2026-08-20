// render_opl_patch -- t00t's OPL engine, rendered on the host with a CLI
// close enough to tools/opl_ref/nuked_render's own that tools/opl_compare.py
// is a thin diff rather than a translation layer. Same device code path
// tools/host_render/render_opl.cpp's sanity check uses
// (opl_voice_note_on()/opl_render_voice()/opl_voice_note_off(), opl_voice.h),
// but CLI-driven by note/velocity/gate/tail instead of that tool's fixed
// A3-at-full-velocity sweep over every patch.
//
// `--list` prints one CSV row per src/engines/opl/patches.h patch, fields in
// nuked_render's own --op0/--op1 order (mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws)
// plus feedback/algorithm -- so the Python side builds nuked_render's
// arguments straight from patches.h, the single source of truth, rather than
// keeping a second hand-copied table in sync with it.
//
// Vibrato is fixed off (mod_wheel=0): Nuked-OPL3's own vibrato register
// (AM/VIB, 0xBD) is never set by nuked_render, and a real pitch modulation on
// only one side would read as a harmonic-tracking error that has nothing to
// do with the engine under test.
//
// Build: part of the shared CMake host build (tools/host_render/CMakeLists.txt).

#include "../../src/engines/opl/opl_voice.h"
#include "../../src/engines/opl/patches.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static constexpr uint32_t NATIVE_BUFFER = 256;

struct Args {
    int         patch    = 0;
    int         note     = 57;   // A3, matching nuked_render's own default
    int         velocity = 127;
    double      gate     = 1.0;
    double      tail     = 2.0;
    std::string out      = "t00t.wav";
    bool        list     = false;
};

static void usage() {
    printf(
        "render_opl_patch -- t00t OPL engine, host render (matches nuked_render's CLI)\n"
        "\n"
        "  --patch N         patch index into src/engines/opl/patches.h (default 0)\n"
        "  --note N          MIDI note number (default 57 = A3)\n"
        "  --vel N           MIDI velocity 1-127 (default 127)\n"
        "  --gate SEC        seconds to hold the note (default 1.0)\n"
        "  --tail SEC        seconds to render after key-up (default 2.0)\n"
        "  --out PATH        stereo PCM16 WAV (default t00t.wav)\n"
        "  --list            print every patch's registers as CSV and exit\n");
}

static void print_patch_list() {
    printf("# domain=patches cols=idx,name,mult0,ksl0,tl0,ar0,dr0,sl0,rr0,egt0,ksr0,ws0,"
           "mult1,ksl1,tl1,ar1,dr1,sl1,rr1,egt1,ksr1,ws1,feedback,algo\n");
    for (uint32_t i = 0; i < OPL_PATCH_COUNT; i++) {
        const OplPatch &p = *OPL_PATCHES[i];
        const OplOpParams &o0 = p.op[0], &o1 = p.op[1];
        printf("%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s\n",
               i, p.name,
               o0.mult, o0.ksl, o0.tl, o0.ar, o0.dr, o0.sl, o0.rr, o0.egt, o0.ksr, o0.ws,
               o1.mult, o1.ksl, o1.tl, o1.ar, o1.dr, o1.sl, o1.rr, o1.egt, o1.ksr, o1.ws,
               p.feedback, p.algorithm == OPL_ALGO_ADD ? "add" : "fm");
    }
}

int main(int argc, char **argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", k.c_str()); exit(2); }
            return argv[++i];
        };
        if      (k == "--patch") a.patch = atoi(next());
        else if (k == "--note")  a.note = atoi(next());
        else if (k == "--vel")   a.velocity = atoi(next());
        else if (k == "--gate")  a.gate = atof(next());
        else if (k == "--tail")  a.tail = atof(next());
        else if (k == "--out")   a.out = next();
        else if (k == "--list")  a.list = true;
        else if (k == "-h" || k == "--help") { usage(); return 0; }
        else { fprintf(stderr, "error: unknown argument %s\n", k.c_str()); usage(); return 2; }
    }

    if (a.list) {
        print_patch_list();
        return 0;
    }
    if (a.patch < 0 || a.patch >= (int)OPL_PATCH_COUNT) {
        fprintf(stderr, "error: --patch must be 0-%d (patches.h holds %u patches)\n",
                (int)OPL_PATCH_COUNT - 1, OPL_PATCH_COUNT);
        return 2;
    }

    opl_init_waveforms();
    osc_init_sine();      // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables(); // eg_to_gain()'s exp2 LUT -- must run before any EG step

    const OplPatch &patch = *OPL_PATCHES[a.patch];

    FmOp ops[FM_NUM_OPS];
    EnvOpl env[2];
    FmRouting routing;
    OplVibrato vib;
    opl_voice_init_inert(ops);

    // Same note->frequency conversion and velocity->amplitude scaling render_fm_patch
    // uses; env_opl_init() re-derives velocity from `amplitude` (env_opl.h), so this
    // round-trips through the same formula nuked_render's --vel takes directly.
    const float freq_hz = 440.0f * powf(2.0f, (float)(a.note - 69) / 12.0f);
    const uint32_t note_inc = fm_phase_inc(freq_hz);
    const int16_t amplitude = (int16_t)((a.velocity / 127.0f) * 32767.0f);

    opl_voice_note_on(ops, env, routing, patch, note_inc, amplitude, (uint8_t)a.note, vib);

    static int32_t bus0[FM_BLOCK], bus1[FM_BLOCK], bus2[FM_BLOCK];
    static int32_t bus3[FM_BLOCK], bus4[FM_BLOCK], bus5[FM_BLOCK], bus_out[FM_BLOCK];
    FmVoiceBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    const uint32_t gate_frames  = (uint32_t)(a.gate * SAMPLE_RATE);
    const uint32_t total_frames = (uint32_t)((a.gate + a.tail) * SAMPLE_RATE);
    std::vector<int32_t> dl(total_frames, 0), dr(total_frames, 0);

    bool released = false, became_idle = false;
    uint32_t freed_at = 0;
    for (uint32_t done = 0; done < total_frames; ) {
        if (!released && done >= gate_frames) {
            opl_voice_note_off(env);
            released = true;
        }
        uint32_t n = std::min(NATIVE_BUFFER, total_frames - done);
        opl_render_voice(ops, env, routing, bus, /*pan=*/0, dl.data() + done, dr.data() + done,
                          n, vib, /*mod_wheel=*/0);
        done += n;
        if (released && !opl_voice_active(env, routing)) {
            became_idle = true;
            freed_at = done;
            break;
        }
    }

    uint32_t total = became_idle ? freed_at : total_frames;
    std::vector<int16_t> wav(total_frames * 2, 0);
    int16_t peak = 0;
    for (uint32_t i = 0; i < total; i++) {
        int16_t l = (int16_t)std::clamp(dl[i], -32768, 32767);
        int16_t r = (int16_t)std::clamp(dr[i], -32768, 32767);
        wav[i * 2 + 0] = l;
        wav[i * 2 + 1] = r;
        peak = std::max(peak, (int16_t)std::abs((int)l));
    }

    if (!write_wav_pcm16(a.out, wav, SAMPLE_RATE, 2)) {
        fprintf(stderr, "error: cannot write %s\n", a.out.c_str());
        return 1;
    }

    printf("render_opl_patch: patch %d \"%s\" note %d vel %d gate %.2fs tail %.2fs\n",
           a.patch, patch.name, a.note, a.velocity, a.gate, a.tail);
    printf("  wrote %s -- %u frames, peak %d\n", a.out.c_str(), total_frames, peak);
    if (released) {
        if (became_idle) printf("  voice went idle %.3fs after key-up\n",
                                 (double)(freed_at - gate_frames) / SAMPLE_RATE);
        else              printf("  voice still active at end of tail\n");
    }
    return 0;
}
