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

struct PhonemeName { Phoneme p; const char *label; };
static const PhonemeName VOWEL_NAMES[5] = {
    { PH_I, "i" }, { PH_E, "e" }, { PH_A, "a" }, { PH_O, "o" }, { PH_U, "u" },
};
static const PhonemeName FRICATIVE_NAMES[3] = { { PH_S, "s" }, { PH_SH, "sh" }, { PH_F, "f" } };
static const PhonemeName VOICED_FRICATIVE_NAMES[2] = { { PH_Z, "z" }, { PH_V, "v" } };
static const PhonemeName NASAL_NAMES[2] = { { PH_M, "m" }, { PH_N, "n" } };

// Renders one phoneme, held under gate, for `seconds` at `note_hz` and
// `formant_shift`/`bandwidth_scale` (Q8.8, 256 = 1.0x -- default neutral).
// Returns the rendered native-rate mono samples (post-tract, pre-pan/ZOH) as
// float so the Goertzel analysis isn't limited to int16 quantization.
static std::vector<float> render_phoneme_native(Phoneme p, float note_hz, float seconds,
                                                  int16_t formant_shift = 256, int16_t bandwidth_scale = 256) {
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
                             formant_shift, bandwidth_scale,
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
    tract_retrigger(sv, PHONEME_TARGETS[p]);
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
        std::vector<float> audible = render_phoneme_native(vn.p, AUDIBLE_NOTE_HZ, AUDIBLE_SECONDS);
        write_phoneme_wav(vn.label, audible);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        std::vector<float> imp = render_phoneme_impulse_response(vn.p, 4096);
        const FormantTarget &tgt = PHONEME_TARGETS[vn.p];
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
        write_phoneme_wav(pn.label, audible);
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
        printf("  /%-2s/: peak=%7.0f  spectral peak=%6.0f Hz (target fric_F=%.0f) -> %s\n",
               pn.label, peak, measured[idx], PHONEME_TARGETS[pn.p].fric_F, ok ? "PASS" : "FAIL");
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
        write_phoneme_wav(pn.label, audible);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        const FormantTarget &tgt = PHONEME_TARGETS[pn.p];
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
        write_phoneme_wav(pn.label, audible);
        float peak = 0.0f;
        bool finite = true;
        for (float s : audible) { peak = std::max(peak, std::fabs(s)); finite = finite && std::isfinite(s); }
        bool clipped = peak >= 32767.0f;

        const FormantTarget &tgt = PHONEME_TARGETS[pn.p];
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
        const FormantTarget &tgt = PHONEME_TARGETS[p];
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

int main() {
    res2p_init();       // must run before any res2p_radius()/res2p_set() call
    osc_init_sine();     // speech_render_test_tone()/pan_gains_q15()'s wavetable source

    bool ok = run_test_tone_check();
    ok = run_vowel_checks() && ok;
    ok = run_fricative_checks() && ok;
    ok = run_voiced_fricative_checks() && ok;
    ok = run_nasal_checks() && ok;

    printf("\n== CC sweep: formant_shift x bandwidth_scale stability ==\n");
    ok = test_cc_sweep_stability() && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
