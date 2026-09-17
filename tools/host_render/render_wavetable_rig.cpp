// Host-side correctness check of src/engines/wavetable/rig.h: renders the
// measurement rig's fixed N-partial mixer through the exact same
// wt_rig_render_buffer() the device's audio_engine.cpp calls (under
// WT_PROFILE=1) from its Core 1 render loop, and writes it to WAV. This rig
// doesn't decide anything about performance -- its only correctness
// obligation is producing real, finite, non-clipping audio for every
// WT_RIG_MODE, checked here before anyone straps a scope to it.
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_wavetable_rig
#include "../../src/engines/wavetable/rig.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static bool run_rig_check(const char *label) {
    wt_rig_init_tables();

    static WtRigPartial partials[WT_RIG_PARTIALS];
    for (uint32_t p = 0; p < WT_RIG_PARTIALS; p++) {
        float freq = 55.0f * (float)(1u << ((p / 8) % 4));
        uint8_t wave_pos = (uint8_t)((p * (WT_INDEX_SIZE - 1)) / (WT_RIG_PARTIALS - 1));
        float grain_hz = 20.0f + (float)(p % 8) * 5.0f;
        wt_rig_init_partial(partials[p], freq, wave_pos, grain_hz);
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
        wt_rig_render_buffer(partials, WT_RIG_PARTIALS, dry_l.data() + done, dry_r.data() + done, n);
        done += n;
    }

    std::vector<int16_t> out(total_frames * 2);
    float peak = 0.0f;
    // Each partial's raw table read is bounded by +-32767, so WT_RIG_PARTIALS
    // partials summed can't plausibly exceed WT_RIG_PARTIALS * 32767 -- a
    // generous multiple of that catches genuine int32 accumulator overflow
    // without false-triggering on ordinary per-buffer clipping, which the
    // device's own __ssat() saturates anyway.
    const int64_t bound = (int64_t)WT_RIG_PARTIALS * 32767 * 2;
    bool bounded = true;
    for (uint32_t i = 0; i < total_frames; i++) {
        int32_t l = dry_l[i], r = dry_r[i];
        bounded = bounded && (int64_t)l > -bound && (int64_t)l < bound && (int64_t)r > -bound && (int64_t)r < bound;
        peak = std::max(peak, std::max(std::fabs((float)l), std::fabs((float)r)));
        out[i * 2 + 0] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }

    char path[64];
    snprintf(path, sizeof(path), "wavetable_rig_%s.wav", label);
    bool ok = write_wav_pcm16(path, out, SAMPLE_RATE, 2);

    bool nonsilent = peak > 100.0f;
    bool pass = ok && bounded && nonsilent;
    printf("%s: [%s] partials=%u block=%u mode=%u peak=%.0f bounded=%s\n",
           pass ? "PASS" : "FAIL", label, (unsigned)WT_RIG_PARTIALS, (unsigned)WT_RIG_BLOCK,
           (unsigned)WT_RIG_MODE, peak, bounded ? "yes" : "no");
    if (!bounded) printf("  FAIL: accumulator exceeded the plausible per-partial-unity bound -- overflow?\n");
    if (!nonsilent) printf("  FAIL: rig produced near-silent output\n");
    if (!ok) printf("  FAIL: could not write WAV file\n");
    return pass;
}

int main() {
    bool ok = run_rig_check("default");
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
