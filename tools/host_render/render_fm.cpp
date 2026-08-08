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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef T00T_FM_HAS_PATCHES
#include "../../src/engines/fm/patches.h"
#endif

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

// #45's acceptance criteria on env_dx.h directly: operator output level
// goes through a real nonlinear (log/exp) curve rather than
// `gain = reference * level/99`, and velocity sensitivity is a genuine
// per-operator effect (0 at sensitivity=0, real and monotonic otherwise).
static bool run_level_table_checks() {
    env_dx_init_tables();

    constexpr int32_t REFERENCE = 1 << 24;  // arbitrary, mid-range reference gain

    // Level 99 = the reference exactly (0 dB offset); level 0 = an exact
    // digital 0 (env_dx.h's EG_SILENCE_THRESHOLD guarantee); the table is
    // monotonically non-decreasing across 0-99.
    int32_t g99 = eg_to_linear(REFERENCE, DX7_LEVEL_TO_LOG2[99]);
    int32_t g0 = eg_to_linear(REFERENCE, DX7_LEVEL_TO_LOG2[0]);
    bool unity_ok = g99 == REFERENCE;
    bool floor_ok = g0 == 0;
    bool monotonic_ok = true;
    for (uint32_t lvl = 1; lvl < 100; lvl++) {
        if (DX7_LEVEL_TO_LOG2[lvl] < DX7_LEVEL_TO_LOG2[lvl - 1]) monotonic_ok = false;
    }

    // Nonlinear, not "a linear approximation": level 50's linear gain must
    // NOT be anywhere near reference*50/99 (a straight-line curve) -- the
    // log-domain table makes it much quieter than that, since half the
    // level-parameter range is only a fraction of the dB range.
    int32_t g50 = eg_to_linear(REFERENCE, DX7_LEVEL_TO_LOG2[50]);
    float linear_guess = (float)REFERENCE * 50.0f / 99.0f;
    bool nonlinear_ok = (float)g50 < linear_guess * 0.5f;

    // Velocity sensitivity: 0 -> no effect regardless of velocity; nonzero
    // -> real, monotonic (softer hits are strictly quieter than harder
    // hits on the same sensitivity, and 0 velocity is strictly quieter at
    // sensitivity 7 than at sensitivity 1).
    int32_t sens0_soft = eg_vel_sensitivity_log2(0, 0);
    int32_t sens0_hard = eg_vel_sensitivity_log2(0, 32767);
    bool sens_zero_ok = sens0_soft == 0 && sens0_hard == 0;

    int32_t sens7_soft = eg_vel_sensitivity_log2(7, 0);
    int32_t sens7_hard = eg_vel_sensitivity_log2(7, 32767);
    int32_t sens1_soft = eg_vel_sensitivity_log2(1, 0);
    bool sens_effect_ok = sens7_hard == 0 && sens7_soft < 0 && sens7_soft < sens1_soft;

    bool pass = unity_ok && floor_ok && monotonic_ok && nonlinear_ok && sens_zero_ok && sens_effect_ok;
    printf("%s: level table -- L99=reference(%d)=%d, L0=exact-0(%d), monotonic=%d, "
           "L50 nonlinear (got %d, linear guess %.0f)=%d, vel_sens=0 no-op=%d, "
           "vel_sens=7 real+monotonic=%d\n",
           pass ? "PASS" : "FAIL", (int)unity_ok, g99, (int)floor_ok, (int)monotonic_ok,
           g50, linear_guess, (int)nonlinear_ok, (int)sens_zero_ok, (int)sens_effect_ok);
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

// A "flat EG" copy of FM_TEST_PATCH -- same routing/ratios/levels, but
// every operator's EG jumps straight to, and holds at, full level (all
// three pre-release stage levels = 99, fast attack). Used only by
// run_patch_spectrum_check() below: #44's routing/ratio/sideband claims are
// about the *routing*, not the *envelope shape*, so measuring them against
// a stable, non-decaying level isolates that claim from #45's (deliberately
// fast-decaying, EP-style) real EG shape -- which has its own dedicated
// checks in run_eg_shape_check().
static FmPatch flat_eg_patch() {
    FmPatch p = FM_TEST_PATCH;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        p.op[i].eg_rate[0] = 99;
        p.op[i].eg_level[0] = 99;
        p.op[i].eg_level[1] = 99;
        p.op[i].eg_level[2] = 99;
    }
    return p;
}

