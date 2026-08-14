// Host-side correctness check of src/engines/fm/rig.h (#42): renders the P0
// measurement rig's fixed 6-operator x N-voice topology through the exact
// same fm_rig_render_buffer() the device's audio_engine.cpp calls (under
// FM_PROFILE=1) from its Core 1 render loop, and writes it to WAV. This rig
// doesn't decide anything about performance (that's the bench session this
// issue is blocked ahead of) — its only correctness obligation is "produces
// real, finite, non-clipping audio for every kernel path and every §3.6
// lever combination," checked here before anyone straps a scope to it.
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_fm_rig
#include "../../src/engines/fm/rig.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static bool run_rig_check(const char *label) {
    fm_rig_init_table();

    static FmRigOp voices[FM_RIG_VOICES][6];
    static int32_t bus0[FM_RIG_BLOCK], bus1[FM_RIG_BLOCK], bus2[FM_RIG_BLOCK];
    static int32_t bus3[FM_RIG_BLOCK], bus4[FM_RIG_BLOCK], bus5[FM_RIG_BLOCK];
    static int32_t bus_out[FM_RIG_BLOCK];
    FmRigBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    for (uint32_t v = 0; v < FM_RIG_VOICES; v++) {
        float base_hz = 55.0f * (float)(1u << (v / 4));
        fm_rig_init_voice(voices[v], base_hz, (float)SAMPLE_RATE);
    }

    const uint32_t total_frames = SAMPLE_RATE * 2;  // 2 seconds
    std::vector<int32_t> dry_l(total_frames), dry_r(total_frames);

    // Rendered in device-sized chunks (SAMPLES_PER_BUFFER-equivalent),
    // matching how audio_engine.cpp calls this per DMA buffer rather than
    // once for the whole clip.
    constexpr uint32_t CHUNK = 256;
    uint32_t done = 0;
    while (done < total_frames) {
        uint32_t n = total_frames - done;
        if (n > CHUNK) n = CHUNK;
        fm_rig_render_buffer(voices, bus, FM_RIG_VOICES, dry_l.data() + done, dry_r.data() + done, n);
        done += n;
    }

    std::vector<int16_t> out(total_frames * 2);
    float peak = 0.0f;
    // Each voice's carrier is scaled to roughly unity (fm_rig_init_voice's
    // CARRIER_GAIN), so FM_RIG_VOICES voices summed can't plausibly exceed
    // ~FM_RIG_VOICES * 32767 -- a generous multiple of that catches genuine
    // int32 accumulator overflow/wraparound (which would show up as a huge
    // or wildly negative swing) without false-triggering on ordinary
    // per-buffer clipping, which the device's own __ssat() saturates anyway.
    const int64_t bound = (int64_t)FM_RIG_VOICES * 32767 * 4;
    bool bounded = true;
    for (uint32_t i = 0; i < total_frames; i++) {
        int32_t l = dry_l[i], r = dry_r[i];
        bounded = bounded && (int64_t)l > -bound && (int64_t)l < bound && (int64_t)r > -bound && (int64_t)r < bound;
        peak = std::max(peak, std::max(std::fabs((float)l), std::fabs((float)r)));
        out[i * 2 + 0] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }

    char path[64];
    snprintf(path, sizeof(path), "fm_rig_%s.wav", label);
    bool ok = write_wav_pcm16(path, out, SAMPLE_RATE, 2);

    bool nonsilent = peak > 100.0f;
    bool pass = ok && bounded && nonsilent;
    printf("%s: [%s] voices=%u block=%u table_bits=%u interleave=%d not_in_flash=%d smulwb=%d fb=%d "
           "peak=%.0f bounded=%s\n",
           pass ? "PASS" : "FAIL", label, (unsigned)FM_RIG_VOICES, (unsigned)FM_RIG_BLOCK,
           (unsigned)FM_RIG_TABLE_BITS, FM_RIG_INTERLEAVE, FM_RIG_NOT_IN_FLASH, FM_RIG_SMULWB,
           FM_RIG_FB, peak, bounded ? "yes" : "no");
    if (!bounded) printf("  FAIL: accumulator exceeded the plausible per-voice-unity bound -- overflow?\n");
    if (!nonsilent) printf("  FAIL: rig produced near-silent output\n");
    if (!ok) printf("  FAIL: could not write WAV file\n");
    return pass;
}

int main() {
    bool ok = run_rig_check("default");
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
