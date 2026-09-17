// Host-side correctness check + listening aid for
// tools/ppg/convert_ppg_waves.py's output (src/engines/wavetable/
// ppg_waves.h): confirms every generated wavetable's baked-in
// WaveTableIndexEntry references an in-range waveform, then renders a few
// tables at a fixed note with wave-position swept 0..63 through
// osc/wavetable.h's own bilinear read -- the same "prove the data before
// wiring it into a real engine" step the tracker/FM/speech modules'
// own conversion tools all take.
//
// Only builds when ppg_waves.h has actually been generated locally (see
// tools/host_render/CMakeLists.txt's EXISTS gate) -- it is git-ignored,
// derived from third-party ROM content, same as the FM module's patches.h.
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_ppg_waves
#include "../../src/osc/wavetable.h"
#include "../../src/engines/wavetable/ppg_waves.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <cstdio>
#include <vector>

static bool check_ranges() {
    bool ok = true;
    for (int t = 0; t < PPG_WAVETABLE_COUNT; t++) {
        for (int i = 0; i < (int)WT_INDEX_SIZE; i++) {
            const WaveTableIndexEntry &e = ppg_wavetable_index[t][i];
            if (e.wave_a >= PPG_WAVE_COUNT || e.wave_b >= PPG_WAVE_COUNT) {
                printf("FAIL: table %d slot %d references out-of-range wave (%u,%u)\n",
                       t, i, e.wave_a, e.wave_b);
                ok = false;
            }
        }
    }
    return ok;
}

static void render_sweep(int table, const char *path, float freq_hz, float sweep_seconds) {
    WaveTable wt{ ppg_wave_data, (uint8_t)PPG_WAVE_COUNT, ppg_wavetable_index[table] };
    uint32_t phase = 0;
    uint32_t inc = wavetable_phase_inc(freq_hz);
    uint32_t total = (uint32_t)(SAMPLE_RATE * sweep_seconds);
    std::vector<int16_t> out(total * 2);
    for (uint32_t i = 0; i < total; i++) {
        uint8_t wave_pos = (uint8_t)((uint64_t)i * (WT_INDEX_SIZE - 1) / total);
        int32_t s = wavetable_read_bilinear(wt, phase, wave_pos);
        int16_t s16 = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        out[i * 2 + 0] = s16;
        out[i * 2 + 1] = s16;
        phase += inc;
    }
    write_wav_pcm16(path, out, SAMPLE_RATE, 2);
    printf("wrote %s (table %d, %.1f Hz, %.1fs wave-position sweep)\n", path, table, freq_hz, sweep_seconds);
}

int main() {
    bool ok = check_ranges();
    printf("%s: range check across %d wavetables (%d waveforms)\n",
           ok ? "PASS" : "FAIL", (int)PPG_WAVETABLE_COUNT, (int)PPG_WAVE_COUNT);

    render_sweep(0, "ppg_wave_table0_sweep.wav", 110.0f, 3.0f);
    render_sweep(4, "ppg_wave_table4_sweep.wav", 110.0f, 3.0f);
    // Table 13: the one whose ROM record references waveforms 244/245,
    // clamped to the last real waveform by the converter (README.md's
    // "Wavetable 13 references waveforms 244 and 245" note) -- rendered
    // here specifically so that clamp's audible effect can be checked.
    render_sweep(13, "ppg_wave_table13_sweep.wav", 110.0f, 3.0f);

    return ok ? 0 : 1;
}
