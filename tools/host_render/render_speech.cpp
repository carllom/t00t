// Host-side proof of src/engines/speech/render.h. Two independent checks,
// both calling the exact functions the device's audio_engine.cpp calls from
// its Core 1 render loop -- there is no openmpt123 equivalent for a formant
// synth (speech.md "Testing"), so this harness *is* the reference from here
// on: same source, host compiler, WAV out, diffable against a device
// capture to separate DSP bugs from embedded bugs. No pico-sdk, no ARM
// intrinsics -- render.h only touches header-only common-layer DSP (see its
// own header comment for why that matters).
//
//   - run_test_tone_check() (#27): the ZOH x2 native-rate resample seam,
//     proven before any formant DSP existed.
//   - run_vowel_checks() (#28): renders the five cardinal vowels
//     (phonemes.h) through the formant cascade, checks the cascade puts a
//     real spectral peak where each phoneme's F1/F2 target says it should,
//     and writes one WAV per vowel for listening.
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_speech
#include "../../src/engines/speech/render.h"
#include "../../src/osc/common.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// Mirrors src/engines/speech/engine.h's SPEECH_RATE (SAMPLE_RATE/2) without
// pulling engine.h/engine_base.h in here — see render.h's comment on why.
static constexpr uint32_t SPEECH_RATE = SAMPLE_RATE / 2;
static constexpr uint32_t NATIVE_BUFFER = 128;  // matches the device's default SAMPLES_PER_BUFFER(256)/2

static bool run_test_tone_check() {
    static constexpr float TEST_TONE_HZ = 220.0f;

    const uint32_t inc = (uint32_t)((TEST_TONE_HZ / (float)SPEECH_RATE)
        * (float)WAVETABLE_SIZE * (float)(1 << PHASE_FRAC_BITS));
    uint32_t phase = 0;

    const uint32_t total_native_frames = SPEECH_RATE * 2;      // 2 seconds of native audio
    const uint32_t total_output_frames = total_native_frames * 2;  // ZOH x2
    std::vector<int16_t> out(total_output_frames * 2);         // interleaved stereo

    std::vector<int32_t> dry_l(NATIVE_BUFFER * 2), dry_r(NATIVE_BUFFER * 2);

    uint32_t done_native = 0, done_out = 0;
    int32_t peak = 0;
    while (done_native < total_native_frames) {
        uint32_t n = std::min(NATIVE_BUFFER, total_native_frames - done_native);
        speech_render_test_tone(phase, inc, /*pan=*/0, dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n * 2; i++) {
            int16_t l = (int16_t)dry_l[i];
            int16_t r = (int16_t)dry_r[i];
            out[(done_out + i) * 2 + 0] = l;
            out[(done_out + i) * 2 + 1] = r;
            peak = std::max({ peak, (int32_t)(l < 0 ? -l : l), (int32_t)(r < 0 ? -r : r) });
        }
        done_native += n;
        done_out += n * 2;
    }

    bool ok = write_wav_pcm16("speech_test_tone.wav", out, SAMPLE_RATE, 2);
    // Frame-count arithmetic sanity: ZOH x2 means every consecutive pair of
    // output frames is exactly identical -- this is the property every later
    // formant slice sits on (issue #27's acceptance criterion).
    bool zoh_ok = true;
    for (uint32_t i = 0; i + 1 < total_output_frames && zoh_ok; i += 2) {
        if (out[i * 2] != out[(i + 1) * 2] || out[i * 2 + 1] != out[(i + 1) * 2 + 1]) zoh_ok = false;
    }

    printf("%s: wrote speech_test_tone.wav -- %u output frames (%u native @ %u Hz, ZOH x2 -> %u Hz), peak=%d\n",
           ok && zoh_ok ? "PASS" : "FAIL", total_output_frames, total_native_frames, SPEECH_RATE, SAMPLE_RATE, peak);
    if (!zoh_ok) printf("  FAIL: a ZOH x2 pair diverged -- frame-count arithmetic is broken\n");
    if (!ok) printf("  FAIL: could not write WAV file\n");

    return ok && zoh_ok;
}

// Goertzel magnitude of a Hann-windowed segment at `freq` Hz.
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

