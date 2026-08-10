#pragma once

#include "audio_common.h"
#include "patch.h"
#include "sine_tab.h"
#include <cmath>
#include <cstdint>

// FmLfo -- the DX7 voice LFO (#49, fm.md §5.5): rate/delay/waveform/PMD/AMD/
// key-sync, evaluated once per control block (same cadence as env_dx.h's
// EnvDX and this engine's own pitch_eg.h). Per-sample cost: zero -- nothing
// here ever touches op_render/op_render_first/op_render_fb (op.h) or the
// kernel's `gain`/`gain_step` fields directly; op.h's
// fm_voice_step_pitch_and_mod() folds this file's two outputs (a signed
// cents value and a 0..1 tremolo-attenuation fraction) into the same
// increment-scaling and gain-multiply op.h already does for pitch_eg.h and
// env_dx.h respectively.
//
// #49 also closes fm.md open question 5 (global vs. per-voice LFO):
// **per-voice, no global-phase mode** -- decided, not deferred. DX7
// hardware has exactly one physical LFO shared by the whole instrument;
// fm.md's own §5.5 already recommended per-voice with key-sync as strictly
// better for polyphony, keeping a patch flag for global-phase "where DX7
// fidelity on specific patches matters" as an option. That option is
// dropped here: #48 already made this engine genuinely multitimbral (one
// patch pointer per voice, fm.md §6.3), and a literal single shared LFO has
// no principled behavior once two simultaneously-active voices request
// global phase with two different patches' rates -- a case that cannot
// arise on real single-timbral hardware at all, so there is no "real DX7
// behavior" to fall back on for it. Per-voice-with-key-sync already gives
// every note struck at the same instant identical LFO phase (the common
// "block chord" case DX7 fidelity actually cares about); the remaining gap
// -- notes of the SAME patch struck at different times drifting slightly
// out of phase with each other -- is a small, patch-dependent effect, not
// worth the architectural ambiguity above. Revisit only if #53's real
// Dexed-diff work finds this is actually audible on a reference bank.
//
// Waveform math is NOT a bit-exact port of Dexed's own `Lfo::getsample()`
// (Source/msfa/lfo.cc, Apache-2.0) -- that function's bit tricks
// (`phase_>>7`, `phase_>>8` into `Sin::lookup`, etc.) are tuned to Dexed's
// own internal phase/table Q-format, not a property of the DX7's real
// waveform *shapes* that needs replicating for this engine to sound right.
// Reasoned through by hand against Dexed's real bit operations (documented
// per-waveform below) and re-expressed against this file's own phase
// convention (uint32_t Q32, one cycle = 2^32 -- same convention as op.h's
// own FmOp::phase) in plain float, since this all runs at control rate
// (~2756 Hz at BLOCK=16, not per audio sample) where float cost is
// negligible and every other block-rate/note-on computation in this engine
// (fm_op_inc) already uses float freely.
//
// Rate table (`lfoSource`) IS ported verbatim (real hardware-calibrated
// data, same "port the table, don't re-derive" rule pitch_eg.h's own tables
// follow) -- see dx7_lfo_rate_to_hz()'s own comment for the real-Hz
// derivation and calibration anchors (rate 0 ~0.065 Hz / ~15.5s period,
// rate 99 ~50.9 Hz, both matching commonly cited real DX7 LFO figures).
//
// PMD/PMS/AMD combination IS numerically equivalent to Dexed's real
// `Dx7Note::compute()` "PITCH"/"AMP MOD" sections (Apache-2.0) for the
// LFO-driven term (`pmod_1`/`amod_1`) -- the real formula
// `(pmd*lfo_delay * pms*(lfo_val-center)) >> 39` is exactly four
// independent multiplicative factors (pmd, pms, delay, and the centered
// LFO value) with no cross-term, so re-deriving it as plain fractions times
// a single calibration constant (`FM_LFO_PMD_MAX_CENTS`, derived below and
// cross-checked against a standalone calibration harness running Dexed's
// real integer formula) is exact, not approximate, just without the
// Q24/Q32/`>>39` bit-shift plumbing that was only ever there to keep Dexed
// itself integer-only. Dexed's SEPARATE non-LFO controller-driven term
// (`pmod_2`/`ctrls->pitch_mod`, a JUCE-plugin-configurable "aftertouch/
// breath/wheel targets pitch directly, independent of LFO oscillation"
// mod-matrix feature) is NOT replicated -- fm.md's own acceptance criterion
// ("mod wheel scales LFO depth") asks for the much simpler, extremely
// common convention this file implements instead: mod wheel multiplies the
// LFO's own configured PMD/AMD depth (0 = no vibrato/tremolo regardless of
// patch data, matching speech's own CC1 "mod wheel -> vibrato depth"
// precedent, #36/speech's midi_controller.cpp) -- NOT real DX7 hardware's
// own convention, where a patch's configured PMD/PMS plays at full depth
// regardless of wheel position and the wheel is a separate, additive,
// globally-assignable modulation source. Worth flagging on a first
// hardware listen: a patch with real vibrato/tremolo configured will sound
// completely flat until the mod wheel is actually moved -- expected, not a
// bug.