// Renders `total` samples of `patch` at `note_hz` (default: one second)
// through the exact device code path (fm_voice_note_on/
// fm_voice_update_pitch/fm_render_voice, op.h), mono-summed for spectral
// analysis. Never releases (no fm_voice_note_off call) -- callers that need
// release behaviour use render_patch_release() below instead.
static void render_patch_note(const FmPatch &patch, float note_hz, const FmRouting &routing,
                               FmVoiceBuses &bus, std::vector<float> &mono, int32_t &peak,
                               uint32_t total = SAMPLE_RATE) {
    FmOp ops[FM_NUM_OPS];
    uint32_t inc = fm_phase_inc(note_hz);
    // #48: fm_voice_note_on() now needs the raw MIDI note (key level/rate
    // scaling), not just the Hz-derived phase increment -- inverting
    // midi_controller.cpp's own note-to-Hz formula (A4=69=440Hz) rather
    // than adding a second, separate "midinote" parameter every caller
    // here would have to also track alongside note_hz.
    int note_round = (int)lroundf(69.0f + 12.0f * log2f(note_hz / 440.0f));
    uint8_t midinote = (uint8_t)std::clamp(note_round, 0, 127);
    fm_voice_note_on(ops, patch, inc, /*amplitude=*/32767, midinote);

    std::vector<int32_t> dl(total, 0), dr(total, 0);
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = std::min(NATIVE_BUFFER, total - done);
        fm_voice_update_pitch(ops, patch, inc);
        fm_render_voice(ops, patch, routing, bus, /*pan=*/0, dl.data() + done, dr.data() + done, n);
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
// drone), which is what "correct ratios across the keyboard" means. Uses
// flat_eg_patch() (above), not FM_TEST_PATCH's real EG shape, so a
// deliberately-fast-decaying modulator (op4, #45's EP timbre) doesn't
// confound a check that's fundamentally about routing, not envelopes.
static bool run_patch_spectrum_check() {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();

    const FmPatch patch = flat_eg_patch();

    FmRouting routing;
    if (!fm_resolve_routing(patch, routing)) {
        printf("FAIL: FM_TEST_PATCH itself failed DAG validation\n");
        return false;
    }

    static int32_t bus0[FM_BLOCK], bus1[FM_BLOCK], bus2[FM_BLOCK];
    static int32_t bus3[FM_BLOCK], bus4[FM_BLOCK], bus5[FM_BLOCK], bus_out[FM_BLOCK];
    FmVoiceBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    constexpr float NOTE_LOW = 220.0f, NOTE_HIGH = 440.0f;  // one octave apart

    std::vector<float> low, high;
    int32_t peak_low, peak_high;
    render_patch_note(patch, NOTE_LOW, routing, bus, low, peak_low);
    render_patch_note(patch, NOTE_HIGH, routing, bus, high, peak_high);

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

    // (1) harmonic content: 2nd harmonic must clear the noise floor by a
    // real margin, and a genuinely off-grid probe must NOT (otherwise the
    // "signal" is just broadband noise, not the specific sideband structure
    // FM predicts). The floor probe used to sit at NOTE_LOW*4.5 -- safely
    // off-grid back when op0's real modulation depth was negligible (pre-
    // #57's ceiling fix), because the audible spectrum was then dominated
    // by NOTE_LOW's own 220 Hz-multiple grid. #57 raised real depth enough
    // that op0's ratio (0.5, two hops upstream of the carrier through op2/
    // op4) now visibly pulls the *true* fundamental down to NOTE_LOW*0.5 --
    // host-verified (#57): every multiple of 110 Hz carries real energy,
    // and 990 Hz (NOTE_LOW*4.5) is exactly the 9th one, not noise at all.
    // NOTE_LOW*4.3 (946 Hz) isn't a multiple of either grid -- verified ~0.01.
    float h1 = goertzel_mag(low, start, n, NOTE_LOW * 1, (float)SAMPLE_RATE);
    float h2 = goertzel_mag(low, start, n, NOTE_LOW * 2, (float)SAMPLE_RATE);
    float floor_mag = goertzel_mag(low, start, n, NOTE_LOW * 4.3f, (float)SAMPLE_RATE);
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

// #45's EG acceptance criteria, checked directly on op.h's per-operator
// `gain` rather than the mixed audio: with six real, distinct EGs summed
// into one output bus, a spectral/level read of the *mix* can't cleanly
// attribute a level change to any one operator. Stepping FM_TEST_PATCH's
// real (non-flat) EGs block-by-block and reading `ops[i].gain` directly
// is the exact same computation audio_engine.cpp does every buffer, just
// with the per-operator intermediate values kept instead of thrown away
// after mixing -- so this is still testing the real code path, not a
// simulation of it.
static bool run_eg_shape_check() {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();

    FmOp ops[FM_NUM_OPS];
    uint32_t inc = fm_phase_inc(220.0f);
    fm_voice_note_on(ops, FM_TEST_PATCH, inc, /*amplitude=*/32767, /*midinote=*/57);  // A3, matches 220Hz

    // Step in FM_BLOCK-sized increments (same granularity the device
    // uses), recording each operator's gain at a handful of checkpoints.
    // Checkpoints were 5/100/800ms until #59: real DX7 rates (ported from
    // Dexed, replacing #45's ~20x-too-slow-at-R99 exponential guess) settle
    // FM_TEST_PATCH's envelopes into their stage-3 sustain within ~200ms,
    // not ~800ms+ -- host-probed directly (op4/op5 both flat, unchanging,
    // well before 200ms) before picking these -- so 1ms/200ms is what
    // "near attack peak" / "settled" now actually mean.
    uint32_t done = 0;
    auto step_to = [&](uint32_t target_sample, int32_t out_gain[FM_NUM_OPS]) {
        while (done < target_sample) {
            uint32_t n = std::min(FM_BLOCK, target_sample - done);
            fm_voice_step_envelopes(ops, FM_TEST_PATCH, n);
            done += n;
        }
        for (uint8_t i = 0; i < FM_NUM_OPS; i++) out_gain[i] = ops[i].gain;
    };

    int32_t g_1ms[FM_NUM_OPS], g_200ms[FM_NUM_OPS];
    step_to((uint32_t)(0.001f * SAMPLE_RATE), g_1ms);
    step_to((uint32_t)(0.200f * SAMPLE_RATE), g_200ms);

    // (1) Attack: every operator should have risen to a real, nonzero
    // level within 1ms (all six have R1=90-99; real DX7 R1=99 is near-
    // instantaneous, ~0.02ms for a full sweep -- see env_dx.h's rate curve).
    bool attack_ok = true;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) attack_ok = attack_ok && (g_1ms[i] != 0);

    // (2) Independence: op4 (the modulator, EG_LEVEL {99,20,15,0}) decays
    // to a much smaller fraction of its own 1ms level than op5 (the
    // carrier, {99,70,60,0}) does of its own -- the whole point of the EP
    // patch (fm.md P2 gate). Compared as fractions, not raw gain, since
    // op4 and op5 have very different reference `level` magnitudes
    // (patch.h's #44 modulator-vs-carrier scale, unrelated to the EG).
    float op4_frac = std::fabs((float)g_200ms[4] / (float)g_1ms[4]);
    float op5_frac = std::fabs((float)g_200ms[5] / (float)g_1ms[5]);
    bool independence_ok = op4_frac < op5_frac * 0.6f;

    // (3) All six operators land at genuinely different gains at 200ms --
    // six independent EGs, not six copies of one shape (guards against a
    // copy-paste patch or a step function that ignores per-op rate/level
    // data entirely).
    int32_t distinct = 0;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        bool unique = true;
        for (uint8_t j = 0; j < i; j++) if (g_200ms[i] == g_200ms[j]) unique = false;
        if (unique) distinct++;
    }
    bool distinct_ok = distinct == FM_NUM_OPS;

    bool pass = attack_ok && independence_ok && distinct_ok;
    printf("%s: EG shape -- attack(1ms) nonzero=%d, op4/op5 200ms-decay-fraction=%.4f/%.4f "
           "(independence=%d), %d/%d operators land at distinct 200ms gains\n",
           pass ? "PASS" : "FAIL", attack_ok, op4_frac, op5_frac, independence_ok,
           (int)distinct, (int)FM_NUM_OPS);
    if (!attack_ok) printf("  FAIL: at least one operator never rose off silence\n");
    if (!independence_ok) printf("  FAIL: op4 doesn't decay meaningfully faster/further than op5\n");
    if (!distinct_ok) printf("  FAIL: two or more operators landed at the identical gain -- not independent\n");
    return pass;
}