// Hill-climbs from `center` to the local magnitude peak within +-150 Hz, in
// 5 Hz steps. Used instead of a blind global spectrum search because a
// cascade of five formants can have multiple comparably-sized peaks; what
// #28's acceptance criterion actually wants to know is "is there a real
// resonance where phonemes.h says F1/F2 should be," not "find the two
// biggest peaks anywhere" (which is ambiguous for /u/'s F1/F2 sitting close
// together).
static float local_peak_freq(const std::vector<float> &x, size_t start, size_t n, float center, float fs) {
    float best_f = center, best_mag = -1.0f;
    for (float f = center - 150.0f; f <= center + 150.0f; f += 5.0f) {
        if (f < 20.0f) continue;
        float m = goertzel_mag(x, start, n, f, fs);
        if (m > best_mag) { best_mag = m; best_f = f; }
    }
    return best_f;
}

struct VowelName { Vowel v; const char *label; };
static const VowelName VOWEL_NAMES[VOWEL_COUNT] = {
    { VOWEL_I, "i" }, { VOWEL_E, "e" }, { VOWEL_A, "a" }, { VOWEL_O, "o" }, { VOWEL_U, "u" },
};

// Renders one vowel, held under gate, for `seconds` at `note_hz`. Returns
// the rendered native-rate mono samples (post-tract, pre-pan/ZOH) as float
// so the Goertzel analysis isn't limited to int16 quantization.
static std::vector<float> render_vowel_native(Vowel v, float note_hz, float seconds) {
    SpeechVoice sv{};
    const uint32_t total_native = (uint32_t)(SPEECH_RATE * seconds);
    std::vector<float> mono(total_native);

    std::vector<int32_t> dry_l(NATIVE_BUFFER * 2), dry_r(NATIVE_BUFFER * 2);
    uint32_t phase_inc = glottal_phase_inc(note_hz, (float)SPEECH_RATE);

    uint32_t done = 0;
    while (done < total_native) {
        uint32_t n = std::min(NATIVE_BUFFER, total_native - done);
        std::fill(dry_l.begin(), dry_l.begin() + n * 2, 0);
        std::fill(dry_r.begin(), dry_r.begin() + n * 2, 0);
        // trigger=1 (!= SpeechVoice's initial last_trigger=0xFF) on every
        // call so the very first call retriggers; held constant afterwards
        // so later calls glide/hold rather than re-triggering each buffer.
        speech_render_voice(sv, phase_inc, (float)SPEECH_RATE, /*trigger=*/1,
                             /*amplitude=*/32767, /*gate=*/true, (uint8_t)v, /*pan=*/0,
                             dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n; i++) mono[done + i] = (float)dry_l[i * 2];  // pan=0 -> L==R==mono
        done += n;
    }
    return mono;
}

// Impulse response of just the formant cascade (tract.h directly, no
// glottal excitation, no amplitude smoothing) for spectral measurement.
// The audible speech_vowel_*.wav files are glottal-driven (render_vowel_
// native() above) because that's what the device actually plays, but their
// spectrum only has energy at F0's harmonics -- at F0=100 Hz that's too
// coarse to resolve two formants 40-100 Hz apart (e.g. /e/ and /o/'s F1).
// An impulse response has a continuous spectrum, so Goertzel can be
// evaluated at *any* frequency and directly reports the cascade's real
// transfer function, decoupled from excitation harmonic spacing.
static std::vector<float> render_vowel_impulse_response(Vowel v, uint32_t n_samples) {
    SpeechVoice sv{};
    tract_retrigger(sv, VOWEL_TARGETS[v]);
    std::vector<float> y(n_samples);
    uint32_t done = 0;
    while (done < n_samples) {
        uint32_t k = std::min((uint32_t)SPEECH_SUBBLOCK, n_samples - done);
        tract_advance_subblock(sv, (float)SPEECH_RATE);
        for (uint32_t i = 0; i < k; i++) {
            y[done + i] = tract_process(sv, (done + i == 0) ? 1.0f : 0.0f);
        }
        done += k;
    }
    return y;
}

// Writes a vowel's native-rate render, ZOH x2'd to output rate, as a stereo
// WAV -- exactly the shape audio_engine.cpp produces on device.
static void write_vowel_wav(const char *label, const std::vector<float> &mono) {
    std::vector<int16_t> out(mono.size() * 2 * 2);  // ZOH x2, interleaved stereo
    for (size_t i = 0; i < mono.size(); i++) {
        int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, mono[i]));
        out[i * 4 + 0] = s; out[i * 4 + 1] = s;  // frame 2*i
        out[i * 4 + 2] = s; out[i * 4 + 3] = s;  // frame 2*i+1
    }
    char path[64];
    snprintf(path, sizeof(path), "speech_vowel_%s.wav", label);
    write_wav_pcm16(path, out, SAMPLE_RATE, 2);
}

