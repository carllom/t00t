// Host-side proof of src/res2p.h: renders it to WAV and checks its behaviour
// objectively (pole stability, resonant frequency, decay) before it gets
// wired into any real-time engine. See issue #5 phase 2 — "the right place
// to prove res2p.h objectively, instead of rewriting working 808 sounds to
// do it."
//
// Run from the build directory (tools/host_render/build) so the WAVs land
// next to the binary, in the git-ignored build/ tree:
//   cmake -S .. -B . && cmake --build . && ./render_res2p
#include "res2p.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float FS = 44100.0f;

// #28: res2p_radius() moved from expf() to a LUT + linear interpolation.
// Proves the LUT matches expf() to an inaudible margin across every (bw, fs)
// this codebase uses before relying on it in the speech formant cascade.
static bool test_radius_lut_accuracy() {
    bool all_ok = true;
    float max_err = 0.0f;
    struct { float fs; float bw_lo, bw_hi; } ranges[] = {
        { 44100.0f,  20.0f, 400.0f },  // groovebox resonators (render_res2p sweep)
        { 22050.0f,  50.0f, 400.0f },  // speech formants @ SPEECH_RATE (speech.md)
    };
    for (auto &r : ranges) {
        for (float bw = r.bw_lo; bw <= r.bw_hi; bw += 2.0f) {
            float exact = expf(-(float)M_PI * bw / r.fs);
            float approx = res2p_radius(bw, r.fs);
            float err = std::fabs(approx - exact);
            max_err = std::max(max_err, err);
            if (err > 1e-4f) {
                printf("  FAIL fs=%.0f bw=%.1f -> exact=%.6f approx=%.6f err=%.6f\n",
                       r.fs, bw, exact, approx, err);
                all_ok = false;
            }
        }
    }
    printf("  max |approx - exact| = %.7f over both ranges\n", max_err);
    return all_ok;
}

// Sweeps the (f, bw) ranges the groovebox backport and the speech formant
// cascade actually use, and checks every pole lands strictly inside the
// unit circle (a2 < 1.0f) — the same assertion speech.md prescribes as a
// runtime debug check after every res2p_set().
static bool test_stability_sweep() {
    bool all_ok = true;
    int checked = 0;
    for (float f = 50.0f; f <= 4000.0f; f += 50.0f) {
        for (float bw = 20.0f; bw <= 400.0f; bw += 20.0f) {
            Res2p r{};
            res2p_set(r, f, bw, FS);
            checked++;
            if (!(r.a2 >= 0.0f && r.a2 < 1.0f)) {
                printf("  FAIL f=%.0f bw=%.0f -> a2=%.5f (pole outside unit circle)\n", f, bw, r.a2);
                all_ok = false;
            }
        }
    }
    printf("  %d (f, bw) combinations checked\n", checked);
    return all_ok;
}

// Zero-crossing frequency estimate over [start, end).
static float measure_frequency(const std::vector<float> &y, size_t start, size_t end, float fs) {
    int crossings = 0;
    for (size_t i = start + 1; i < end; i++) {
        if ((y[i - 1] < 0.0f) != (y[i] < 0.0f)) crossings++;
    }
    float seconds = (float)(end - start) / fs;
    return (crossings / 2.0f) / seconds;
}

static double rms_window(const std::vector<float> &y, size_t s, size_t e) {
    double sum = 0.0;
    for (size_t i = s; i < e; i++) sum += (double)y[i] * (double)y[i];
    return std::sqrt(sum / (double)(e - s));
}

// Feeds an impulse through a res2p tuned to (target_f, target_bw), writes
// the response to WAV, and checks it rings at the right frequency and then
// decays (a lossy resonator, not a sustaining or diverging one).
static bool test_impulse_response(float target_f, float target_bw, const char *wav_name) {
    Res2p r{};
    res2p_set(r, target_f, target_bw, FS);

    const int N = (int)FS;  // 1 second
    std::vector<float> y(N);
    std::vector<int16_t> pcm(N);

    for (int i = 0; i < N; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;
        float out = res2p_tick(r, x);
        if (!std::isfinite(out)) {
            printf("  FAIL: non-finite output at sample %d (f=%.0f bw=%.0f)\n", i, target_f, target_bw);
            return false;
        }
        y[i] = out;
        float clipped = std::max(-1.0f, std::min(1.0f, out * 8.0f));
        pcm[i] = (int16_t)std::lround(clipped * 32000.0f);
    }
    write_wav_pcm16(wav_name, pcm, (uint32_t)FS);

    // Early steady window: past the initial transient, well before decay.
    size_t win_start = (size_t)(FS * 0.01f);
    size_t win_end   = (size_t)(FS * 0.05f);
    float measured_f = measure_frequency(y, win_start, win_end, FS);
    float f_err = std::fabs(measured_f - target_f) / target_f;

    double early_rms = rms_window(y, win_start, win_end);
    double late_rms  = rms_window(y, N - (size_t)(FS * 0.05f), N);
    bool decays = late_rms < early_rms * 0.1;

    printf("  f=%.0f Hz bw=%.0f Hz -> measured %.1f Hz (%.1f%% err), early RMS=%.5f late RMS=%.5f, decays=%s -> %s\n",
           target_f, target_bw, measured_f, f_err * 100.0f, early_rms, late_rms,
           decays ? "yes" : "no", (f_err < 0.05f && decays) ? "PASS" : "FAIL");

    return f_err < 0.05f && decays;
}

int main() {
    bool ok = true;
    res2p_init();

    printf("== res2p_radius() LUT vs expf() ==\n");
    ok = test_radius_lut_accuracy() && ok;

    printf("\n== res2p stability sweep (50-4000 Hz x 20-400 Hz bandwidth) ==\n");
    ok = test_stability_sweep() && ok;

    printf("\n== res2p impulse response (writes WAV alongside the binary) ==\n");
    // Low + narrow: 808 tom/conga register.
    ok = test_impulse_response(90.0f, 30.0f, "res2p_tom.wav") && ok;
    // Mid + wider: a vowel formant.
    ok = test_impulse_response(700.0f, 90.0f, "res2p_formant.wav") && ok;
    // High + wide: an upper formant / cymbal-ish partial.
    ok = test_impulse_response(2500.0f, 200.0f, "res2p_formant_hi.wav") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