// #45's acceptance criterion: "Note-off releases through the EG's release
// stage; a voice reports itself free only when its carriers have actually
// decayed." Verifies both halves: fm_voice_active() does NOT drop the
// instant gate goes false (there IS a release, not #44's hard cutoff), and
// it DOES eventually become false, with the carrier's own gain landing on
// an exact 0 -- not an epsilon-close guess (env_dx.h's EG_SILENCE_THRESHOLD
// guarantee) -- avoiding the tracker's #21 "key-off never frees a voice"
// bug.
static bool run_release_check() {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();

    FmRouting routing;
    if (!fm_resolve_routing(FM_TEST_PATCH, routing)) {
        printf("FAIL: FM_TEST_PATCH failed DAG validation\n");
        return false;
    }

    FmOp ops[FM_NUM_OPS];
    uint32_t inc = fm_phase_inc(220.0f);
    fm_voice_note_on(ops, FM_TEST_PATCH, inc, /*amplitude=*/32767, /*midinote=*/57);  // A3, matches 220Hz

    // Let the note settle into its held (stage-3) shape before releasing --
    // 300ms is comfortably past every operator's stage-1/2 transition at
    // FM_TEST_PATCH's rates.
    uint32_t done = 0;
    while (done < (uint32_t)(0.3f * SAMPLE_RATE)) {
        uint32_t n = std::min(FM_BLOCK, (uint32_t)(0.3f * SAMPLE_RATE) - done);
        fm_voice_step_envelopes(ops, FM_TEST_PATCH, n);
        done += n;
    }
    bool active_before_release = fm_voice_active(ops, routing);

    fm_voice_note_off(ops);
    bool active_immediately_after = fm_voice_active(ops, routing);  // release just started -- must still be true

    // Step forward up to 5 seconds (comfortably past even a rate-40-ish
    // release at the far end of env_dx.h's rate curve) looking for idle.
    bool became_idle = false;
    uint32_t release_samples = 0;
    const uint32_t max_samples = (uint32_t)(5.0f * SAMPLE_RATE);
    while (release_samples < max_samples) {
        uint32_t n = std::min(FM_BLOCK, max_samples - release_samples);
        fm_voice_step_envelopes(ops, FM_TEST_PATCH, n);
        release_samples += n;
        if (!fm_voice_active(ops, routing)) { became_idle = true; break; }
    }

    // Once idle, the carrier's own gain (op5) must be an EXACT 0 -- the
    // real guarantee, not merely "fm_voice_active() said so".
    bool carrier_zero = became_idle && ops[5].gain == 0 && ops[5].gain_step == 0;

    bool pass = active_before_release && active_immediately_after && became_idle && carrier_zero;
    printf("%s: release -- active before release=%d, still active right after note-off=%d, "
           "went idle after %.0f ms, carrier gain exactly 0=%d\n",
           pass ? "PASS" : "FAIL", active_before_release, active_immediately_after,
           1000.0f * (float)release_samples / (float)SAMPLE_RATE, carrier_zero);
    if (!active_before_release) printf("  FAIL: voice wasn't active before release -- test setup is wrong\n");
    if (!active_immediately_after) printf("  FAIL: note-off silenced the voice instantly -- that's #44's hard cutoff, not a release\n");
    if (!became_idle) printf("  FAIL: voice never went idle within 5s of release -- this IS the tracker's #21 bug\n");
    if (became_idle && !carrier_zero) printf("  FAIL: idle but carrier gain isn't exactly 0\n");
    return pass;
}