// Dexed's `lfoSource` (Source/msfa/lfo.cc, Apache-2.0), ported verbatim --
// 100 raw values (index 0-99), converted to real Hz by
// dx7_lfo_rate_to_hz() below.
inline constexpr float DX7_LFO_RATE_SOURCE[100] = {
    0.062541f, 0.125031f, 0.312393f, 0.437120f, 0.624610f,
    0.750694f, 0.936330f, 1.125302f, 1.249609f, 1.436782f,
    1.560915f, 1.752081f, 1.875117f, 2.062494f, 2.247191f,
    2.374451f, 2.560492f, 2.686728f, 2.873976f, 2.998950f,
    3.188013f, 3.369840f, 3.500175f, 3.682224f, 3.812065f,
    4.000800f, 4.186202f, 4.310716f, 4.501260f, 4.623209f,
    4.814636f, 4.930480f, 5.121901f, 5.315191f, 5.434783f,
    5.617346f, 5.750431f, 5.946717f, 6.062811f, 6.248438f,
    6.431695f, 6.564264f, 6.749460f, 6.868132f, 7.052186f,
    7.250580f, 7.375719f, 7.556294f, 7.687577f, 7.877738f,
    7.993605f, 8.181967f, 8.372405f, 8.504848f, 8.685079f,
    8.810573f, 8.986341f, 9.122423f, 9.300595f, 9.500285f,
    9.607994f, 9.798158f, 9.950249f, 10.117361f, 11.251125f,
    11.384335f, 12.562814f, 13.676149f, 13.904338f, 15.092062f,
    16.366612f, 16.638935f, 17.869907f, 19.193858f, 19.425019f,
    20.833333f, 21.034918f, 22.502250f, 24.003841f, 24.260068f,
    25.746653f, 27.173913f, 27.578599f, 29.052876f, 30.693677f,
    31.191516f, 32.658393f, 34.317090f, 34.674064f, 36.416606f,
    38.197097f, 38.550501f, 40.387722f, 40.749796f, 42.625746f,
    44.326241f, 44.883303f, 46.772685f, 48.590865f, 49.261084f
};

// Dexed's own `Lfo::init()` derives Hz from `lfoSource[rate] * lforatio_`
// where `lforatio_ = 4437500000.0 * N / sample_rate` is a Q32-per-Dexed-
// block phase increment; since Dexed's `Lfo::getsample()` is called once
// per its own N-sample block, real Hz cancels N and sample_rate entirely --
// real_Hz = lfoSource[rate] * 4437500000.0 / 2^32. Cross-checked against
// commonly cited real DX7 figures: rate 0 -> 0.0646 Hz (~15.5s period),
// rate 99 -> 50.9 Hz -- both match.
inline float dx7_lfo_rate_to_hz(int rate) {
    return DX7_LFO_RATE_SOURCE[rate] * (4437500000.0f / 4294967296.0f);
}

// Dexed's `pitchmodsenstab` (Source/msfa/dx7note.cc, Apache-2.0), ported
// verbatim -- PMS (pitch mod sensitivity) is 0-7, unlike AMS below (0-3).
inline constexpr uint8_t DX7_PITCH_MOD_SENS[8] = { 0, 10, 20, 33, 55, 92, 153, 255 };

// Dexed's `ampmodsenstab` (Source/msfa/dx7note.cc, Apache-2.0), Q24
// fractions re-expressed as plain 0..1 floats (AMS is a 2-bit DX7 field,
// 0-3) -- op.h's fm_voice_step_envelopes() multiplies this directly against
// each operator's own gain.
inline constexpr float DX7_AMP_MOD_SENS[4] = {
    0.0f, 4342338.0f / 16777216.0f, 7171437.0f / 16777216.0f, 1.0f
};

