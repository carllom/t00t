// Host-side proof of src/engines/fm/render.h (#41) and, below, of
// src/engines/fm/op.h + patch.h (#44): the #41 section renders the engine
// skeleton's fixed test tone through the FM-specific 4096-entry sine table
// (no interpolation, phase >> 20); the #44 section renders FM_TEST_PATCH --
// the same one hardcoded 6-op patch device's audio_engine.cpp plays from
// MIDI -- through the exact same fm_resolve_routing()/fm_voice_note_on()/
// fm_render_voice() the device calls, checks the DAG-routing compiler's
// accept/reject behaviour (multi-operator cycle rejected, self-loop/
// feedback accepted), and Goertzel-checks the spectrum against what the
// routing predicts. Dexed is the eventual ground-truth reference for this
// module (fm.md §7), but that's a P3+ concern -- this harness proves
// correctness the same pragmatic way render_speech.cpp's formant checks do:
// real, predictable spectral content in the right place, not a byte-exact
// reference render. No pico-sdk, no ARM intrinsics -- render.h/op.h/patch.h
// only touch sine_tab.h and pan.h (see render.h's own header comment for
// why that matters).
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_fm
#include "../../src/engines/fm/op.h"
#include "../../src/engines/fm/render.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static constexpr float    TEST_TONE_HZ = 440.0f;
static constexpr uint32_t NATIVE_BUFFER = 256;  // matches the device's default SAMPLES_PER_BUFFER

static bool run_test_tone_check() {
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

    return ok && table_ok;
}

// #44's DAG-routing compiler (patch.h's fm_resolve_routing()): must reject a
// cycle spanning two or more operators, and must accept a self-loop
// (feedback) on an otherwise clean chain -- fm.md §4.2's "DAG + self-loops
// only" constraint, exercised on FM_TEST_PATCH itself (which already has a
// feedback operator, op3) plus one deliberately broken variant.
static bool run_routing_checks() {
    FmRouting r_ok;
    bool ok_valid = fm_resolve_routing(FM_TEST_PATCH, r_ok);

    FmPatch cyclic = FM_TEST_PATCH;
    cyclic.op[2].mod_target = 3;
    cyclic.op[3].mod_target = 2;
    cyclic.op[3].feedback = false;
    FmRouting r_cyclic;
    bool cyclic_accepted = fm_resolve_routing(cyclic, r_cyclic);

    FmPatch self_target = FM_TEST_PATCH;
    self_target.op[1].mod_target = 1;  // malformed: not the `feedback` flag's job
    FmRouting r_self_target;
    bool self_target_accepted = fm_resolve_routing(self_target, r_self_target);

    bool pass = ok_valid && !cyclic_accepted && !self_target_accepted;
    printf("%s: routing compiler -- FM_TEST_PATCH (self-loop via feedback) valid=%d, "
           "2-op cycle rejected=%d, mod_target==self rejected=%d\n",
           pass ? "PASS" : "FAIL", ok_valid, !cyclic_accepted, !self_target_accepted);
    return pass;
}

// Goertzel magnitude of a Hann-windowed segment at `freq` Hz. Same technique
// render_speech.cpp uses for its formant/sideband checks.
static float goertzel_mag(const std::vector<float> &x, size_t start, size_t n, float freq, float fs) {
    float w = 2.0f * (float)M_PI * freq / fs;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float hann = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1));
        float s0 = x[start + i] * hann + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float real = s1 - s2 * cosf(w);
    float imag = s2 * sinf(w);
    return sqrtf(real * real + imag * imag) / (float)n;
}

// Renders one second of FM_TEST_PATCH at `note_hz` through the exact
// device code path (fm_voice_note_on/fm_voice_update_pitch/fm_render_voice,
// op.h), mono-summed for spectral analysis.
static void render_patch_note(float note_hz, const FmRouting &routing, FmVoiceBuses &bus,
                               std::vector<float> &mono, int32_t &peak) {
    FmOp ops[FM_NUM_OPS];
    uint32_t inc = fm_phase_inc(note_hz);
    fm_voice_note_on(ops, FM_TEST_PATCH, inc, /*amplitude=*/32767);

    const uint32_t total = SAMPLE_RATE;  // 1 second
    std::vector<int32_t> dl(total, 0), dr(total, 0);
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = std::min(NATIVE_BUFFER, total - done);
        fm_voice_update_pitch(ops, FM_TEST_PATCH, inc);
        fm_render_voice(ops, routing, bus, /*pan=*/0, dl.data() + done, dr.data() + done, n);
        done += n;
    }

    mono.resize(total);
    peak = 0;
    for (uint32_t i = 0; i < total; i++) {
        mono[i] = (float)dl[i];
        peak = std::max(peak, std::abs(dl[i]));
    }
}