// #48's acceptance criteria: key level scaling and rate scaling are real,
// audible, note-dependent effects. FM_TEST_PATCH has zero scaling on every
// operator (the new fields all default-zero, #48's own behavior-neutrality
// requirement), so nothing else in this file exercises `dx7_scale_level`/
// `dx7_scale_rate` at all -- this builds a small patch that deliberately
// does, and checks both the resolved note-on-time values and the resulting
// per-sample effect, not just that the code compiles and runs.
static bool run_key_rate_scaling_check() {
    fm_init_sine_tab();
    env_dx_init_tables();

    FmPatch patch = FM_TEST_PATCH;
    FmOpParams &carrier = patch.op[5];
    carrier.output_level = 50;       // mid-range, so a boost or cut is measurable either direction
    carrier.scale_breakpoint = 60;   // ~middle C-ish
    carrier.scale_left_depth = 0;
    carrier.scale_right_depth = 99;  // strong effect above the breakpoint
    carrier.scale_left_curve = 0;
    carrier.scale_right_curve = 3;   // +LIN: notes above breakpoint get LOUDER
    carrier.rate_scaling = 7;        // max: notes further from the reference get a real speed boost
    carrier.eg_rate[1] = 40;         // a deliberately moderate (not 99) stage-2 rate, so scaling has room to matter

    FmOp low[FM_NUM_OPS], high[FM_NUM_OPS];
    uint32_t inc_low = fm_phase_inc(110.0f), inc_high = fm_phase_inc(880.0f);
    fm_voice_note_on(low, patch, inc_low, 32767, /*midinote=*/30);
    fm_voice_note_on(high, patch, inc_high, 32767, /*midinote=*/96);

    // (1) Key level scaling: resolved once at note-on into static_log2 --
    // the high (above-breakpoint, +LIN) note must resolve louder than the
    // low (below-breakpoint, 0 depth -> no effect) note.
    bool level_scaling_ok = high[5].static_log2 > low[5].static_log2;

    // (2) Rate scaling: resolved once at note-on into rate_scale_qrate --
    // dx7_scale_rate() is monotonic in distance from its low reference note,
    // so midinote=96 must resolve a larger (faster) delta than midinote=30.
    bool rate_scale_resolved_ok = high[5].rate_scale_qrate > low[5].rate_scale_qrate
                                 && low[5].rate_scale_qrate >= 0;

    // (3) The actual per-sample effect: step both into stage 2 (past the
    // instant R1=99 attack) and compare how far each has moved after a
    // FIXED number of samples -- the high note, with a real rate_scale_qrate
    // boost on top of the same base rate 40, must move further/faster.
    auto step_into_stage2 = [&](FmOp ops[FM_NUM_OPS], const FmPatch &p) {
        uint32_t done = 0;
        while (ops[5].eg.stage != EG_STAGE_2 && done < SAMPLE_RATE) {
            fm_voice_step_envelopes(ops, p, FM_BLOCK);
            done += FM_BLOCK;
        }
    };
    step_into_stage2(low, patch);
    step_into_stage2(high, patch);
    int32_t low_stage2_start = low[5].gain, high_stage2_start = high[5].gain;
    for (int i = 0; i < 20; i++) {
        fm_voice_step_envelopes(low, patch, FM_BLOCK);
        fm_voice_step_envelopes(high, patch, FM_BLOCK);
    }
    int32_t low_moved = std::abs(low[5].gain - low_stage2_start);
    int32_t high_moved = std::abs(high[5].gain - high_stage2_start);
    bool rate_effect_ok = high_moved > low_moved;

    bool pass = level_scaling_ok && rate_scale_resolved_ok && rate_effect_ok;
    printf("%s: key/rate scaling -- static_log2(low/high)=%d/%d (level_scaling=%d), "
           "rate_scale_qrate(low/high)=%d/%d (resolved=%d), stage-2 movement(low/high)=%d/%d (effect=%d)\n",
           pass ? "PASS" : "FAIL", low[5].static_log2, high[5].static_log2, level_scaling_ok,
           low[5].rate_scale_qrate, high[5].rate_scale_qrate, rate_scale_resolved_ok,
           low_moved, high_moved, rate_effect_ok);
    if (!level_scaling_ok) printf("  FAIL: key level scaling didn't make the above-breakpoint note louder\n");
    if (!rate_scale_resolved_ok) printf("  FAIL: rate scaling didn't resolve a larger delta for the higher note\n");
    if (!rate_effect_ok) printf("  FAIL: rate scaling's note-on delta didn't produce a real per-sample speed difference\n");
    return pass;
}