// Calibration constant: the real cents deviation Dexed's own LFO-driven
// pitch-mod term (`pmod_1` in `Dx7Note::compute()`) reaches at maximum
// settings (PMD=99, PMS=7, LFO at a waveform extreme, delay fully open).
// `(pmd*lfo_delay * pms*(lfo_val-center)) >> 39` collapses (both raw-scale
// factors at their own max, 255) to `1200 * (255*255*256) / 2^24` cents --
// exact given the real formula's four-independent-factors shape (see this
// file's header comment), not a fit. Cross-checked by running the real
// integer formula in a standalone calibration harness: 16,646,400 raw Q24-
// octave units at PMD=99/PMS=7/lfo_val=0, matching this expression's own
// 255*255*256 = 16,646,400 exactly. 1190.6 cents ~= just under an octave --
// a real, dramatic vibrato depth at the DX7's own extreme settings, not a
// subtle one; ordinary vibrato patches use far less than PMD=99/PMS=7.
inline constexpr float FM_LFO_PMD_MAX_CENTS = 1200.0f * (float)(255 * 255 * 256) / (float)(1u << 24);

// Dexed's own `Lfo::reset()` delay-ramp derivation (Source/msfa/lfo.cc,
// Apache-2.0), reduced to real seconds-to-fully-open the same way
// pitch_eg.h's rate table was: `unit_ = N*2^24/(sample_rate*"21.3s"*11)`
// cancels N/sample_rate (comment: "constant is 1<<32/15.5s/11") the same
// way pitch_eg.h's own rate constant did, leaving a pure function of the
// raw 0-99 delay parameter. Only the DOMINANT first-stage ramp is kept
// (Dexed's own delay envelope is a two-stage curve, a fast fade-in after a
// slower "not yet audible" stage; the second stage is a comparatively quick
// tail once the LFO has already started becoming audible) -- a deliberate
// simplification, not a fidelity claim, chosen because the two-stage
// accumulator only exists to serve Dexed's own per-block Q32 arithmetic,
// not because the perceptual delay curve genuinely needs two pieces.
// Cross-checked anchors: delay 0 -> instant (no ramp), delay 99 -> ~2.66s,
// delay 50 -> ~0.31s -- all match commonly cited real DX7 LFO delay figures.
inline float dx7_lfo_delay_seconds(int delay_raw) {
    int a = 99 - delay_raw;
    if (a >= 99) return 0.0f;  // delay_raw <= 0: no delay, matches Dexed's own a==99 branch
    a = (16 + (a & 15)) << (1 + (a >> 4));
    return 2147483648.0f / (25190424.0f * (float)a);
}

// Canonical unipolar (0..1) waveform sample from a Q32 phase -- reasoned
// through against Dexed's real per-waveform bit tricks and cross-checked
// shape-by-shape (see each case below), but expressed against this file's
// own phase convention rather than replicating Dexed's own >>7/>>8 shifts,
// which are calibrated to Dexed's internal table/Q-format, not the DX7's
// real waveform shape. Waveform 5 (sample & hold) needs mutable state
// (last-held value, wrap detection) a pure function of `phase` alone can't
// carry -- handled directly in fm_lfo_step_block() instead.
inline float fm_lfo_waveform_unipolar(uint32_t phase, uint8_t waveform) {
    float t = (float)phase * (1.0f / 4294967296.0f);
    switch (waveform) {
        case 0: return 1.0f - fabsf(2.0f * t - 1.0f);                    // triangle: 0 at t=0/1, peak at t=0.5 (Dexed: x=phase>>7, complemented past the halfway point -- a tent, not a ramp-then-ramp)
        case 1: return 1.0f - t;                                          // saw down: 1 at t=0, ramps to 0, resets (Dexed: (~phase^sign)>>8, a falling ramp)
        case 2: return t;                                                 // saw up: 0 at t=0, ramps to 1, resets (Dexed: (phase^sign)>>8, a rising ramp)
        case 3: return t < 0.5f ? 1.0f : 0.0f;                            // square: 1 for the first half-cycle, 0 for the second (Dexed: (~phase>>7)&bit, set exactly while phase's top bit is clear)
        case 4: return 0.5f + 0.5f * sinf(2.0f * (float)M_PI * t);        // sine: Dexed's own "(1<<23) + (Sin::lookup(...)>>1)" is exactly center + half-amplitude sine, i.e. this
        default: return 0.5f;  // unreachable for waveform 5 -- see fm_lfo_step_block()
    }
}