// #28 acceptance: "F1/F2 of each sustained vowel, measured from a
// host-rendered WAV, land inside the published vowel-space region for that
// vowel." Also writes the glottal-driven, gate-held audible WAV per vowel
// (the actual acceptance-criterion listening test: "recognisable as those
// vowels ... played from a MIDI keyboard, held under gate") and checks it
// for clipping/non-finite output.
static bool run_vowel_checks() {
    static constexpr float AUDIBLE_NOTE_HZ = 220.0f;  // A3, mid-keyboard
    static constexpr float AUDIBLE_SECONDS = 1.0f;
    static constexpr float TOLERANCE = 0.10f;  // 10% -- impulse response, no harmonic quantization

    bool all_ok = true;
    printf("\n== Formant cascade: five cardinal vowels ==\n");
    printf("%-4s %10s %10s %10s %10s %8s\n", "vwl", "F1 tgt", "F1 meas", "F2 tgt", "F2 meas", "peak");

    for (auto &vn : VOWEL_NAMES) {
        std::vector<float> audible = render_vowel_native(vn.v, AUDIBLE_NOTE_HZ, AUDIBLE_SECONDS);
        write_vowel_wav(vn.label, audible);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        std::vector<float> imp = render_vowel_impulse_response(vn.v, 4096);
        const FormantTarget &tgt = VOWEL_TARGETS[vn.v];
        float f1 = local_peak_freq(imp, 0, imp.size(), tgt.F[0], (float)SPEECH_RATE);
        float f2 = local_peak_freq(imp, 0, imp.size(), tgt.F[1], (float)SPEECH_RATE);
        float f1_err = std::fabs(f1 - tgt.F[0]) / tgt.F[0];
        float f2_err = std::fabs(f2 - tgt.F[1]) / tgt.F[1];
        bool ok = (f1_err < TOLERANCE) && (f2_err < TOLERANCE) && !clipped && finite;

        printf("%-4s %10.0f %10.0f %10.0f %10.0f %8.0f  %s\n",
               vn.label, tgt.F[0], f1, tgt.F[1], f2, peak, ok ? "PASS" : "FAIL");
        if (clipped) printf("  FAIL: clipped (peak=%.0f) -- lower SPEECH_EXCITATION_HEADROOM\n", peak);
        if (!finite) printf("  FAIL: non-finite sample in audible render\n");
        all_ok = all_ok && ok;
    }

    // Pitch tracking (#28: "glottal pitch tracks note number across at
    // least three octaves without the cascade going unstable or dull").
    // res2p_set()'s own assert(a2 < 1.0f) is the stability backstop (fires
    // and aborts a debug build on a bad pole); this just confirms the
    // output stays finite and non-silent at each octave.
    printf("\n== Pitch tracking across octaves (vowel /a/) ==\n");
    float notes[] = { 110.0f, 220.0f, 440.0f, 880.0f };  // A2..A5, 3 octaves
    for (float hz : notes) {
        std::vector<float> mono = render_vowel_native(VOWEL_A, hz, 0.3f);
        float peak = 0.0f, sum_sq = 0.0f;
        bool finite = true;
        size_t start = mono.size() / 2;
        for (size_t i = start; i < mono.size(); i++) {
            finite = finite && std::isfinite(mono[i]);
            peak = std::max(peak, std::fabs(mono[i]));
            sum_sq += mono[i] * mono[i];
        }
        float rms = sqrtf(sum_sq / (float)(mono.size() - start));
        bool ok = finite && rms > 10.0f && peak < 32767.0f;
        printf("  %6.0f Hz: peak=%7.0f rms=%7.1f -> %s\n", hz, peak, rms, ok ? "PASS" : "FAIL");
        all_ok = all_ok && ok;
    }

    return all_ok;
}

int main() {
    res2p_init();       // must run before any res2p_radius()/res2p_set() call
    osc_init_sine();     // speech_render_test_tone()/pan_gains_q15()'s wavetable source

    bool ok = run_test_tone_check();
    ok = run_vowel_checks() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