#ifdef T00T_FM_HAS_PATCHES
// #47's acceptance criterion: "render_fm renders a fixed set of notes per
// patch to WAV on the host, so #53 has something to diff against Dexed."
// One held note (A3, matching NOTE_LOW's convention above), 3 seconds --
// longer than render_patch_note()'s 1s default, since real DX7 patches
// include deliberately slow-swelling sound effects (ROM1A's "TAKE OFF",
// R1=9, is inaudible inside 1s but real inside 3s) -- per patch in the
// generated bank. #53's own job is the actual Dexed diff and any
// multi-note/velocity-layer coverage that needs; this just has to produce
// real, bounded, non-silent audio for every patch that made it through
// syx2patch.py, through the exact same fm_resolve_routing()/
// fm_voice_note_on()/fm_render_voice() path everything else in this file
// uses. Conditionally compiled -- patches.h is generated locally and
// gitignored (see CMakeLists.txt), so this function (and its #include
// above) only exist once someone has run tools/syx2patch.py.
static bool run_patch_bank_render() {
    fm_init_sine_tab();
    osc_init_sine();
    env_dx_init_tables();

    mkdir("fm_patches", 0755);  // ignore EEXIST -- fine if it's already there

    constexpr float NOTE_HZ = 220.0f;
    constexpr uint32_t DURATION = SAMPLE_RATE * 3;
    int32_t bus0[FM_BLOCK], bus1[FM_BLOCK], bus2[FM_BLOCK];
    int32_t bus3[FM_BLOCK], bus4[FM_BLOCK], bus5[FM_BLOCK], bus_out[FM_BLOCK];
    FmVoiceBuses bus{ { bus0, bus1, bus2, bus3, bus4, bus5 }, bus_out };

    uint32_t rendered = 0, bounded_ok = 0;
    for (uint32_t p = 0; p < FM_PATCH_COUNT; p++) {
        const FmPatch &patch = FM_PATCHES[p];
        FmRouting routing;
        if (!fm_resolve_routing(patch, routing)) {
            printf("FAIL: patch %u (\"%s\") failed DAG validation -- syx2patch.py should "
                   "never emit an unresolvable patch\n", p, patch.name);
            continue;
        }

        std::vector<float> mono;
        int32_t peak;
        render_patch_note(patch, NOTE_HZ, routing, bus, mono, peak, DURATION);
        rendered++;

        std::string fname = "fm_patches/";
        for (const char *c = patch.name; *c; c++) {
            fname += (isalnum((unsigned char)*c)) ? *c : '_';
        }
        fname += ".wav";

        std::vector<int16_t> wav(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); i++) {
            int16_t s = (int16_t)std::clamp(mono[i], -32768.0f, 32767.0f);
            wav[i * 2 + 0] = s;
            wav[i * 2 + 1] = s;
        }
        bool wrote = write_wav_pcm16(fname.c_str(), wav, SAMPLE_RATE, 2);
        bool bounded = wrote && peak > 0 && peak < 32768;
        if (bounded) bounded_ok++;

        // #57 regression guard: real sideband energy relative to the
        // patch's own fundamental, not just "bounded and non-silent"
        // (which #47 alone already checked, and which a near-pure sine
        // satisfies just as well as a rich FM tone -- exactly the bug #57
        // fixed). The fundamental is the *lowest-ratio carrier's* own
        // frequency, not necessarily note_hz -- carriers can run at
        // fractional ratios (BRASS 1's op0 is 0.5x), so assuming note_hz
        // would silently probe the wrong bin for those patches.
        float fundamental_ratio = -1.0f;
        for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
            if (patch.op[i].mod_target == FM_TARGET_OUT) {
                if (fundamental_ratio < 0.0f || patch.op[i].ratio < fundamental_ratio) {
                    fundamental_ratio = patch.op[i].ratio;
                }
            }
        }
        float thd_ratio = 0.0f;
        if (fundamental_ratio > 0.0f) {
            float fundamental_hz = NOTE_HZ * fundamental_ratio;
            size_t start = SAMPLE_RATE / 4, n = SAMPLE_RATE / 2;
            float f1 = goertzel_mag(mono, start, n, fundamental_hz, (float)SAMPLE_RATE);
            float f2 = goertzel_mag(mono, start, n, fundamental_hz * 2.0f, (float)SAMPLE_RATE);
            if (f1 > 1.0f) thd_ratio = f2 / f1;
        }

        printf("  %s: %s -- peak=%d, 2nd-harmonic/fundamental=%.3f\n",
               bounded ? "ok" : "FAIL", fname.c_str(), peak, thd_ratio);
    }

    bool pass = rendered == FM_PATCH_COUNT && bounded_ok == FM_PATCH_COUNT;
    printf("%s: patch bank render -- %u/%u patches rendered, %u/%u bounded+non-silent, "
           "wrote fm_patches/*.wav (2nd-harmonic/fundamental ratio printed per patch -- "
           "#57 regression guard, not an automatic per-patch gate: a patch with no real "
           "modulation, e.g. an all-carrier algorithm at feedback_level=0, legitimately "
           "has close to zero)\n", pass ? "PASS" : "FAIL", rendered, (uint32_t)FM_PATCH_COUNT,
           bounded_ok, (uint32_t)FM_PATCH_COUNT);
    return pass;
}
#endif

int main() {
    bool ok1 = run_test_tone_check();
    bool ok2 = run_routing_checks();
    bool ok3 = run_level_table_checks();
    bool ok4 = run_patch_spectrum_check();
    bool ok5 = run_eg_shape_check();
    bool ok6 = run_release_check();
    bool ok8 = run_key_rate_scaling_check();
    bool ok = ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok8;
#ifdef T00T_FM_HAS_PATCHES
    bool ok7 = run_patch_bank_render();
    ok = ok && ok7;
#endif
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
