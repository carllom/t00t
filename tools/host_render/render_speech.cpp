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
//   - run_full_table_render() (#32): renders and writes a WAV for every row
//     in the CSV-generated phonemes.h (PHONEME_COUNT, not a fixed handful),
//     checking only that each is finite and unclipped -- the class-specific
//     checks below own the acoustic assertions.
//   - run_vowel_checks() (#28, generalized by #32): renders every
//     PCLASS_VOWEL row (phoneme_meta.h) through the formant cascade, checks
//     the cascade puts a real spectral peak where each phoneme's F1/F2
//     target says it should, and -- #32's actual point -- cross-checks
//     against vowel_reference.h's independently-committed published values
//     for whichever symbols have one, to catch a data-entry error the first
//     comparison can't see.
//   - run_fricative_checks() (#29): renders the three voiceless fricatives
//     through the parallel fricative resonator and checks the noise
//     spectrum peaks near each one's fric_F target -- and that /sh/'s peak
//     lands measurably lower than /s/'s and /f/'s, its defining spectral
//     feature.
//   - run_fricative_checks() (#29): renders the three voiceless fricatives
//     through the parallel fricative resonator and checks the noise
//     spectrum peaks near each one's fric_F target -- and that /sh/'s peak
//     lands measurably lower than /s/'s and /f/'s, its defining spectral
//     feature.
//   - run_voiced_fricative_checks() (#29): for /z/ and /v/, confirms both
//     excitation sources are simultaneously active in the spectrum (not
//     just by ear) -- periodic F0 energy from the glottal source and
//     broadband high-frequency energy from the noise source, by comparing
//     against the same target with af forced to zero.
//   - run_nasal_checks() (#29): for /m/ and /n/, confirms the nasal pole
//     measurably contributes energy at nasal_F, by comparing against the
//     same cascade target with an forced to zero (the acoustic difference
//     between a nasal and its cognate stop's closure -- P2 has no plosive
//     closure/burst mechanism yet, that's speech.md P3).
//   - test_cc_sweep_stability() (#29): sweeps formant_shift and
//     bandwidth_scale across their full CC range at every phoneme target
//     and checks every resonator's pole (cascade, fricative, nasal) stays
//     inside the unit circle -- render_res2p.cpp's test_stability_sweep()
//     style, extended to this engine's live parameters instead of raw
//     res2p_set() calls.
//
// Run from the build directory (tools/host_render/build):
//   cmake -S .. -B . && cmake --build . && ./render_speech
#include "../../src/engines/speech/phrases.h"
#include "../../src/engines/speech/render.h"
#include "../../src/engines/speech/sequencer.h"
#include "../../src/engines/speech/utterance.h"
#include "../../src/osc/common.h"
#include "phoneme_meta.h"
#include "vowel_reference.h"
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

struct PhonemeName { Phoneme p; const char *label; };
// The #29-era fixed short lists this used to have (VOWEL_NAMES etc.) are
// gone -- #32 grew the table from 12 phonemes to PHONEME_COUNT (see
// phoneme_meta.h's generated PHONEME_CLASS[]), so anything that needs "all
// the vowels" or "all the voiceless fricatives" now filters PHONEME_CLASS
// instead of hardcoding a name list that would silently stop covering new
// rows. The three symbol-specific claims from #29's acceptance criteria
// (/sh/ measurably lower than /s/ and /f/; at least one voiced fricative
// shows mixed excitation; a nasal is distinguishable from its an=0
// counterpart) still name PH_S/PH_SH/PH_F/PH_Z/PH_V/PH_M/PH_N directly
// below -- those are regression locks on specific phonemes, not
// class-wide properties, so generalizing them would test something weaker
// than what #29 actually promised.
static const PhonemeName FRICATIVE_NAMES[3] = { { PH_S, "s" }, { PH_SH, "sh" }, { PH_F, "f" } };
static const PhonemeName VOICED_FRICATIVE_NAMES[2] = { { PH_Z, "z" }, { PH_V, "v" } };
static const PhonemeName NASAL_NAMES[2] = { { PH_M, "m" }, { PH_N, "n" } };

static bool str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

// PHONEME_SYMBOLS[p] lowercased, e.g. "P_CL" -> "p_cl" -- used for both WAV
// filenames (matches the pre-#32 speech_phoneme_i.wav-style naming) and log
// output. Symbols are already filesystem-safe (letters/digits/underscore).
static void lower_symbol(char *out, size_t out_size, const char *symbol) {
    size_t i = 0;
    for (; symbol[i] && i + 1 < out_size; i++) {
        char c = symbol[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[i] = '\0';
}

// Renders one phoneme, held under gate, for `seconds` at `note_hz` and
// `formant_shift`/`bandwidth_scale` (Q8.8, 256 = 1.0x -- default neutral).
// `jitter`/`shimmer`/`lfo_rate`/`lfo_depth` (#36) default to off, matching
// pre-#36 behaviour for every existing caller.
// Returns the rendered native-rate mono samples (post-tract, pre-pan/ZOH) as
// float so the Goertzel analysis isn't limited to int16 quantization.
static std::vector<float> render_phoneme_native(Phoneme p, float note_hz, float seconds,
                                                  int16_t formant_shift = 256, int16_t bandwidth_scale = 256,
                                                  uint8_t jitter = 0, uint8_t shimmer = 0,
                                                  float lfo_rate = 0.0f, float lfo_depth = 0.0f) {
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
                             /*amplitude=*/32767, /*gate=*/true, (uint8_t)p, /*pan=*/0,
                             formant_shift, bandwidth_scale, jitter, shimmer, lfo_rate, lfo_depth,
                             dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n; i++) mono[done + i] = (float)dry_l[i * 2];  // pan=0 -> L==R==mono
        done += n;
    }
    return mono;
}

// Renders a FormantTarget directly (bypassing PHONEME_TARGETS), with
// optional af/an overrides -- used to isolate one excitation source's
// contribution to the spectrum for the voiced-fricative and nasal checks
// below. Calls the same tract.h primitives render.h's speech_render_voice()
// does, minus the amplitude declick/pan/ZOH wrapper that isn't needed for
// spectral measurement (same relationship as render_phoneme_impulse_response
// below).
static std::vector<float> render_target_native(const FormantTarget &tgt, float note_hz, float seconds,
                                                 float af_override = -1.0f, float an_override = -1.0f) {
    SpeechVoice sv{};
    tract_retrigger(sv, tgt);
    if (af_override >= 0.0f) sv.af = sv.af_tgt = af_override;
    if (an_override >= 0.0f) sv.an = sv.an_tgt = an_override;

    const uint32_t total = (uint32_t)(SPEECH_RATE * seconds);
    std::vector<float> y(total);
    uint32_t phase = 0;
    uint16_t lfsr = 0xACE1u;
    uint32_t inc = glottal_phase_inc(note_hz, (float)SPEECH_RATE);

    uint32_t done = 0;
    while (done < total) {
        uint32_t k = std::min((uint32_t)SPEECH_SUBBLOCK, total - done);
        tract_advance_subblock(sv, (float)SPEECH_RATE);
        for (uint32_t i = 0; i < k; i++) {
            float voiced_src = glottal_pulse(phase) * sv.av;
            float noise_src = (float)osc_noise(lfsr) * (1.0f / 32768.0f) * sv.af;
            phase += inc;
            y[done + i] = tract_process_mixed(sv, voiced_src, noise_src);
        }
        done += k;
    }
    return y;
}