struct FmLfo {
    uint32_t phase;          // Q32, one waveform cycle = 2^32 (same convention as FmOp::phase, op.h)
    float    delay_progress; // 0 (just triggered) .. 1 (fully open)
    uint8_t  sh_state;       // sample & hold PRNG state (Dexed's own `randstate_ * 179 + 17` recurrence)
    float    sh_value;       // last sample & hold output, held between wrap events, 0..1
};

// Matches env_dx_init()'s role: the power-on/never-triggered resting state.
// delay_progress starts at 1 (fully open) rather than 0 -- harmless either
// way since fm_lfo_trigger() always runs before a real note reads this, but
// "fully open" is the safer resting value if anything ever reads a
// never-triggered voice's LFO.
inline void fm_lfo_init(FmLfo &lfo) {
    lfo.phase = 0;
    lfo.delay_progress = 1.0f;
    lfo.sh_state = 0x5A;
    lfo.sh_value = 0.0f;
}

// key_sync resets phase to a fixed start-of-cycle point -- matches DX7's
// real per-note LFO restart. delay_progress always restarts at 0 regardless
// of key_sync (matches Dexed's own `keydown()`: `delaystate_ = 0`
// unconditionally) -- a fresh note always re-triggers the fade-in.
inline void fm_lfo_trigger(FmLfo &lfo, bool key_sync) {
    if (key_sync) lfo.phase = 0;
    lfo.delay_progress = 0.0f;
}

// Advances by one control block of `n` samples; returns this block's pitch
// modulation (signed cents, folds into pitch_eg.h's own output before
// scaling operator increments) and amplitude modulation (0..1 tremolo
// attenuation fraction, before each operator's own AM sensitivity weights
// it -- op.h's fm_voice_step_envelopes()). `mod_wheel_frac` (0..1, from
// VoiceParams::mod_wheel) multiplies BOTH -- see this file's header comment
// for why that's a deliberate simplification of real DX7 wheel behavior.
inline void fm_lfo_step_block(FmLfo &lfo, const FmLfoParams &p, uint32_t n, float mod_wheel_frac,
                               float &pitch_cents_out, float &amp_atten_out) {
    uint32_t inc = fm_phase_inc(dx7_lfo_rate_to_hz(p.rate));
    uint32_t old_phase = lfo.phase;
    lfo.phase += inc * n;  // n*inc is always << 2^32 at real LFO rates (max ~51 Hz), so at most one wrap per block

    float unipolar;
    if (p.waveform == 5) {
        if (lfo.phase < old_phase) {  // wrapped this block -> regenerate (Dexed's own "phase_ < delta_" check, generalized from per-sample to per-block)
            lfo.sh_state = (uint8_t)(lfo.sh_state * 179 + 17);  // Dexed's own PRNG recurrence, ported verbatim
            lfo.sh_value = (float)(lfo.sh_state ^ 0x80) / 255.0f;
        }
        unipolar = lfo.sh_value;
    } else {
        unipolar = fm_lfo_waveform_unipolar(lfo.phase, p.waveform);
    }

    float delay_secs = dx7_lfo_delay_seconds(p.delay);
    if (delay_secs <= 0.0f) {
        lfo.delay_progress = 1.0f;
    } else {
        lfo.delay_progress += (float)n / (SAMPLE_RATE * delay_secs);
        if (lfo.delay_progress > 1.0f) lfo.delay_progress = 1.0f;
    }

    float bipolar = 2.0f * unipolar - 1.0f;  // pitch signal, -1..1
    float amp_signal = 1.0f - unipolar;      // tremolo dip signal, 0..1 (matches Dexed's own lfo_val=(1<<24)-lfo_val inversion)

    int pmd_scaled = (p.pmd * 165) >> 6;     // ported bit-shift, Dexed's own pitchmoddepth_ derivation
    float pitch_depth_frac = (float)pmd_scaled / 255.0f;
    float pitch_sens_frac = (float)DX7_PITCH_MOD_SENS[p.pms & 7] / 255.0f;
    pitch_cents_out = FM_LFO_PMD_MAX_CENTS * pitch_depth_frac * pitch_sens_frac
                     * lfo.delay_progress * bipolar * mod_wheel_frac;

    int amd_scaled = (p.amd * 165) >> 6;     // same ported bit-shift, Dexed's own ampmoddepth_ derivation
    float amp_depth_frac = (float)amd_scaled / 255.0f;
    amp_atten_out = amp_depth_frac * lfo.delay_progress * amp_signal * mod_wheel_frac;
}
