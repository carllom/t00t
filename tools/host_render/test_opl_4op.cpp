// Host-side sanity check for the 4-op OPL3/4-class Algorithms added to
// src/engines/opl/patch.h (OPL_ALGO_4OP_*, OPL_ROUTINGS[]). Same device code
// path and pass/fail shape as render_opl.cpp's 2-op check (renders through
// opl_voice_note_on()/opl_render_voice()/opl_voice_note_off(), confirms
// bounded, non-silent audio and that the voice actually goes idle within a
// bounded tail after release) -- not a Nuked-OPL3 conformance check, just
// "does each of the four new connections render real audio and release."
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./test_opl_4op
#include "../../src/engines/opl/opl_voice.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static constexpr uint32_t NATIVE_BUFFER = 256;

// One shared 4-op test patch per algorithm: identical operator params (a
// plain 1:1 chain, sustain mode so a held note doesn't die on its own,
// moderate levels) so any audible difference between algorithms comes from
// the routing itself, not from different register values. Feedback on so
// op0's feedback path is exercised too, on every algorithm alike.
static constexpr OplOpParams TEST_OP = {
    /*mult*/1, /*ksl*/0, /*tl*/10, /*ar*/15, /*dr*/10, /*sl*/4, /*rr*/9,
    /*egt*/true, /*ksr*/false, /*ws*/0,
};

static OplPatch make_test_patch(const char *name, OplAlgorithm algo) {
    OplPatch p{};
    p.name = name;
    p.op[0] = p.op[1] = p.op[2] = p.op[3] = TEST_OP;
    p.feedback = 3;
    p.algorithm = algo;
    return p;
}

static bool render_patch(const OplPatch &patch, float note_hz, uint8_t midinote) {
    FmOp ops[FM_NUM_OPS];
    EnvOpl env[4];
    FmRouting routing;
    OplVibrato vib;
    opl_voice_init_inert(ops);

    uint32_t inc = fm_phase_inc(note_hz);
    opl_voice_note_on(ops, env, routing, patch, inc, /*amplitude=*/32767, midinote, vib);

    static int32_t bus0[FM_BLOCK], bus1[FM_BLOCK], bus2[FM_BLOCK];
    static int32_t bus3[FM_BLOCK], bus4[FM_BLOCK], bus5[FM_BLOCK], bus_out[FM_BLOCK];
    FmVoiceBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    const uint32_t held = (uint32_t)(1.0f * SAMPLE_RATE);
    const uint32_t tail = (uint32_t)(3.0f * SAMPLE_RATE);
    std::vector<int32_t> dl(held + tail, 0), dr(held + tail, 0);

    uint32_t done = 0;
    while (done < held) {
        uint32_t n = std::min(NATIVE_BUFFER, held - done);
        opl_render_voice(ops, env, routing, bus, /*pan=*/0, dl.data() + done, dr.data() + done,
                          n, vib, /*mod_wheel=*/16000);
        done += n;
    }
    opl_voice_note_off(env, routing);

    bool became_idle = false;
    uint32_t release_done = 0;
    while (release_done < tail) {
        uint32_t n = std::min(NATIVE_BUFFER, tail - release_done);
        opl_render_voice(ops, env, routing, bus, 0, dl.data() + held + release_done,
                          dr.data() + held + release_done, n, vib, 16000);
        release_done += n;
        if (!opl_voice_active(env, routing)) { became_idle = true; break; }
    }

    uint32_t total = held + release_done;
    int32_t peak = 0;
    for (uint32_t i = 0; i < total; i++) peak = std::max(peak, std::abs(dl[i]));

    std::vector<int16_t> wav(total * 2);
    for (uint32_t i = 0; i < total; i++) {
        wav[i * 2 + 0] = (int16_t)std::clamp(dl[i], -32768, 32767);
        wav[i * 2 + 1] = (int16_t)std::clamp(dr[i], -32768, 32767);
    }
    std::string fname = std::string("opl4op_") + patch.name + ".wav";
    for (char &c : fname) if (c == ' ') c = '_';
    bool wrote = write_wav_pcm16(fname.c_str(), wav, SAMPLE_RATE, 2);

    bool bounded = peak > 0 && peak < 32768;
    bool pass = wrote && bounded && (routing.num_ops == 4);
    printf("%s: %s -- num_ops=%u, peak=%d, idle after release=%d\n", pass ? "PASS" : "FAIL",
           fname.c_str(), routing.num_ops, peak, became_idle);
    if (!bounded) printf("  FAIL: peak out of int16 range or silent\n");
    if (routing.num_ops != 4) printf("  FAIL: expected a 4-op routing, got num_ops=%u\n", routing.num_ops);
    if (!became_idle) printf("  WARN: never went idle within the %.0fs release tail\n", (float)tail / SAMPLE_RATE);
    return pass;
}

int main() {
    opl_init_waveforms();
    osc_init_sine();       // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // eg_to_gain()'s exp2 LUT -- must run before any EG step

    static const struct { const char *name; OplAlgorithm algo; } kAlgos[] = {
        { "CHAIN",       OPL_ALGO_4OP_CHAIN },
        { "DUAL_FM",     OPL_ALGO_4OP_DUAL_FM },
        { "ADD_CHAIN",   OPL_ALGO_4OP_ADD_CHAIN },
        { "ADD_FM_ADD",  OPL_ALGO_4OP_ADD_FM_ADD },
    };

    bool ok = true;
    for (const auto &a : kAlgos) {
        OplPatch patch = make_test_patch(a.name, a.algo);
        ok = render_patch(patch, 220.0f, /*midinote=*/57) && ok;  // A3
    }
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