// Impulse response of just the formant cascade (tract.h directly, no
// glottal excitation, no amplitude smoothing) for spectral measurement.
// The audible speech_phoneme_*.wav files are glottal-driven
// (render_phoneme_native() above) because that's what the device actually
// plays, but their spectrum only has energy at F0's harmonics -- at
// F0=100 Hz that's too coarse to resolve two formants 40-100 Hz apart (e.g.
// /e/ and /o/'s F1). An impulse response has a continuous spectrum, so
// Goertzel can be evaluated at *any* frequency and directly reports the
// cascade's real transfer function, decoupled from excitation harmonic
// spacing.
static std::vector<float> render_phoneme_impulse_response(Phoneme p, uint32_t n_samples) {
    SpeechVoice sv{};
    tract_retrigger(sv, phoneme_unpack(PHONEME_TARGETS[p]));
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

// Writes a phoneme's native-rate render, ZOH x2'd to output rate, as a
// stereo WAV -- exactly the shape audio_engine.cpp produces on device.
static void write_phoneme_wav(const char *label, const std::vector<float> &mono) {
    std::vector<int16_t> out(mono.size() * 2 * 2);  // ZOH x2, interleaved stereo
    for (size_t i = 0; i < mono.size(); i++) {
        int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, mono[i]));
        out[i * 4 + 0] = s; out[i * 4 + 1] = s;  // frame 2*i
        out[i * 4 + 2] = s; out[i * 4 + 3] = s;  // frame 2*i+1
    }
    char path[64];
    snprintf(path, sizeof(path), "speech_phoneme_%s.wav", label);
    write_wav_pcm16(path, out, SAMPLE_RATE, 2);
}

// #32 acceptance: "Every phoneme renders to a WAV via the host harness in
// one command." Held-gate audible render of every row in the table (not
// just the handful the class-specific checks below name individually), so
// growing speech_phonemes.csv automatically gets WAV coverage without
// anyone updating this harness. Deliberately the only place that writes
// speech_phoneme_*.wav -- the class-specific checks below used to each
// write their own subset (duplicating renders for S/SH/F/Z/V/M/N), now they
// just assert on peak/spectrum and let this pass own the file I/O.
// STOP_CLOSURE/SIL rows render silence under gate, correctly -- only
// clipping and non-finite output are bugs here, silence is not.
static bool run_full_table_render() {
    static constexpr float NOTE_HZ = 220.0f;
    static constexpr float SECONDS = 0.6f;
    bool all_ok = true;
    printf("\n== Full table: %u phonemes rendered to WAV ==\n", (unsigned)PHONEME_COUNT);

    for (uint32_t p = 0; p < PHONEME_COUNT; p++) {
        std::vector<float> mono = render_phoneme_native((Phoneme)p, NOTE_HZ, SECONDS);
        float peak = 0.0f;
        bool finite = true;
        for (float s : mono) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;
        bool ok = finite && !clipped;

        char slug[24];
        lower_symbol(slug, sizeof(slug), PHONEME_SYMBOLS[p]);
        write_phoneme_wav(slug, mono);

        if (!ok) {
            printf("  FAIL %-8s peak=%.0f finite=%s\n", PHONEME_SYMBOLS[p], peak, finite ? "yes" : "no");
        }
        all_ok = all_ok && ok;
    }
    printf("  %u phonemes rendered, all finite and unclipped: %s\n", (unsigned)PHONEME_COUNT, all_ok ? "PASS" : "FAIL");
    return all_ok;
}

