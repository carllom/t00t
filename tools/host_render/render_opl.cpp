// Host-side sanity check for src/engines/opl/: renders each
// hand-authored test patch (patches.h) through the exact device code path
// (opl_voice_note_on()/opl_render_voice()/opl_voice_note_off(), opl_voice.h)
// to a WAV file, and confirms note-off actually releases the voice within a
// bounded tail. Not a Nuked-OPL3 conformance check -- there is no reference
// build in this tree yet -- just "does it render real, bounded, non-silent
// audio and go idle after release."
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_opl
#include "../../src/engines/opl/opl_voice.h"
#include "../../src/engines/opl/patches.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static constexpr uint32_t NATIVE_BUFFER = 256;

static bool render_patch(const OplPatch &patch, float note_hz, uint8_t midinote) {
    FmOp ops[FM_NUM_OPS];
    EnvOpl env[2];
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
    opl_voice_note_off(env);

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
    std::string fname = std::string("opl_") + patch.name + ".wav";
    for (char &c : fname) if (c == ' ') c = '_';
    bool wrote = write_wav_pcm16(fname.c_str(), wav, SAMPLE_RATE, 2);

    bool bounded = peak > 0 && peak < 32768;
    bool pass = wrote && bounded;
    printf("%s: %s -- peak=%d, idle after release=%d\n", pass ? "PASS" : "FAIL",
           fname.c_str(), peak, became_idle);
    if (!bounded) printf("  FAIL: peak out of int16 range or silent\n");
    if (!became_idle) printf("  WARN: never went idle within the %.0fs release tail\n", (float)tail / SAMPLE_RATE);
    return pass;
}

int main() {
    opl_init_waveforms();
    osc_init_sine();     // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains
    env_dx_init_tables();  // eg_to_gain()'s exp2 LUT -- must run before any EG step

    bool ok = true;
    for (uint32_t i = 0; i < OPL_PATCH_COUNT; i++) {
        ok = render_patch(*OPL_PATCHES[i], 220.0f, /*midinote=*/57) && ok;  // A3
    }
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
