// Host-side proof of src/engines/fm/render.h (#41): renders the FM engine
// skeleton's test tone through the FM-specific 4096-entry sine table (no
// interpolation, phase >> 20) using the exact same fm_render_test_tone() the
// device's audio_engine.cpp calls from its Core 1 render loop. Dexed is the
// eventual ground-truth reference for this module (fm.md §7), but that's a
// P3+ concern -- this harness only proves the table/phase seam before any
// operator kernel exists, same role render_speech.cpp's ZOH check played
// for #27. No pico-sdk, no ARM intrinsics -- render.h only touches
// sine_tab.h and pan.h (see its own header comment for why that matters).
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_fm
#include "../../src/engines/fm/render.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

static constexpr float    TEST_TONE_HZ = 440.0f;
static constexpr uint32_t NATIVE_BUFFER = 256;  // matches the device's default SAMPLES_PER_BUFFER

int main() {
    fm_init_sine_tab();  // fm_render_test_tone()'s table source
    osc_init_sine();     // pan.h's pan_gains_q15() reuses the shared sine table for its quadrature gains

    const uint32_t inc = fm_phase_inc(TEST_TONE_HZ);
    uint32_t phase = 0;

    const uint32_t total_frames = SAMPLE_RATE * 2;  // 2 seconds
    std::vector<int16_t> out(total_frames * 2);      // interleaved stereo

    std::vector<int32_t> dry_l(NATIVE_BUFFER), dry_r(NATIVE_BUFFER);

    uint32_t done = 0;
    int32_t peak = 0;
    while (done < total_frames) {
        uint32_t n = std::min(NATIVE_BUFFER, total_frames - done);
        fm_render_test_tone(phase, inc, /*pan=*/0, dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n; i++) {
            int16_t l = (int16_t)dry_l[i];
            int16_t r = (int16_t)dry_r[i];
            out[(done + i) * 2 + 0] = l;
            out[(done + i) * 2 + 1] = r;
            peak = std::max({ peak, (int32_t)(l < 0 ? -l : l), (int32_t)(r < 0 ? -r : r) });
        }
        done += n;
    }

    bool ok = write_wav_pcm16("fm_test_tone.wav", out, SAMPLE_RATE, 2);

    // Table sanity: a 4096-entry table with quarter-wave symmetric
    // generation (fm.md §5.1) must still be an odd function about the origin
    // and zero at phase 0 -- the actual invariant this skeleton's table
    // generation depends on, independent of anything audible.
    bool table_ok = (fm_sine_table[0] == 0);
    for (uint32_t i = 1; i < FM_TABLE_SIZE && table_ok; i++) {
        table_ok = (fm_sine_table[i] == (int16_t)-fm_sine_table[FM_TABLE_SIZE - i]);
    }

    printf("%s: wrote fm_test_tone.wav -- %u frames @ %u Hz (%.1f Hz tone), peak=%d\n",
           ok && table_ok ? "PASS" : "FAIL", total_frames, SAMPLE_RATE, TEST_TONE_HZ, peak);
    if (!table_ok) printf("  FAIL: fm_sine_table isn't an odd function about the origin\n");
    if (!ok) printf("  FAIL: could not write WAV file\n");

    return (ok && table_ok) ? 0 : 1;
}