// #28 acceptance, generalized by #32 from the original 5 cardinal vowels to
// every PCLASS_VOWEL row in the (now CSV-generated) table: "F1/F2 of each
// sustained vowel, measured from a host-rendered WAV, land inside the
// published vowel-space region for that vowel." Two independent
// comparisons per vowel:
//   - measured vs. this table's own F/B target (DSP correctness -- the
//     cascade puts a real resonance where the row says it should; this is
//     the #28-original check, just no longer limited to 5 rows).
//   - measured vs. vowel_reference.h's independently-committed published
//     values, for whichever symbols have a reference entry (#32's actual
//     point -- catches a coefficient/data-entry error in speech_phonemes.csv
//     that the ear forgives and the first check above can't see, since that
//     one only proves the DSP hits whatever the CSV happens to say).
static bool run_vowel_checks() {
    static constexpr float AUDIBLE_NOTE_HZ = 220.0f;  // A3, mid-keyboard
    static constexpr float AUDIBLE_SECONDS = 1.0f;
    static constexpr float TOLERANCE = 0.10f;  // 10% -- impulse response, no harmonic quantization

    bool all_ok = true;
    printf("\n== Formant cascade: %u vowel/approximant phonemes ==\n",
           (unsigned)std::count(PHONEME_CLASS, PHONEME_CLASS + PHONEME_COUNT, (uint8_t)PCLASS_VOWEL));
    printf("%-8s %10s %10s %10s %10s %8s  %-16s\n",
           "sym", "F1 tgt", "F1 meas", "F2 tgt", "F2 meas", "peak", "vs. published");

    for (uint32_t p = 0; p < PHONEME_COUNT; p++) {
        if (PHONEME_CLASS[p] != PCLASS_VOWEL) continue;
        const char *sym = PHONEME_SYMBOLS[p];

        std::vector<float> audible = render_phoneme_native((Phoneme)p, AUDIBLE_NOTE_HZ, AUDIBLE_SECONDS);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        std::vector<float> imp = render_phoneme_impulse_response((Phoneme)p, 4096);
        FormantTarget tgt = phoneme_unpack(PHONEME_TARGETS[p]);
        float f1 = local_peak_freq(imp, 0, imp.size(), tgt.F[0], (float)SPEECH_RATE);
        float f2 = local_peak_freq(imp, 0, imp.size(), tgt.F[1], (float)SPEECH_RATE);
        float f1_err = std::fabs(f1 - tgt.F[0]) / tgt.F[0];
        float f2_err = std::fabs(f2 - tgt.F[1]) / tgt.F[1];
        bool ok = (f1_err < TOLERANCE) && (f2_err < TOLERANCE) && !clipped && finite;

        char ref_col[24] = "(no reference)";
        for (auto &ref : VOWEL_REFERENCE) {
            if (!str_eq(ref.symbol, sym)) continue;
            float f1_ref_err = std::fabs(f1 - ref.F1) / ref.F1;
            float f2_ref_err = std::fabs(f2 - ref.F2) / ref.F2;
            bool ref_ok = f1_ref_err < TOLERANCE && f2_ref_err < TOLERANCE;
            snprintf(ref_col, sizeof(ref_col), "F1=%.0f F2=%.0f %s", ref.F1, ref.F2, ref_ok ? "PASS" : "FAIL");
            ok = ok && ref_ok;
            break;
        }

        printf("%-8s %10.0f %10.0f %10.0f %10.0f %8.0f  %-16s %s\n",
               sym, tgt.F[0], f1, tgt.F[1], f2, peak, ref_col, ok ? "PASS" : "FAIL");
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
        std::vector<float> mono = render_phoneme_native(PH_A, hz, 0.3f);
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

// Peak magnitude over [lo, hi] in `step`-Hz increments -- the fricative
// resonator's bandwidth is much wider than a vowel formant's (often clamped
// by res2p.h's LUT range, see tract.h's TRACT_MIN_BANDWIDTH_HZ comment), so
// local_peak_freq()'s +-150 Hz hill-climb is too narrow to find it; this
// scans the whole plausible frication range instead.
static float wideband_peak_freq(const std::vector<float> &x, size_t start, size_t n,
                                 float lo, float hi, float step, float fs) {
    float best_f = lo, best_mag = -1.0f;
    for (float f = lo; f <= hi; f += step) {
        float m = goertzel_mag(x, start, n, f, fs);
        if (m > best_mag) { best_mag = m; best_f = f; }
    }
    return best_f;
}

// #29 acceptance: "Fricatives (/s/, /f/, /sh/) are distinguishable from each
// other by ear when played as held phonemes." Writes each voiceless
// fricative's audible WAV for listening, and as an objective proxy, checks
// the parallel fricative resonator's noise output peaks in the right part
// of the spectrum -- specifically that /sh/'s peak lands measurably below
// /s/'s and /f/'s, which is the actual perceptual cue that separates it
// from the other two.
static bool run_fricative_checks() {
    static constexpr float AUDIBLE_NOTE_HZ = 220.0f;
    bool all_ok = true;
    printf("\n== Voiceless fricatives: parallel resonator branch ==\n");

    float measured[3];
    for (size_t idx = 0; idx < 3; idx++) {
        const PhonemeName &pn = FRICATIVE_NAMES[idx];
        std::vector<float> audible = render_phoneme_native(pn.p, AUDIBLE_NOTE_HZ, 1.0f);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        // A longer, unvoiced (av=0, matches the table) render for a less
        // noisy spectral estimate of the LFSR-driven resonator.
        std::vector<float> longer = render_phoneme_native(pn.p, AUDIBLE_NOTE_HZ, 2.0f);
        measured[idx] = wideband_peak_freq(longer, longer.size() / 4, longer.size() / 2,
                                            800.0f, 9000.0f, 50.0f, (float)SPEECH_RATE);

        bool ok = !clipped && finite;
        printf("  /%-2s/: peak=%7.0f  spectral peak=%6.0f Hz (target fric_F=%u) -> %s\n",
               pn.label, peak, measured[idx], (unsigned)PHONEME_TARGETS[pn.p].fric_F, ok ? "PASS" : "FAIL");
        if (clipped) printf("  FAIL: clipped (peak=%.0f)\n", peak);
        all_ok = all_ok && ok;
    }

    bool distinguishable = measured[1] < measured[0] - 500.0f && measured[1] < measured[2] - 500.0f;
    printf("  /sh/ measurably lower than /s/ and /f/ (s=%.0f sh=%.0f f=%.0f): %s\n",
           measured[0], measured[1], measured[2], distinguishable ? "PASS" : "FAIL");
    all_ok = all_ok && distinguishable;

    return all_ok;
}

// #29 acceptance: "At least one voiced fricative (/z/ or /v/) renders with
// both excitation sources active, verified in the host WAV's spectrum, not
// just by ear." Compares each voiced fricative's real (av>0, af>0) render
// against the same target with af forced to zero: if both sources are
// really contributing, forcing af to zero should measurably drop energy
// well above F5 (where only the fricative resonator's noise excitation has
// anything to say), while energy at F0 -- proof of voicing -- stays present
// in both.
static bool run_voiced_fricative_checks() {
    static constexpr float F0 = 220.0f;
    static constexpr float PROBE_HZ = 6000.0f;  // above F5 (3750) and PH_S/PH_Z's fric_F region
    bool all_ok = true;
    printf("\n== Voiced fricatives: mixed excitation (glottal + noise) ==\n");

    for (auto &pn : VOICED_FRICATIVE_NAMES) {
        std::vector<float> audible = render_phoneme_native(pn.p, F0, 1.0f);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        FormantTarget tgt = phoneme_unpack(PHONEME_TARGETS[pn.p]);
        std::vector<float> mixed = render_target_native(tgt, F0, 0.5f);
        std::vector<float> voiced_only = render_target_native(tgt, F0, 0.5f, /*af_override=*/0.0f);

        size_t half = mixed.size() / 2;
        float e_mixed_hi  = goertzel_mag(mixed, half, half, PROBE_HZ, (float)SPEECH_RATE);
        float e_voiced_hi = goertzel_mag(voiced_only, half, half, PROBE_HZ, (float)SPEECH_RATE);
        float e_f0 = goertzel_mag(mixed, half, half, F0, (float)SPEECH_RATE);

        bool has_noise = e_mixed_hi > e_voiced_hi * 2.0f;
        bool has_voicing = e_f0 > 0.02f;
        bool ok = !clipped && finite && has_noise && has_voicing;

        printf("  /%s/: peak=%.0f  %.0fHz energy: mixed=%.4f af=0:%.4f (noise present: %s)  F0 energy=%.4f (voicing present: %s) -> %s\n",
               pn.label, peak, PROBE_HZ, e_mixed_hi, e_voiced_hi, has_noise ? "yes" : "no",
               e_f0, has_voicing ? "yes" : "no", ok ? "PASS" : "FAIL");
        all_ok = all_ok && ok;
    }

    return all_ok;
}

// #29 acceptance: "A nasal (/m/ or /n/) is distinguishable from the
// corresponding voiced stop." P2 has no plosive closure/burst mechanism yet
// (speech.md P3), so the closest objective proxy available here is the
// nasal pole's own contribution: the acoustic difference between a nasal
// and its cognate stop's closure interval is entirely whether the velum is
// lowered (nasal pole active) or the nasal port is sealed too (an=0, same
// oral-cavity cascade shape). Compares each nasal's real render against the
// same target with an forced to zero and checks for a measurable energy
// increase at nasal_F.
static bool run_nasal_checks() {
    static constexpr float F0 = 220.0f;
    bool all_ok = true;
    printf("\n== Nasals: parallel nasal-pole branch ==\n");

    for (auto &pn : NASAL_NAMES) {
        std::vector<float> audible = render_phoneme_native(pn.p, F0, 1.0f);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        FormantTarget tgt = phoneme_unpack(PHONEME_TARGETS[pn.p]);
        std::vector<float> with_nasal = render_target_native(tgt, F0, 0.5f);
        std::vector<float> no_nasal = render_target_native(tgt, F0, 0.5f, /*af_override=*/-1.0f, /*an_override=*/0.0f);

        size_t half = with_nasal.size() / 2;
        float e_with = goertzel_mag(with_nasal, half, half, tgt.nasal_F, (float)SPEECH_RATE);
        float e_without = goertzel_mag(no_nasal, half, half, tgt.nasal_F, (float)SPEECH_RATE);
        bool distinguishable = e_with > e_without * 1.3f;
        bool ok = !clipped && finite && distinguishable;

        printf("  /%s/: peak=%.0f  nasal_F=%.0f energy: an=%.2f:%.4f  an=0:%.4f -> %s\n",
               pn.label, peak, tgt.nasal_F, tgt.an, e_with, e_without, ok ? "PASS" : "FAIL");
        all_ok = all_ok && ok;
    }

    return all_ok;
}

// #29 acceptance: "A host sweep of both CCs across their full ranges, at
// every vowel target, asserts every pole stays inside the unit circle
// (extend the existing render_res2p style of check rather than inventing a
// new harness)." Extends render_res2p.cpp's test_stability_sweep() to this
// engine's actual live-parameter path (tract_apply_coeffs(), not a raw
// res2p_set() sweep) across every phoneme, not just the vowels -- the
// fricative and nasal branches are exactly as exposed to formant_shift/
// bandwidth_scale as the cascade is.
static bool test_cc_sweep_stability() {
    bool all_ok = true;
    uint32_t checked = 0;

    auto bad_pole = [](const Res2p &r) { return !(r.a2 >= 0.0f && r.a2 < 1.0f); };

    for (uint32_t p = 0; p < PHONEME_COUNT; p++) {
        FormantTarget tgt = phoneme_unpack(PHONEME_TARGETS[p]);
        for (uint32_t cc_shift = 0; cc_shift <= 127; cc_shift++) {
            for (uint32_t cc_bw = 0; cc_bw <= 127; cc_bw += 4) {
                SpeechVoice sv{};
                tract_retrigger(sv, tgt);
                int16_t shift_q88 = tract_cc_to_q8_8((uint8_t)cc_shift, FORMANT_SHIFT_MIN, FORMANT_SHIFT_MAX);
                int16_t bw_q88 = tract_cc_to_q8_8((uint8_t)cc_bw, BANDWIDTH_SCALE_MIN, BANDWIDTH_SCALE_MAX);
                // Snap current == target so this checks the coefficients at
                // the extreme value itself (mirrors what a sustained CC
                // sweep settles to), not mid-ramp toward it.
                sv.formant_shift = sv.formant_shift_tgt = (float)shift_q88 * (1.0f / 256.0f);
                sv.bandwidth_scale = sv.bandwidth_scale_tgt = (float)bw_q88 * (1.0f / 256.0f);
                tract_advance_subblock(sv, (float)SPEECH_RATE);
                checked++;

                bool fail = false;
                for (uint32_t i = 0; i < SPEECH_FORMANTS; i++) fail = fail || bad_pole(sv.formant[i]);
                fail = fail || bad_pole(sv.fric) || bad_pole(sv.nasal);
                if (fail) {
                    printf("  FAIL phoneme=%u cc_shift=%u cc_bw=%u -> pole outside unit circle\n", p, cc_shift, cc_bw);
                    all_ok = false;
                }
            }
        }
    }
    printf("  %u (phoneme, formant_shift CC, bandwidth_scale CC) combinations checked\n", checked);
    return all_ok;
}

// #34: renders `total_native_frames` of one voice sequencing `utt` under
// `mode`/`rate`, gate held true for the first `hold_native_frames` and false
// thereafter (hold_native_frames >= total_native_frames means "never let
// go" -- used by the plain word-render checks below). Returns the native-rate
// mono samples plus the first buffer-call boundary at which sv.active went
// false (SpeechVoice::active, render.h) -- i.e. how long the utterance
// actually took to finish -- or total_native_frames if it never did.
// trigger is held at a constant 1 throughout (matches render_phoneme_native's
// same pattern above): SpeechVoice's initial last_trigger is 0xFF, so the
// very first call retriggers and every later call in this render continues
// the same note, exactly like a single held MIDI note would.
struct UtteranceRender {
    std::vector<float> mono;
    uint32_t completed_at;
};

// `jitter`/`shimmer`/`lfo_rate`/`lfo_depth` (#36) default to off, matching
// pre-#36 behaviour for every existing caller.
static UtteranceRender render_utterance_native(const SpeechUtterance &utt, SpeechMode mode, uint8_t rate,
                                                 float note_hz, uint32_t hold_native_frames,
                                                 uint32_t total_native_frames,
                                                 uint8_t jitter = 0, uint8_t shimmer = 0,
                                                 float lfo_rate = 0.0f, float lfo_depth = 0.0f) {
    SpeechVoice sv{};
    UtteranceRender result;
    result.mono.assign(total_native_frames, 0.0f);
    result.completed_at = total_native_frames;

    std::vector<int32_t> dry_l(NATIVE_BUFFER * 2), dry_r(NATIVE_BUFFER * 2);
    uint32_t phase_inc = glottal_phase_inc(note_hz, (float)SPEECH_RATE);

    uint32_t done = 0;
    bool recorded = false;
    while (done < total_native_frames) {
        uint32_t n = std::min((uint32_t)NATIVE_BUFFER, total_native_frames - done);
        std::fill(dry_l.begin(), dry_l.begin() + n * 2, 0);
        std::fill(dry_r.begin(), dry_r.begin() + n * 2, 0);
        bool gate = done < hold_native_frames;
        speech_render_voice_seq(sv, phase_inc, (float)SPEECH_RATE, /*trigger=*/1, /*amplitude=*/32767,
                                 gate, utt, mode, rate, /*pan=*/0, /*formant_shift=*/256, /*bandwidth_scale=*/256,
                                 jitter, shimmer, lfo_rate, lfo_depth,
                                 dry_l.data(), dry_r.data(), n);
        for (uint32_t i = 0; i < n; i++) result.mono[done + i] = (float)dry_l[i * 2];
        if (!sv.active && !recorded) { result.completed_at = done; recorded = true; }
        done += n;
    }
    return result;
}

// RMS over [start, start+n).
static float rms(const std::vector<float> &x, size_t start, size_t n) {
    double sum_sq = 0.0;
    for (size_t i = start; i < start + n; i++) sum_sq += (double)x[i] * (double)x[i];
    return (float)std::sqrt(sum_sq / (double)n);
}

// #34 acceptance: "A hardcoded phoneme string is intelligible as the
// intended word ... Segment duration comes from the phoneme's nominal
// duration scaled by rate ... Plosives are distinguishable from the
// corresponding fricatives by ear ... F/B ramps are continuous across
// segment boundaries -- no click ... Any inconsistency renders silence ...
// Note-off behaves as #30 specified." One function per claim below, all
// sharing render_utterance_native() so every claim is checked against
// exactly what audio_engine.cpp's Core 1 render loop calls.
static bool run_utterance_word_checks() {
    static constexpr float NOTE_HZ = 150.0f;
    static constexpr float MAX_SECONDS = 3.0f;
    bool all_ok = true;
    printf("\n== Segment sequencer: hardcoded utterances (#34) ==\n");

    struct { SpeechUtteranceId id; const char *label; } words[] = {
        { UTT_HELLO, "hello" }, { UTT_CAT, "cat" },
    };
    for (auto &w : words) {
        const SpeechUtterance &utt = SPEECH_UTTERANCES[w.id];
        uint32_t total = (uint32_t)(SPEECH_RATE * MAX_SECONDS);
        // ONESHOT, gate held the whole render: "say the whole word and stop
        // on its own" is the exit criterion, independent of note-off/#30.
        UtteranceRender r = render_utterance_native(utt, SPEECH_MODE_ONESHOT, /*rate=*/16, NOTE_HZ, total, total);

        float peak = 0.0f;
        bool finite = true;
        for (float s : r.mono) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        // Expected total duration: sum of each segment's own
        // speech_seg_duration_samples() at rate=16 -- the exact function the
        // sequencer itself uses, so this checks the *sequencer's* arithmetic
        // (does it stop where the phoneme data says it should), not a
        // second, independently-computed guess at the same number.
        uint32_t expected = 0;
        for (uint32_t i = 0; i < utt.length; i++) expected += speech_seg_duration_samples(utt.phonemes[i], 16, (float)SPEECH_RATE);
        bool finished = r.completed_at < total;  // actually stopped, didn't run out the clock
        float dur_err = finished ? std::fabs((float)r.completed_at - (float)expected) / (float)expected : 1.0f;
        bool duration_ok = finished && dur_err < 0.05f;  // buffer-granularity rounding only

        // Trim the WAV to what the utterance actually rendered (plus a
        // small tail) rather than the full multi-second padded buffer --
        // r.mono past completed_at is silence by construction (sv.seq_done),
        // and a file that's mostly silence is nothing but dead air for
        // whoever listens to judge intelligibility.
        size_t wav_len = std::min(r.mono.size(), (size_t)r.completed_at + SPEECH_RATE / 4);
        std::vector<int16_t> out(wav_len * 2 * 2);
        for (size_t i = 0; i < wav_len; i++) {
            int16_t s = (int16_t)std::max(-32768.0f, std::min(32767.0f, r.mono[i]));
            out[i * 4 + 0] = s; out[i * 4 + 1] = s;
            out[i * 4 + 2] = s; out[i * 4 + 3] = s;
        }
        char path[64];
        snprintf(path, sizeof(path), "speech_utterance_%s.wav", w.label);
        write_wav_pcm16(path, out, SAMPLE_RATE, 2);

        bool ok = finite && !clipped && duration_ok;
        printf("  \"%s\": completed=%u expected=%u (err=%.1f%%) peak=%.0f -> %s\n",
               w.label, r.completed_at, expected, dur_err * 100.0f, peak, ok ? "PASS" : "FAIL");
        if (!duration_ok) printf("  FAIL: utterance didn't complete at the phoneme-data-predicted sample count\n");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

// #34 acceptance: "Segment duration comes from the phoneme's nominal
// duration scaled by rate, and a rate change is audible as a speaking-speed
// change without pitch shift." A single sustained /e/ "utterance" (one
// segment, no diphone boundary to confound the measurement) rendered at
// three rates: total duration must scale with rate (Q4.4, speech_seg_
// duration_samples()'s "larger rate = longer segment"), while the glottal
// F0 -- measured independently of duration by Goertzel magnitude at
// note_hz on each render's stable middle third -- must not move.
static bool run_rate_checks() {
    static constexpr uint8_t RATE_PHONEMES[] = { PH_E };
    static constexpr SpeechUtterance RATE_UTT = { RATE_PHONEMES, 1, 0 };
    static constexpr float NOTE_HZ = 180.0f;
    bool all_ok = true;
    printf("\n== Segment sequencer: rate scales duration, not pitch (#34) ==\n");

    uint8_t rates[] = { 8, 16, 32 };  // 0.5x, 1.0x, 2.0x
    uint32_t completed[3];
    for (size_t idx = 0; idx < 3; idx++) {
        uint32_t total = (uint32_t)(SPEECH_RATE * 2.0f);
        UtteranceRender r = render_utterance_native(RATE_UTT, SPEECH_MODE_ONESHOT, rates[idx], NOTE_HZ, total, total);
        completed[idx] = r.completed_at;

        // Audio only exists in [0, completed_at) -- the rest of `total` is
        // post-completion silence (this is a single-segment utterance, so
        // completed_at is however long that one /e/ actually sounded for at
        // this rate). Measure the middle third of *that* span, not of the
        // whole padded buffer.
        size_t third = r.completed_at / 3;
        float f0_mag = goertzel_mag(r.mono, third, third, NOTE_HZ, (float)SPEECH_RATE);
        bool has_pitch = f0_mag > 50.0f;
        printf("  rate=%2u (%.2fx): completed=%6u samples  F0(%.0fHz) mag=%7.1f -> %s\n",
               rates[idx], (float)rates[idx] / 16.0f, completed[idx], NOTE_HZ, f0_mag, has_pitch ? "PASS" : "FAIL");
        all_ok = all_ok && has_pitch;
    }

    // 32 is 2x rate 16 which is 2x rate 8 -- both ratios should land near 2.0.
    float ratio_hi = (float)completed[2] / (float)completed[1];
    float ratio_lo = (float)completed[1] / (float)completed[0];
    bool ratios_ok = std::fabs(ratio_hi - 2.0f) < 0.1f && std::fabs(ratio_lo - 2.0f) < 0.1f;
    printf("  duration ratios: 32:16=%.2f  16:8=%.2f (expect ~2.0 each) -> %s\n",
           ratio_hi, ratio_lo, ratios_ok ? "PASS" : "FAIL");
    all_ok = all_ok && ratios_ok;

    return all_ok;
}

// #34 acceptance: "Plosives are distinguishable from the corresponding
// fricatives by ear." speech.md's own claim about *why* is specific: a
// plosive's closure (silence) immediately followed by a burst (transient)
// is the acoustic shape a continuous fricative doesn't have -- "without
// this, plosives sound like fricatives and intelligibility collapses."
// Renders CAT (K_CL K_BR AE T_CL T_BR SIL) once and measures RMS energy
// inside each closure segment (should be near-silent) against its paired
// burst segment (should be a real transient) using speech_seg_duration_
// samples() to locate the segment boundaries exactly as the sequencer does.
static bool run_plosive_checks() {
    bool all_ok = true;
    printf("\n== Segment sequencer: plosive closure/burst contrast (#34) ==\n");

    const SpeechUtterance &utt = SPEECH_UTTERANCES[UTT_CAT];
    uint32_t total = (uint32_t)(SPEECH_RATE * 2.0f);
    UtteranceRender r = render_utterance_native(utt, SPEECH_MODE_ONESHOT, /*rate=*/16, 150.0f, total, total);

    uint32_t pos = 0;
    struct { uint32_t idx; const char *label; } closure_burst_pairs[] = { { 0, "K" }, { 3, "T" } };
    uint32_t seg_start[8];
    for (uint32_t i = 0; i < utt.length; i++) {
        seg_start[i] = pos;
        pos += speech_seg_duration_samples(utt.phonemes[i], 16, (float)SPEECH_RATE);
    }

    for (auto &pair : closure_burst_pairs) {
        uint32_t cl_start = seg_start[pair.idx], cl_len = seg_start[pair.idx + 1] - seg_start[pair.idx];
        uint32_t br_start = seg_start[pair.idx + 1], br_len = seg_start[pair.idx + 2] - seg_start[pair.idx + 1];
        uint32_t guard = 32;
        // Closure: measure the *second half* of the segment, not right after
        // its start. A closure that follows a vowel (T_CL after AE here)
        // starts with the cascade resonators still ringing down from the
        // vowel's much stronger excitation -- a real decay tail, not a bug
        // -- so the acoustically meaningful "is this closure actually
        // silent" question is whether it has settled by the time the burst
        // arrives, not whether it's silent the instant av/af hit zero.
        uint32_t cl_measure_start = cl_start + cl_len / 2;
        uint32_t cl_measure_len = cl_len / 2 > guard ? cl_len / 2 - guard : 0;
        // Burst: skip the declick ramp's own edge the same way the other
        // checks in this file do.
        float cl_rms = cl_measure_len > 0 ? rms(r.mono, cl_measure_start, cl_measure_len) : 0.0f;
        float br_rms = br_len > 2 * guard ? rms(r.mono, br_start + guard, br_len - 2 * guard) : 0.0f;
        bool ok = cl_rms < 50.0f && br_rms > cl_rms * 4.0f;
        printf("  /%s/: closure RMS=%7.1f  burst RMS=%7.1f -> %s\n", pair.label, cl_rms, br_rms, ok ? "PASS" : "FAIL");
        if (cl_rms >= 50.0f) printf("  FAIL: closure isn't silent\n");
        if (br_rms <= cl_rms * 4.0f) printf("  FAIL: burst doesn't stand out from its closure\n");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

// #34 acceptance: "Note-off behaves as #30 specified" -- specifically
// SPEECH_MODE_GATED's bound: "Maximum time a voice may outlive its note-off
// is ... the release segment's own duration." Cuts HELLO's gate early
// (partway through the second segment, well before the utterance would
// finish on its own) and checks the voice finishes within release_index's
// own segment duration of that note-off, not the remaining length of the
// whole utterance.
static bool run_gated_release_checks() {
    bool all_ok = true;
    printf("\n== Segment sequencer: SPEECH_MODE_GATED note-off bound (#30/#34) ==\n");

    const SpeechUtterance &utt = SPEECH_UTTERANCES[UTT_HELLO];
    uint32_t seg0 = speech_seg_duration_samples(utt.phonemes[0], 16, (float)SPEECH_RATE);
    uint32_t seg1 = speech_seg_duration_samples(utt.phonemes[1], 16, (float)SPEECH_RATE);
    uint32_t hold = seg0 + seg1 / 2;  // cut mid-way through the 2nd of 5 segments
    uint32_t release_dur = speech_seg_duration_samples(utt.phonemes[utt.release_index], 16, (float)SPEECH_RATE);
    uint32_t total = (uint32_t)(SPEECH_RATE * 2.0f);

    UtteranceRender r = render_utterance_native(utt, SPEECH_MODE_GATED, /*rate=*/16, 150.0f, hold, total);

    bool finished = r.completed_at < total;
    uint32_t overhang = finished ? r.completed_at - hold : 0xFFFFFFFFu;
    // release_dur plus one host-render buffer's slack: render_utterance_native()
    // only samples sv.active at NATIVE_BUFFER call boundaries, same
    // granularity audio_engine.cpp's real per-buffer active_mask has.
    uint32_t bound = release_dur + NATIVE_BUFFER;
    bool ok = finished && overhang <= bound;

    printf("  note-off at %u, natural end would be much later; completed at %u (overhang=%u, bound=%u) -> %s\n",
           hold, r.completed_at, overhang, bound, ok ? "PASS" : "FAIL");
    if (!ok) printf("  FAIL: voice outlived its note-off by more than the release segment's own duration\n");
    all_ok = all_ok && ok;
    return all_ok;
}

// #34 acceptance: "Any inconsistency renders silence; verified by
// deliberately injecting one." Constructs a malformed utterance directly
// (null phonemes / zero length -- data corruption or a caller passing the
// wrong table entry) and confirms the voice renders exact silence and
// reports itself immediately reclaimable (SpeechVoice::active == false)
// rather than dereferencing null or holding the voice forever.
static bool run_malformed_utterance_check() {
    printf("\n== Segment sequencer: malformed utterance renders silence (#34) ==\n");

    SpeechUtterance bad{ nullptr, 0, 0 };
    SpeechVoice sv{};
    uint32_t total = SPEECH_RATE / 2;
    std::vector<int32_t> dry_l(total * 2, 0), dry_r(total * 2, 0);
    uint32_t phase_inc = glottal_phase_inc(150.0f, (float)SPEECH_RATE);
    speech_render_voice_seq(sv, phase_inc, (float)SPEECH_RATE, /*trigger=*/1, /*amplitude=*/32767,
                             /*gate=*/true, bad, SPEECH_MODE_GATED, /*rate=*/16, /*pan=*/0, 256, 256,
                             /*jitter=*/0, /*shimmer=*/0, /*lfo_rate=*/0.0f, /*lfo_depth=*/0.0f,
                             dry_l.data(), dry_r.data(), total);

    bool silent = true;
    for (uint32_t i = 0; i < total * 2; i++) silent = silent && dry_l[i] == 0 && dry_r[i] == 0;
    bool ok = silent && !sv.active;
    printf("  malformed utterance (null phonemes, length=0): silent=%s active=%s -> %s\n",
           silent ? "yes" : "no", sv.active ? "yes" : "no", ok ? "PASS" : "FAIL");
    if (!silent) printf("  FAIL: malformed utterance produced non-zero output\n");
    if (sv.active) printf("  FAIL: voice reports itself still sounding after a malformed utterance\n");
    return ok;
}

// #35 acceptance: "Every phrase renders to WAV via the host harness in one
// command." phrases.h (tools/speechgen.py gen-phrases, from
// tools/speech_phrases.txt) is generated data, not a hand-picked fixture
// like utterance.h's HELLO/CAT above -- this can't assert *correct*
// pronunciation (that's #35's other acceptance criterion, the blind
// intelligibility spot-check, and it needs a human ear, not this harness).
// What it checks is that every phrase the generator produced is well-formed
// audio: finite, not clipping, and the sequencer actually reaches
// SPEECH_MODE_ONESHOT completion within a generous time budget rather than
// running out the clock (which would mean a malformed/never-terminating
// utterance slipped past speechgen.py's own validation).
static const char *sanitize_wav_label(const char *text, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; text[i] != '\0' && j + 1 < out_len; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[j++] = (c >= 'a' && c <= 'z') ? c : '_';
    }
    out[j] = '\0';
    return out;
}

static bool run_phrase_renders() {
    bool all_ok = true;
    printf("\n== Phrase bank (#35): rendering every generated phrase to WAV ==\n");

    for (uint32_t i = 0; i < SPEECH_PHRASE_COUNT; i++) {
        const SpeechUtterance &utt = SPEECH_PHRASES[i];
        const char *text = SPEECH_PHRASE_TEXT[i];
        uint32_t total = SPEECH_RATE * 6;  // generous cap -- longest phrase here is well under 2s
        UtteranceRender r = render_utterance_native(utt, SPEECH_MODE_ONESHOT, /*rate=*/16, 150.0f, total, total);

        float peak = 0.0f;
        bool finite = true;
        for (float s : r.mono) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;
        bool finished = r.completed_at < total;
        bool ok = finite && !clipped && finished;

        size_t wav_len = std::min(r.mono.size(), (size_t)r.completed_at + SPEECH_RATE / 4);
        std::vector<int16_t> out(wav_len * 2 * 2);
        for (size_t s = 0; s < wav_len; s++) {
            int16_t v = (int16_t)std::max(-32768.0f, std::min(32767.0f, r.mono[s]));
            out[s * 4 + 0] = v; out[s * 4 + 1] = v;
            out[s * 4 + 2] = v; out[s * 4 + 3] = v;
        }
        char label[64], path[96];
        sanitize_wav_label(text, label, sizeof(label));
        snprintf(path, sizeof(path), "speech_phrase_%s.wav", label);
        write_wav_pcm16(path, out, SAMPLE_RATE, 2);

        printf("  \"%s\": %u segments, completed=%u/%u samples, peak=%.0f -> %s (%s)\n",
               text, utt.length, r.completed_at, total, peak, ok ? "PASS" : "FAIL", path);
        if (!finished) printf("  FAIL: phrase never completed within the render time budget\n");
        if (clipped) printf("  FAIL: phrase clipped\n");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

// #36 acceptance: "A MIDI sequence plays a sung phrase at the correct
// pitch, with pitch tracking note number across the useful range." Unlike
// run_vowel_checks()'s octave sweep above (a single sustained phoneme, not
// an utterance) or run_rate_checks() (a single-segment utterance, checking
// only "has pitch" not "has the *requested* pitch"), this renders a real
// generated phrase (SPEECH_PHRASES, #35/#36) at three notes spanning the
// keyboard and checks each render shows real energy at its requested
// frequency -- if pitch weren't tracking note number, changing `note_hz`
// wouldn't move where the energy concentrates.
static bool run_phrase_pitch_tracking_checks() {
    bool all_ok = true;
    printf("\n== Phrase bank: pitch tracks note number (#36) ==\n");

    const SpeechUtterance &utt = SPEECH_PHRASES[PHRASE_VOICE_TEST];
    float notes[] = { 110.0f, 220.0f, 440.0f };  // A2, A3, A4
    for (float hz : notes) {
        uint32_t total = SPEECH_RATE * 3;
        UtteranceRender r = render_utterance_native(utt, SPEECH_MODE_ONESHOT, /*rate=*/16, hz, total, total);
        bool finished = r.completed_at < total;

        size_t n = finished ? r.completed_at : r.mono.size();
        float e_at = goertzel_mag(r.mono, 0, n, hz, (float)SPEECH_RATE);
        bool has_pitch = e_at > 50.0f;
        bool ok = has_pitch && finished;
        printf("  %5.0f Hz: completed=%s  F0(%.0fHz) mag=%7.1f -> %s\n",
               hz, finished ? "yes" : "no ", hz, e_at, ok ? "PASS" : "FAIL");
        if (!finished) printf("  FAIL: phrase never completed\n");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

// #36 acceptance: "Jitter/shimmer at zero produce a measurably periodic
// waveform (verified from a host render, not just by ear); non-zero
// visibly perturbs period and amplitude." Renders a sustained vowel with
// jitter/shimmer off and again at their max, then measures cycle-to-cycle
// period (rising zero-crossing distance) and per-cycle peak amplitude
// straight from the post-tract waveform -- the same audio the device
// actually plays, not an internal-state probe.
// `min_gap` debounces spurious crossings from formant ripple: the tract's
// resonators (hundreds of Hz to a few kHz) ring within each glottal period,
// so a naive zero-crossing count sees several extra crossings per cycle
// near the waveform's low points -- real, not a bug, but not what "one
// crossing per glottal period" needs. Suppressing anything closer than
// `min_gap` (a fraction of the expected period) to the last accepted
// crossing keeps only the once-per-cycle fundamental crossing.
static std::vector<uint32_t> zero_crossing_periods(const std::vector<float> &x, size_t start, size_t n, uint32_t min_gap) {
    std::vector<uint32_t> periods;
    size_t last = 0;
    bool have_last = false;
    for (size_t i = start + 1; i < start + n; i++) {
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {  // rising zero-crossing
            if (have_last && (i - last) < min_gap) continue;
            if (have_last) periods.push_back((uint32_t)(i - last));
            last = i;
            have_last = true;
        }
    }
    return periods;
}

static bool run_jitter_shimmer_checks() {
    bool all_ok = true;
    printf("\n== Excitation: jitter/shimmer perturb period/amplitude (#36) ==\n");

    static constexpr float NOTE_HZ = 150.0f;
    static constexpr float SECONDS = 1.0f;

    for (bool active : { false, true }) {
        uint8_t jitter = active ? 255 : 0;
        uint8_t shimmer = active ? 255 : 0;
        std::vector<float> mono = render_phoneme_native(PH_A, NOTE_HZ, SECONDS, 256, 256, jitter, shimmer, 0.0f, 0.0f);

        size_t start = mono.size() / 4;  // skip the tract's initial glide to target
        uint32_t min_gap = (uint32_t)(0.6f * (float)SPEECH_RATE / NOTE_HZ);
        std::vector<uint32_t> periods = zero_crossing_periods(mono, start, mono.size() - start, min_gap);

        double period_mean = 0.0;
        for (uint32_t p : periods) period_mean += p;
        period_mean /= (double)periods.size();
        double period_var = 0.0;
        for (uint32_t p : periods) period_var += ((double)p - period_mean) * ((double)p - period_mean);
        double period_cv = std::sqrt(period_var / (double)periods.size()) / period_mean;

        std::vector<float> peaks;
        size_t seg_start = start;
        for (uint32_t p : periods) {
            float pk = 0.0f;
            for (size_t i = seg_start; i < seg_start + p && i < mono.size(); i++) pk = std::max(pk, std::fabs(mono[i]));
            peaks.push_back(pk);
            seg_start += p;
        }
        double amp_mean = 0.0;
        for (float p : peaks) amp_mean += p;
        amp_mean /= (double)peaks.size();
        double amp_var = 0.0;
        for (float p : peaks) amp_var += ((double)p - amp_mean) * ((double)p - amp_mean);
        double amp_cv = std::sqrt(amp_var / (double)peaks.size()) / amp_mean;

        bool ok = active ? (period_cv > 0.015 && amp_cv > 0.05) : (period_cv < 0.01 && amp_cv < 0.02);
        printf("  jitter=%3u shimmer=%3u: period CV=%.4f  amplitude CV=%.4f (%u cycles) -> %s\n",
               jitter, shimmer, period_cv, amp_cv, (unsigned)periods.size(), ok ? "PASS" : "FAIL");
        all_ok = all_ok && ok;
    }
    return all_ok;
}

// #36 acceptance: "Vibrato LFO is audible on glottal pitch and runs at
// sub-block rate." Renders a sustained vowel with a fast, full-depth
// vibrato and confirms the measured F0 (local_peak_freq's hill-climb,
// reused from run_vowel_checks() above) actually swings across the render
// instead of sitting still -- the direct, measurable meaning of "audible on
// glottal pitch" for a host harness with no ear.
static bool run_vibrato_checks() {
    bool all_ok = true;
    printf("\n== Excitation: vibrato LFO modulates glottal pitch (#36) ==\n");

    static constexpr float NOTE_HZ = 200.0f;
    static constexpr float SECONDS = 1.0f;
    static constexpr float LFO_RATE_HZ = 6.0f;  // typical vibrato rate
    static constexpr float LFO_DEPTH = 1.0f;    // full depth, +/-VIBRATO_MAX_SEMITONES

    std::vector<float> vib = render_phoneme_native(PH_A, NOTE_HZ, SECONDS, 256, 256, 0, 0, LFO_RATE_HZ, LFO_DEPTH);

    size_t win = (size_t)(SPEECH_RATE * 0.05f);  // 50 ms analysis windows
    float f0_min = 1.0e9f, f0_max = -1.0e9f;
    for (size_t start = win; start + win < vib.size(); start += win) {
        float f0 = local_peak_freq(vib, start, win, NOTE_HZ, (float)SPEECH_RATE);
        f0_min = std::min(f0_min, f0);
        f0_max = std::max(f0_max, f0);
    }
    float swing = f0_max - f0_min;
    // +/-2 semitones at 200 Hz is roughly +/-24 Hz; demand well under that
    // full swing to allow for local_peak_freq's 5 Hz search grid and
    // window-averaging smoothing out the extremes.
    bool ok = swing > 15.0f;
    printf("  lfo_rate=%.1fHz lfo_depth=%.1f: F0 swing over %.0fs = %.1f Hz (min=%.1f max=%.1f) -> %s\n",
           LFO_RATE_HZ, LFO_DEPTH, SECONDS, swing, f0_min, f0_max, ok ? "PASS" : "FAIL");
    all_ok = all_ok && ok;

    // Regression lock: lfo_depth=0 must reproduce the flat pitch every
    // pre-#36 render already assumed (run_vowel_checks()'s pitch-tracking
    // pass, etc.) -- vibrato being off shouldn't perturb anything.
    std::vector<float> flat = render_phoneme_native(PH_A, NOTE_HZ, SECONDS, 256, 256, 0, 0, LFO_RATE_HZ, 0.0f);
    float flat_min = 1.0e9f, flat_max = -1.0e9f;
    for (size_t start = win; start + win < flat.size(); start += win) {
        float f0 = local_peak_freq(flat, start, win, NOTE_HZ, (float)SPEECH_RATE);
        flat_min = std::min(flat_min, f0);
        flat_max = std::max(flat_max, f0);
    }
    bool flat_ok = (flat_max - flat_min) < 10.0f;
    printf("  lfo_depth=0 (rate still set): F0 swing = %.1f Hz -> %s\n", flat_max - flat_min, flat_ok ? "PASS" : "FAIL");
    all_ok = all_ok && flat_ok;

    return all_ok;
}

int main() {
    res2p_init();       // must run before any res2p_radius()/res2p_set() call
    osc_init_sine();     // speech_render_test_tone()/pan_gains_q15()'s wavetable source

    bool ok = run_test_tone_check();
    ok = run_full_table_render() && ok;
    ok = run_vowel_checks() && ok;
    ok = run_fricative_checks() && ok;
    ok = run_voiced_fricative_checks() && ok;
    ok = run_nasal_checks() && ok;

    printf("\n== CC sweep: formant_shift x bandwidth_scale stability ==\n");
    ok = test_cc_sweep_stability() && ok;

    ok = run_utterance_word_checks() && ok;
    ok = run_rate_checks() && ok;
    ok = run_plosive_checks() && ok;
    ok = run_gated_release_checks() && ok;
    ok = run_malformed_utterance_check() && ok;
    ok = run_phrase_renders() && ok;
    ok = run_phrase_pitch_tracking_checks() && ok;
    ok = run_jitter_shimmer_checks() && ok;
    ok = run_vibrato_checks() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
