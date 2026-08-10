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
    cyclic.op[3].feedback_level = 0;
    FmRouting r_cyclic;
    bool cyclic_accepted = fm_resolve_routing(cyclic, r_cyclic);

    FmPatch self_target = FM_TEST_PATCH;
    self_target.op[1].mod_target = 1;  // malformed: not `feedback_level`'s job
    FmRouting r_self_target;
    bool self_target_accepted = fm_resolve_routing(self_target, r_self_target);

    bool pass = ok_valid && !cyclic_accepted && !self_target_accepted;
    printf("%s: routing compiler -- FM_TEST_PATCH (self-loop via feedback) valid=%d, "
           "2-op cycle rejected=%d, mod_target==self rejected=%d\n",
           pass ? "PASS" : "FAIL", ok_valid, !cyclic_accepted, !self_target_accepted);
    return pass;
}

// the old level model (eg_to_linear against DX7_LEVEL_TO_LOG2, unity at level
// 99, monotonicity, "not a linear ramp"), none of which exist any more --
// env_dx.h is now a direct port of Dexed's Env and folds output level into the
// envelope's own targets. The coverage moved somewhere strictly stronger:
// tools/fm_ctl_diff.py's table/scaleoutlevel and eg/* cases compare against
// Dexed's actual numbers rather than against this engine's own assumptions.

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
// through the exact device code path (fm_voice_note_on/fm_render_voice,
// op.h), mono-summed for spectral analysis. Never releases (no
// fm_voice_note_off call) -- callers that need release behaviour use
// render_patch_release() below instead.
//
// #49: always drives a real FmPitchEg/FmLfo (not the nullptr-skip path) --
// this is the "exact device code path" harness, and FM_TEST_PATCH's own
// pmd=amd=0 means every EXISTING caller here is still unaffected (lfo.h's
// own depth-fraction math is exactly 0 regardless of mod_wheel). Default
// mod_wheel=32767 (full) rather than 0 so a real DX7 patch bank's
// configured vibrato/tremolo (once syx2patch.py bakes lfo/pitch_eg fields)
// is actually audible in the batch-rendered fm_patches/*.wav bank by
// default -- see lfo.h's own header comment on why 0 would otherwise mean
// "no effect at all" regardless of patch data.
static void render_patch_note(const FmPatch &patch, float note_hz, const FmRouting &routing,
                               FmVoiceBuses &bus, std::vector<float> &mono, int32_t &peak,
                               uint32_t total = SAMPLE_RATE, int16_t mod_wheel = 32767) {
    FmOp ops[FM_NUM_OPS];
    FmPitchEg peg;
    FmLfo lfo;
    uint32_t inc = fm_phase_inc(note_hz);
    // #48: fm_voice_note_on() now needs the raw MIDI note (key level/rate
    // scaling), not just the Hz-derived phase increment -- inverting
    // midi_controller.cpp's own note-to-Hz formula (A4=69=440Hz) rather
    // than adding a second, separate "midinote" parameter every caller
    // here would have to also track alongside note_hz.
    int note_round = (int)lroundf(69.0f + 12.0f * log2f(note_hz / 440.0f));
    uint8_t midinote = (uint8_t)std::clamp(note_round, 0, 127);
    fm_voice_note_on(ops, patch, inc, /*amplitude=*/32767, midinote, &peg, &lfo);

    std::vector<int32_t> dl(total, 0), dr(total, 0);
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = std::min(NATIVE_BUFFER, total - done);
        fm_render_voice(ops, patch, routing, bus, /*pan=*/0, dl.data() + done, dr.data() + done, n,
                         &peg, &lfo, inc, mod_wheel);
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
    // Checkpoints were 1ms/200ms from #59 until the rate-conversion fix
    // below: `env_dx.h`'s `dx7_qrate_to_octaves_per_sec_q8()` was missing a
    // `>> LG_N` (LG_N=6, Dexed's own real per-render-call block size,
    // Source/msfa/synth.h) when converting Dexed's real `inc_` -- a Q24
    // octaves-per-*64-sample-Dexed-block* delta -- into an octaves-per-
    // *second* rate, so every stage of every envelope ran a real 64x too
    // fast (confirmed by building Dexed's actual env.cc/exp2.cc standalone
    // and A/B-rendering a real ROM1A patch against it -- a decay this
    // engine finished in ~600ms was, in real Dexed, still clearly sounding
    // past 2000ms). Host-probed directly against the *fixed* rate table:
    // FM_TEST_PATCH's slowest-attack operator (op3, R1=90) is still
    // *exactly* zero at 16ms and first nonzero at 17ms, and every operator
    // has fully settled into its stage-3 sustain (bit-for-bit unchanging
    // block to block) by 800ms -- so 25ms/1000ms (comfortable margin either
    // side) is what "near attack peak" / "settled" now actually mean, not
    // 1ms/200ms.
    uint32_t done = 0;
    auto step_to = [&](uint32_t target_sample, int32_t out_gain[FM_NUM_OPS]) {
        while (done < target_sample) {
            uint32_t n = std::min(FM_BLOCK, target_sample - done);
            fm_voice_step_envelopes(ops, FM_TEST_PATCH, n);
            done += n;
        }
        for (uint8_t i = 0; i < FM_NUM_OPS; i++) out_gain[i] = ops[i].gain;
    };

    int32_t g_25ms[FM_NUM_OPS], g_1000ms[FM_NUM_OPS];
    step_to((uint32_t)(0.025f * SAMPLE_RATE), g_25ms);
    step_to((uint32_t)(1.000f * SAMPLE_RATE), g_1000ms);

    // (1) Attack: every operator should have risen to a real, nonzero
    // level within 25ms (all six have R1=90-99; real DX7 R1=99 is
    // near-instantaneous, ~6.6ms for a full 16-octave sweep -- see
    // env_dx.h's rate curve -- but R1=90's slower stage-1 needs the extra
    // margin, see the checkpoint comment above).
    bool attack_ok = true;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) attack_ok = attack_ok && (g_25ms[i] != 0);

    // (2) Independence: op4 (the modulator, EG_LEVEL {99,20,15,0}) decays
    // to a much smaller fraction of its own 25ms level than op5 (the
    // carrier, {99,70,60,0}) does of its own -- the whole point of the EP
    // patch (fm.md P2 gate). Compared as fractions, not raw gain, since
    // op4 and op5 have very different reference `level` magnitudes
    // (patch.h's #44 modulator-vs-carrier scale, unrelated to the EG).
    float op4_frac = std::fabs((float)g_1000ms[4] / (float)g_25ms[4]);
    float op5_frac = std::fabs((float)g_1000ms[5] / (float)g_25ms[5]);
    bool independence_ok = op4_frac < op5_frac * 0.6f;

    // (3) All six operators land at genuinely different gains at 1000ms --
    // six independent EGs, not six copies of one shape (guards against a
    // copy-paste patch or a step function that ignores per-op rate/level
    // data entirely).
    int32_t distinct = 0;
    for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
        bool unique = true;
        for (uint8_t j = 0; j < i; j++) if (g_1000ms[i] == g_1000ms[j]) unique = false;
        if (unique) distinct++;
    }
    bool distinct_ok = distinct == FM_NUM_OPS;

    bool pass = attack_ok && independence_ok && distinct_ok;
    printf("%s: EG shape -- attack(25ms) nonzero=%d, op4/op5 1000ms-decay-fraction=%.4f/%.4f "
           "(independence=%d), %d/%d operators land at distinct 1000ms gains\n",
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

// FmOp::static_log2 / FmOp::rate_scale_qrate, fields F3 deleted when key
// scaling moved inside EnvDX. tools/fm_ctl_diff.py's table/scale-level (55040
// rows) and table/scale-rate (1024 rows) now check exactly this, exactly,
// against Dexed.

// #49's pitch EG acceptance criterion: "applied by scaling all six operator
// increments at block boundaries ... zero per-sample cost." Checked
// directly against pitch_eg.h's own state machine (trigger/step/release)
// plus op.h's fm_voice_step_pitch_and_mod() increment scaling, the same
// "read the intermediate state, not just the mixed audio" method
// run_eg_shape_check() uses for the amplitude EG -- a single voice's pitch
// is a scalar the mixed spectrum can't cleanly separate from ordinary
// vibrato/detune.
static bool run_pitch_eg_check() {
    fm_init_sine_tab();
    env_dx_init_tables();

    FmPatch patch = FM_TEST_PATCH;
    // Fast rise to L1 (80, well above center) = the "attack blip"; settles
    // at center (L2=L3=50); slow swoop to L4 (20, well below center) on
    // release. L4 != 50 is deliberate -- it's what makes fm_pitch_eg_trigger()
    // start from a real nonzero offset (matching Dexed's own "starts where
    // the previous note's release left off" convention) instead of 0.
    patch.pitch_eg = { {99, 99, 99, 60}, {80, 50, 50, 20} };
    patch.op[0].fixed_freq = true;  // must NOT be affected by pitch EG/LFO (matches Dexed's own fixed-mode exemption)
    patch.op[0].fixed_hz = 440.0f;

    FmPitchEg peg;
    fm_pitch_eg_trigger(peg, patch.pitch_eg);
    float start_cents = peg.current_cents;
    bool start_ok = std::fabs(start_cents - dx7_pitchenv_level_to_cents(20)) < 0.5f;

    uint32_t done = 0;
    float blip_cents = start_cents;
    while (done < (uint32_t)(0.01f * SAMPLE_RATE)) {
        blip_cents = fm_pitch_eg_step_block(peg, patch.pitch_eg, FM_BLOCK);
        done += FM_BLOCK;
    }
    bool blip_ok = blip_cents > start_cents + 50.0f;  // real upward movement within 10ms, not just nonzero noise

    while (done < (uint32_t)(0.3f * SAMPLE_RATE)) {
        fm_pitch_eg_step_block(peg, patch.pitch_eg, FM_BLOCK);
        done += FM_BLOCK;
    }
    float settled_cents = peg.current_cents;
    uint8_t settled_stage = peg.stage;
    bool settle_ok = std::fabs(settled_cents) < 1.0f && settled_stage == PEG_STAGE_3;  // holds at center, stage 3, while note is held

    fm_pitch_eg_release(peg);
    float just_after_release = fm_pitch_eg_step_block(peg, patch.pitch_eg, FM_BLOCK);
    bool release_moving_ok = just_after_release < settled_cents
                            && just_after_release > dx7_pitchenv_level_to_cents(20) + 1.0f;
    done = 0;
    while (done < (uint32_t)(2.0f * SAMPLE_RATE)) {
        fm_pitch_eg_step_block(peg, patch.pitch_eg, FM_BLOCK);
        done += FM_BLOCK;
    }
    bool release_settled_ok = std::fabs(peg.current_cents - dx7_pitchenv_level_to_cents(20)) < 1.0f
                             && peg.stage == PEG_STAGE_4;

    // Operator increment scaling: a non-fixed operator's `inc` moves with
    // the blip; a fixed-frequency operator's does not.
    FmOp ops[FM_NUM_OPS];
    FmPitchEg peg2;
    FmLfo lfo2;
    uint32_t note_inc = fm_phase_inc(220.0f);
    fm_voice_note_on(ops, patch, note_inc, 32767, /*midinote=*/57, &peg2, &lfo2);
    uint32_t base_inc_op1 = fm_op_inc(patch.op[1], note_inc);
    uint32_t base_inc_op0 = fm_op_inc(patch.op[0], note_inc);
    fm_voice_step_pitch_and_mod(ops, patch, peg2, lfo2, note_inc, FM_BLOCK, /*mod_wheel=*/32767);
    bool op1_moved_ok = ops[1].inc != base_inc_op1;
    bool op0_fixed_ok = ops[0].inc == base_inc_op0;

    bool pass = start_ok && blip_ok && settle_ok && release_moving_ok && release_settled_ok
              && op1_moved_ok && op0_fixed_ok;
    printf("%s: pitch EG -- start=%.1fc (want %.1f), blip(10ms)=%.1fc (>%.1f), "
           "settled=%.1fc@stage%d, release(1blk)=%.1fc, release_settled=%.1fc@stage%d, "
           "op1 inc moved=%d, op0(fixed) inc unchanged=%d\n",
           pass ? "PASS" : "FAIL", start_cents, dx7_pitchenv_level_to_cents(20), blip_cents,
           start_cents + 50.0f, settled_cents, settled_stage, just_after_release,
           peg.current_cents, peg.stage, op1_moved_ok, op0_fixed_ok);
    if (!start_ok) printf("  FAIL: trigger didn't start from L4\n");
    if (!blip_ok) printf("  FAIL: no real upward blip toward L1\n");
    if (!settle_ok) printf("  FAIL: didn't settle at center / stage 3\n");
    if (!release_moving_ok) printf("  FAIL: release didn't start swooping toward L4\n");
    if (!release_settled_ok) printf("  FAIL: release didn't settle at L4 / stage 4\n");
    if (!op1_moved_ok) printf("  FAIL: a ratio operator's inc didn't move with the pitch EG\n");
    if (!op0_fixed_ok) printf("  FAIL: a fixed-frequency operator's inc moved -- should be exempt\n");
    return pass;
}

// #49's LFO acceptance criteria: all six waveforms, rate, delay, PMD/AMD,
// key sync are real and correctly shaped -- checked directly against
// lfo.h's own tables/functions, plus one integration check that AM
// actually reaches fm_voice_step_envelopes()'s gain output.
static bool run_lfo_check() {
    fm_init_sine_tab();
    env_dx_init_tables();

    // (1) Waveform shapes, sampled at phase 0 / quarter / half / three-quarter.
    constexpr uint32_t Q0 = 0, Q1 = 0x40000000u, Q2 = 0x80000000u, Q3 = 0xC0000000u;
    bool tri_ok = fm_lfo_waveform_unipolar(Q0, 0) < 0.05f && fm_lfo_waveform_unipolar(Q2, 0) > 0.95f;
    bool sawdown_ok = fm_lfo_waveform_unipolar(Q0, 1) > 0.95f && fm_lfo_waveform_unipolar(Q3, 1) < 0.3f;
    bool sawup_ok = fm_lfo_waveform_unipolar(Q0, 2) < 0.05f && fm_lfo_waveform_unipolar(Q3, 2) > 0.7f;
    bool square_ok = fm_lfo_waveform_unipolar(Q1, 3) > 0.95f && fm_lfo_waveform_unipolar(Q3, 3) < 0.05f;
    bool sine_ok = fm_lfo_waveform_unipolar(Q1, 4) > 0.95f && fm_lfo_waveform_unipolar(Q3, 4) < 0.05f
                && std::fabs(fm_lfo_waveform_unipolar(Q0, 4) - 0.5f) < 0.05f;
    bool waveforms_ok = tri_ok && sawdown_ok && sawup_ok && square_ok && sine_ok;

    // (2) Rate table: real-Hz anchors (rate 0 ~0.065 Hz / ~15.5s period,
    // rate 99 ~50.9 Hz -- cross-checked against a standalone calibration
    // harness running Dexed's own real Lfo::init() formula).
    float hz0 = dx7_lfo_rate_to_hz(0), hz99 = dx7_lfo_rate_to_hz(99);
    bool rate_ok = hz0 > 0.05f && hz0 < 0.08f && hz99 > 45.0f && hz99 < 55.0f;

    // (3) Delay: instant at 0, ~2.66s at 99 (same calibration method).
    float delay0 = dx7_lfo_delay_seconds(0), delay99 = dx7_lfo_delay_seconds(99);
    bool delay_ok = delay0 == 0.0f && delay99 > 2.0f && delay99 < 3.5f;

    // (4) Full block-rate integration: fast LFO (rate 99, ~51 Hz), max
    // depth/sensitivity, sine, delay=0, mod_wheel at max -- over 0.5s
    // (>20 cycles at ~51Hz, densely sampled every FM_BLOCK) the pitch
    // output must swing through both signs near the calibrated max, and
    // amp_atten must swing up toward its own max.
    FmLfoParams p{ 99, 0, 99, 99, /*waveform=*/4, false, 7 };
    FmLfo lfo;
    fm_lfo_init(lfo);
    fm_lfo_trigger(lfo, false);
    float min_cents = 1e9f, max_cents = -1e9f, max_atten = 0.0f;
    uint32_t done = 0;
    while (done < (uint32_t)(0.5f * SAMPLE_RATE)) {
        float pc, aa;
        fm_lfo_step_block(lfo, p, FM_BLOCK, /*mod_wheel_frac=*/1.0f, pc, aa);
        min_cents = std::min(min_cents, pc);
        max_cents = std::max(max_cents, pc);
        max_atten = std::max(max_atten, aa);
        done += FM_BLOCK;
    }
    bool oscillates_ok = min_cents < -900.0f && max_cents > 900.0f;  // FM_LFO_PMD_MAX_CENTS is ~1190.6
    bool amp_swings_ok = max_atten > 0.7f;

    // (5) mod_wheel=0 must silence both outputs regardless of depth.
    FmLfo lfo_wheel0;
    fm_lfo_init(lfo_wheel0);
    fm_lfo_trigger(lfo_wheel0, false);
    bool wheel_zero_ok = true;
    done = 0;
    while (done < (uint32_t)(0.1f * SAMPLE_RATE)) {
        float pc, aa;
        fm_lfo_step_block(lfo_wheel0, p, FM_BLOCK, /*mod_wheel_frac=*/0.0f, pc, aa);
        if (pc != 0.0f || aa != 0.0f) wheel_zero_ok = false;
        done += FM_BLOCK;
    }

    // (6) Delay: with a slow delay (99, ~2.66s) and a waveform whose very
    // first sample is already at a bipolar extreme (square, unlike sine's
    // own zero-crossing start), the very first block after trigger must be
    // far smaller than that extreme -- the fade-in is real, not a no-op.
    FmLfoParams p_delay{ 99, 99, 99, 99, /*waveform=*/3, false, 7 };
    FmLfo lfo_delay;
    fm_lfo_init(lfo_delay);
    fm_lfo_trigger(lfo_delay, false);
    float first_pc, first_aa;
    fm_lfo_step_block(lfo_delay, p_delay, FM_BLOCK, 1.0f, first_pc, first_aa);
    bool delay_ramp_ok = std::fabs(first_pc) < 5.0f;  // negligible this early into a ~2.66s ramp (undelayed would be ~1190c immediately)

    // (7) AM integration: fm_voice_step_envelopes() actually reduces gain
    // for an operator with real am_sensitivity, and leaves one with
    // am_sensitivity=0 untouched, at the SAME amp_atten input.
    FmPatch patch = FM_TEST_PATCH;
    patch.op[5].am_sensitivity = 3;  // carrier: max AMS
    patch.op[4].am_sensitivity = 0;  // modulator: no AMS -- must be unaffected
    FmOp ops_a[FM_NUM_OPS], ops_b[FM_NUM_OPS];
    uint32_t inc = fm_phase_inc(220.0f);
    fm_voice_note_on(ops_a, patch, inc, 32767, 57);
    fm_voice_note_on(ops_b, patch, inc, 32767, 57);
    // Run both to a real, settled nonzero gain first (rate-99's attack is
    // fast but not literally one FM_BLOCK=16-sample/0.36ms step, per
    // run_eg_shape_check()'s own 25ms checkpoint -- env_dx.h's rate-
    // conversion fix, see that function's comment) before comparing --
    // otherwise both would still read exactly 0 and "b < a" would trivially
    // fail without AM being the reason.
    uint32_t settle = 0;
    while (settle < (uint32_t)(0.05f * SAMPLE_RATE)) {
        fm_voice_step_envelopes(ops_a, patch, FM_BLOCK, 0.0f);
        fm_voice_step_envelopes(ops_b, patch, FM_BLOCK, 0.0f);
        settle += FM_BLOCK;
    }
    fm_voice_step_envelopes(ops_a, patch, FM_BLOCK, /*amp_atten=*/0.0f);
    fm_voice_step_envelopes(ops_b, patch, FM_BLOCK, /*amp_atten=*/0.8f);
    bool am_reduces_ok = ops_b[5].gain < ops_a[5].gain;
    bool am_unaffected_ok = ops_b[4].gain == ops_a[4].gain;

    bool pass = waveforms_ok && rate_ok && delay_ok && oscillates_ok && amp_swings_ok
              && wheel_zero_ok && delay_ramp_ok && am_reduces_ok && am_unaffected_ok;
    printf("%s: LFO -- waveforms(tri/sawdn/sawup/sq/sine)=%d/%d/%d/%d/%d, rate(0/99)=%.4f/%.2f Hz, "
           "delay(0/99)=%.2f/%.2fs, pitch swing=[%.1f,%.1f]c, amp_atten max=%.3f, "
           "wheel=0 silences=%d, delay ramp small at t=0 (%.2fc)=%d, "
           "AM reduces sensitive op=%d, leaves insensitive op alone=%d\n",
           pass ? "PASS" : "FAIL", tri_ok, sawdown_ok, sawup_ok, square_ok, sine_ok, hz0, hz99,
           delay0, delay99, min_cents, max_cents, max_atten, wheel_zero_ok, first_pc, delay_ramp_ok,
           am_reduces_ok, am_unaffected_ok);
    if (!waveforms_ok) printf("  FAIL: one or more waveform shapes wrong\n");
    if (!rate_ok) printf("  FAIL: rate table out of expected real-Hz range\n");
    if (!delay_ok) printf("  FAIL: delay table out of expected real-seconds range\n");
    if (!oscillates_ok) printf("  FAIL: pitch mod didn't swing through both signs near the calibrated max\n");
    if (!amp_swings_ok) printf("  FAIL: amp mod attenuation didn't reach a real depth\n");
    if (!wheel_zero_ok) printf("  FAIL: mod_wheel=0 didn't silence the LFO\n");
    if (!delay_ramp_ok) printf("  FAIL: LFO delay didn't fade in -- full depth immediately\n");
    if (!am_reduces_ok) printf("  FAIL: AM sensitivity didn't reduce a sensitive operator's gain\n");
    if (!am_unaffected_ok) printf("  FAIL: AM affected an operator with am_sensitivity=0\n");
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
    bool ok4 = run_patch_spectrum_check();
    bool ok5 = run_eg_shape_check();
    bool ok6 = run_release_check();
    bool ok9 = run_pitch_eg_check();
    bool ok10 = run_lfo_check();
    // ok3 (level table) and ok8 (key/rate scaling) removed by F3 -- see the
    // comments where those functions used to be.
    bool ok = ok1 && ok2 && ok4 && ok5 && ok6 && ok9 && ok10;
#ifdef T00T_FM_HAS_PATCHES
    bool ok7 = run_patch_bank_render();
    ok = ok && ok7;
#endif
    printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
    return ok ? 0 : 1;
}