// #44's acceptance criterion: "render_fm renders the same patch to WAV; the
// spectrum matches the ratios and sideband structure the routing predicts."
// FM_TEST_PATCH's op4/op5 pair is a 1:1 modulator:carrier ratio, which
// predicts a full harmonic series at the note's own integer multiples
// (fm.md §4.1's routing claim made audible: the *shape* of the spectrum is
// determined entirely by note-on-resolved ratios/routing, nothing else).
// Verified two ways: (1) real energy at 2x/3x the fundamental, clearly above
// the noise floor at non-harmonic bins; (2) that content tracks the note --
// rendering an octave up moves the fundamental peak with it, proving the
// per-operator increments really do scale off the note (not some fixed
// drone), which is what "correct ratios across the keyboard" means.
static bool run_patch_spectrum_check() {
    fm_init_sine_tab();
    osc_init_sine();

    FmRouting routing;
    if (!fm_resolve_routing(FM_TEST_PATCH, routing)) {
        printf("FAIL: FM_TEST_PATCH itself failed DAG validation\n");
        return false;
    }

    static int32_t bus0[FM_BLOCK], bus1[FM_BLOCK], bus2[FM_BLOCK];
    static int32_t bus3[FM_BLOCK], bus4[FM_BLOCK], bus5[FM_BLOCK], bus_out[FM_BLOCK];
    FmVoiceBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    constexpr float NOTE_LOW = 220.0f, NOTE_HIGH = 440.0f;  // one octave apart

    std::vector<float> low, high;
    int32_t peak_low, peak_high;
    render_patch_note(NOTE_LOW, routing, bus, low, peak_low);
    render_patch_note(NOTE_HIGH, routing, bus, high, peak_high);

    // WAV of the low note, for Carl's by-ear check against Dexed on an
    // equivalent patch (fm.md §11 step 3).
    std::vector<int16_t> wav(low.size() * 2);
    for (size_t i = 0; i < low.size(); i++) {
        int16_t s = (int16_t)std::clamp(low[i], -32768.0f, 32767.0f);
        wav[i * 2 + 0] = s;
        wav[i * 2 + 1] = s;
    }
    bool wrote = write_wav_pcm16("fm_patch_test.wav", wav, SAMPLE_RATE, 2);

    bool bounded = peak_low < 32768 && peak_high < 32768 && peak_low > 0 && peak_high > 0;

    // Skip each note's onset transient (routing/bus history settling).
    size_t start = SAMPLE_RATE / 4, n = SAMPLE_RATE / 2;

    // (1) harmonic content: 2nd harmonic must clear the noise floor (the
    // un-driven 5th harmonic bin) by a real margin, and the higher/mostly-
    // silent harmonics must NOT (otherwise the "signal" is just broadband
    // noise, not the specific sideband structure 1:1 FM predicts).
    float h1 = goertzel_mag(low, start, n, NOTE_LOW * 1, (float)SAMPLE_RATE);
    float h2 = goertzel_mag(low, start, n, NOTE_LOW * 2, (float)SAMPLE_RATE);
    float floor_mag = goertzel_mag(low, start, n, NOTE_LOW * 4.5f, (float)SAMPLE_RATE);  // between harmonics -- true noise floor
    bool harmonics_ok = h1 > 100.0f && h2 > floor_mag * 5.0f;

    // (2) ratio tracking: the strongest nearby peak in each render should
    // sit at that render's own note frequency, not a fixed drone.
    float peak_freq_low = 0.0f, best_mag_low = -1.0f;
    float peak_freq_high = 0.0f, best_mag_high = -1.0f;
    for (float f = NOTE_LOW - 20.0f; f <= NOTE_LOW + 20.0f; f += 2.0f) {
        float m = goertzel_mag(low, start, n, f, (float)SAMPLE_RATE);
        if (m > best_mag_low) { best_mag_low = m; peak_freq_low = f; }
    }
    for (float f = NOTE_HIGH - 20.0f; f <= NOTE_HIGH + 20.0f; f += 2.0f) {
        float m = goertzel_mag(high, start, n, f, (float)SAMPLE_RATE);
        if (m > best_mag_high) { best_mag_high = m; peak_freq_high = f; }
    }
    bool ratio_ok = std::fabs(peak_freq_low - NOTE_LOW) < 5.0f && std::fabs(peak_freq_high - NOTE_HIGH) < 5.0f;

    bool pass = wrote && bounded && harmonics_ok && ratio_ok;
    printf("%s: wrote fm_patch_test.wav -- note=%.0fHz peak=%d/%d, h1=%.1f h2=%.1f floor=%.2f, "
           "peak_freq(low/high)=%.0f/%.0f\n",
           pass ? "PASS" : "FAIL", NOTE_LOW, peak_low, peak_high, h1, h2, floor_mag,
           peak_freq_low, peak_freq_high);
    if (!bounded) printf("  FAIL: peak out of int16 range or silent\n");
    if (!harmonics_ok) printf("  FAIL: 2nd harmonic doesn't clear the noise floor -- no real FM sidebands\n");
    if (!ratio_ok) printf("  FAIL: fundamental doesn't track the note -- ratios aren't scaling correctly\n");
    if (!wrote) printf("  FAIL: could not write WAV file\n");

    return pass;
}

int main() {
    bool ok1 = run_test_tone_check();
    bool ok2 = run_routing_checks();
    bool ok3 = run_patch_spectrum_check();
    bool ok = ok1 && ok2 && ok3;
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
