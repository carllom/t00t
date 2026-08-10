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
// **F6 (fm2.md §5.14) corrected the phase ORIGIN, which that re-expression
// got wrong.** The shapes above were right; where the cycle starts was not.
// Dexed's `keydown()` syncs to `phase_ = (1<<31) - 1` -- the MIDDLE of the
// cycle, not the start -- and its two sawtooth cases carry a compensating
// `^ (1U << 31)`, i.e. a half-cycle rotation baked into the waveform. The
// old code here dropped both halves of that pair: it synced to phase 0 and
// wrote the sawtooths unrotated. Those two errors cancel exactly for the
// sawtooths and for nothing else, which is why F1 measured waveforms 1 and 2
// as correct and waveforms 0, 3 and 4 as *precisely* half a cycle out --
// three "broken waveforms" that were really one wrong constant. Both halves
// are now restored (fm_lfo_trigger() and the two saw cases below), so the
// pair is once again self-consistent AND matches Dexed.
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
// itself integer-only.
//
// **F6 reversed #49's mod-wheel decision.** #49 made the wheel a 0..1
// MULTIPLIER on the patch's configured PMD/AMD, and flagged the consequence
// honestly: "a patch with real vibrato/tremolo configured will sound
// completely flat until the mod wheel is actually moved -- expected, not a
// bug." Measured against the reference, it is a bug: every factory patch
// with configured vibrato played with no vibrato at wheel 0, which is the
// resting position. That is not a small fidelity gap, and it is invisible to
// a scorecard run at wheel 0 -- both sides look "quiet", the way #49's own
// dropped `am_sensitivity` looked like a well-behaved parameter (fm2.md
// §5.13). The wheel now follows Dexed's real `max(pmod_1, pmod_2)` /
// `max(amod_1, amod_2)` rule instead: the patch's own depth always plays,
// and the wheel is a SEPARATE source that takes over once it exceeds the
// patch depth. #49's acceptance criterion ("mod wheel scales LFO depth") is
// still met -- pushing the wheel up still increases vibrato, monotonically,
// from zero-wheel patch depth to full -- so this is a strict superset of the
// old behaviour, not a trade. What is still NOT replicated is Dexed's
// configurable mod MATRIX (which of aftertouch/breath/foot/wheel routes to
// pitch vs amp vs EG, and at what range): the wheel is hardwired to both
// pitch and amp here, matching speech's own CC1 precedent (#36).

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

// Dexed's own `Lfo::reset()` delay-ramp increments (Source/msfa/lfo.cc,
// Apache-2.0), ported as increments-per-SAMPLE rather than per-block so this
// file stays independent of FM_BLOCK the way everything else here is
// (Dexed's own `unit_` folds in its fixed N; dividing that back out leaves
// `25190424 / sample_rate` per sample, and the "constant is 1<<32/15.5s/11"
// comment falls out unchanged).
//
// **F6 (fm2.md §5.14): this used to return a single duration in seconds and
// the caller ramped linearly across it.** That was wrong in shape, not just
// in constants. Dexed's delay is a two-stage accumulator, and the FIRST
// stage is not a ramp at all -- `getdelay()` returns exactly 0 for the whole
// of it (see fm_lfo_delay_step below). So the real curve is *silence, then a
// ramp*, and the old code turned it into one ramp spanning only the silent
// stage's duration: it started opening immediately (when the reference is
// still fully closed) and was fully open at the exact moment the reference
// starts to open. Hence F1's `lfo/delay-99` peaking at 1.00 error, and the
// old comment's claim that stage two is "a comparatively quick tail once the
// LFO has already started becoming audible" being backwards on both counts.
// `a &= 0xff80` looks like it can only shrink `a`, but the `max(0x80, ...)`
// floor means stage two is FASTER for every delay value where the two differ
// (delay 99: a=32 -> a2=128, so 2.66 s closed then 0.67 s opening).
//
// Anchors, now for the full two-stage curve: delay 0 -> instant, delay 50 ->
// 0.31 s closed + 0.08 s opening, delay 99 -> 2.66 s closed + 0.67 s opening.
inline constexpr float FM_LFO_DELAY_UNIT = 25190424.0f / (float)SAMPLE_RATE;

inline void dx7_lfo_delay_incs(int delay_raw, uint32_t &inc1, uint32_t &inc2) {
    int a = 99 - delay_raw;
    if (a >= 99) {  // delay_raw <= 0: no delay, matches Dexed's own a==99 branch
        inc1 = inc2 = ~0u;
        return;
    }
    a = (16 + (a & 15)) << (1 + (a >> 4));
    int a2 = a & 0xff80;
    if (a2 < 0x80) a2 = 0x80;
    inc1 = (uint32_t)(FM_LFO_DELAY_UNIT * (float)a);
    inc2 = (uint32_t)(FM_LFO_DELAY_UNIT * (float)a2);
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
    auto frac = [](float v) { return v - floorf(v); };
    switch (waveform) {
        case 0: return 1.0f - fabsf(2.0f * t - 1.0f);                    // triangle: 0 at t=0/1, peak at t=0.5 (Dexed: x=phase>>7, complemented past the halfway point -- a tent, not a ramp-then-ramp)
        // F6: both sawtooths carry Dexed's own `^ (1U << 31)` -- a half-cycle
        // rotation of the phase, restored here (see this file's header). It
        // pairs with fm_lfo_trigger()'s half-cycle sync point: together they
        // put a freshly key-synced saw at the START of its ramp, which is
        // what the rotation is there for.
        case 1: return 1.0f - frac(t + 0.5f);                             // saw down (Dexed: (~phase ^ sign) >> 8)
        case 2: return frac(t + 0.5f);                                    // saw up   (Dexed: (phase ^ sign) >> 8)
        case 3: return t < 0.5f ? 1.0f : 0.0f;                            // square: 1 for the first half-cycle, 0 for the second (Dexed: (~phase>>7)&bit, set exactly while phase's top bit is clear)
        case 4: return 0.5f + 0.5f * sinf(2.0f * (float)M_PI * t);        // sine: Dexed's own "(1<<23) + (Sin::lookup(...)>>1)" is exactly center + half-amplitude sine, i.e. this
        default: return 0.5f;  // unreachable for waveform 5 -- see fm_lfo_step_block()
    }
}

struct FmLfo {
    uint32_t phase;          // Q32, one waveform cycle = 2^32 (same convention as FmOp::phase, op.h)
    uint32_t delay_state;    // Dexed's `delaystate_`: Q32 accumulator, closed below 2^31, ramping above
    float    delay_progress; // this block's delay output, 0 (closed) .. 1 (fully open) -- derived from delay_state
    uint8_t  sh_state;       // sample & hold PRNG state (Dexed's own `randstate_ * 179 + 17` recurrence)
    float    sh_value;       // last sample & hold output, held between wrap events, 0..1
};

// Matches env_dx_init()'s role: the power-on/never-triggered resting state.
// The delay accumulator starts saturated (fully open) rather than at 0 --
// harmless either way since fm_lfo_trigger() always runs before a real note
// reads this, but "fully open" is the safer resting value if anything ever
// reads a never-triggered voice's LFO.
inline void fm_lfo_init(FmLfo &lfo) {
    lfo.phase = 0;
    lfo.delay_state = ~0u;
    lfo.delay_progress = 1.0f;
    lfo.sh_state = 0x5A;
    lfo.sh_value = 0.0f;
}

// key_sync resets phase to Dexed's own `keydown()` sync point, `(1<<31) - 1`
// -- the MIDDLE of the cycle, not the start. See this file's header for why
// that is not an off-by-one: it is half of a two-part convention whose other
// half is the sawtooths' half-cycle rotation, and F6 restored both together.
// The delay accumulator always restarts at 0 regardless of key_sync (matches
// Dexed's own `delaystate_ = 0`, which sits outside the `if (sync_)`) -- a
// fresh note always re-triggers the delay.
inline void fm_lfo_trigger(FmLfo &lfo, bool key_sync) {
    if (key_sync) lfo.phase = (1u << 31) - 1;
    lfo.delay_state = 0;
    lfo.delay_progress = 0.0f;
}

// Dexed's `Lfo::getdelay()`, ported whole (Source/msfa/lfo.cc, Apache-2.0)
// -- the two-stage accumulator F6 found the old float ramp had flattened.
// Kept as the real integer accumulator rather than re-expressed in seconds
// because the shape *is* the arithmetic here: the "closed" stage is
// `delaystate_ < 2^31` and the ramp is literally the accumulator's own top
// bits, so there is no seconds-domain formula to re-derive that would not
// just be this with extra steps. Advances by `n` samples and returns 0..1.
inline float fm_lfo_delay_step(FmLfo &lfo, const FmLfoParams &p, uint32_t n) {
    uint32_t inc1, inc2;
    dx7_lfo_delay_incs(p.delay, inc1, inc2);
    // Below the halfway point the first (slower) increment applies, above it
    // the second -- Dexed picks per call, on the state as it stands.
    uint32_t delta = (lfo.delay_state < (1u << 31)) ? inc1 : inc2;
    uint64_t d = (uint64_t)lfo.delay_state + (uint64_t)delta * (uint64_t)n;
    if (d > 0xFFFFFFFFull) return 1.0f;  // saturated: Dexed leaves delaystate_ untouched, so this latches
    lfo.delay_state = (uint32_t)d;
    if (d < (1u << 31)) return 0.0f;     // still fully closed -- NOT a ramp from zero
    return (float)((uint32_t)(d >> 7) & ((1u << 24) - 1)) * (1.0f / 16777216.0f);
}

// Dexed's amplitude-mod depth curve (`Dx7Note::compute()`'s "AMP MOD"
// section, Apache-2.0), collapsed to a single float function of one
// argument. Dexed computes `pt = exp(sensamp/262144 * 0.07 + 12.2)` and then
// `level -= level * pt >> 24`, so the whole thing is a multiplicative
// scaling of the operator's LOG-domain envelope level by
// `exp(4.48*x + 12.2) / 2^24`, where x is the mod amount already weighted by
// the operator's own AM sensitivity (0..1). Substituting sensamp = x * 2^24
// makes the 262144 and the >>24 cancel into that single constant 4.48.
//
// Two consequences worth knowing, both faithful to the reference:
//   - at x = 0 the factor is not 0 but ~0.0119, i.e. an operator with AMS > 0
//     is attenuated by ~1 dB at full envelope even with AMD = 0. That is why
//     Dexed guards on `ampmodsens_[op] != 0` and not on the mod amount.
//   - the attenuation is proportional to the CURRENT envelope level, so a
//     decayed operator is tremolo'd less in dB than a loud one.
// The second is physically odd, and Dexed's own comment on this block reads
// "TODO: mehhh.. this needs some real tuning." It is ported anyway because it
// is the reference this engine is being measured against; fm2.md §5.14 flags
// it as the one place where the reference is self-admittedly approximate, and
// therefore the first thing to revisit if a hardware listen disagrees.
inline float dx7_am_level_factor(float x) {
    return expf(4.48f * x + 12.2f) * (1.0f / 16777216.0f);
}

// Advances by one control block of `n` samples; returns this block's pitch
// modulation (signed cents, folds into pitch_eg.h's own output before
// scaling operator increments) and amplitude modulation (0..1, before each
// operator's own AM sensitivity weights it and dx7_am_level_factor() turns
// it into a log-domain attenuation -- op.h's fm_voice_step_envelopes()).
// `mod_wheel_frac` (0..1, from VoiceParams::mod_wheel) is a SEPARATE
// modulation source that competes with the patch's own depth via Dexed's
// max() rule, not a multiplier on it -- see this file's header.
inline void fm_lfo_step_block(FmLfo &lfo, const FmLfoParams &p, uint32_t n, float mod_wheel_frac,
                               float &pitch_cents_out, float &amp_mod_out) {
    uint32_t inc = fm_phase_inc(dx7_lfo_rate_to_hz(p.rate));
    uint32_t old_phase = lfo.phase;
    lfo.phase += inc * n;  // n*inc is always << 2^32 at real LFO rates (max ~51 Hz), so at most one wrap per block

    float unipolar;
    if (p.waveform == 5) {
        if (lfo.phase < old_phase) {  // wrapped this block -> regenerate (Dexed's own "phase_ < delta_" check, generalized from per-sample to per-block)
            lfo.sh_state = (uint8_t)(lfo.sh_state * 179 + 17);  // Dexed's own PRNG recurrence, ported verbatim
            // Dexed returns `((randstate_ ^ 0x80) + 1) << 16`, i.e. 1/256..1,
            // never 0 -- the +1 and the /256 both matter for the range to match.
            lfo.sh_value = (float)((lfo.sh_state ^ 0x80) + 1) * (1.0f / 256.0f);
        }
        unipolar = lfo.sh_value;
    } else {
        unipolar = fm_lfo_waveform_unipolar(lfo.phase, p.waveform);
    }

    lfo.delay_progress = fm_lfo_delay_step(lfo, p, n);

    float bipolar = 2.0f * unipolar - 1.0f;  // pitch signal, -1..1
    float amp_signal = 1.0f - unipolar;      // tremolo dip signal, 0..1 (matches Dexed's own lfo_val=(1<<24)-lfo_val inversion)

    // Both mod sources are expressed on Dexed's own raw 0..255 depth scale so
    // its max() can be taken directly. The wheel maps to 2*CC (0..254): that
    // factor of two is not a fudge, it is the ratio between Dexed's two shift
    // counts -- `pmod_1 = (depth*delay) * senslfo >> 39` against
    // `pmod_2 = wheel * senslfo >> 14`, and identically >>16 against >>17 on
    // the amp side. Both are exact because the LFO value factors out of the
    // max() (it multiplies both terms), so comparing depths alone is the same
    // comparison Dexed makes on the products.
    float wheel_units = 254.0f * mod_wheel_frac;

    int pmd_scaled = (p.pmd * 165) >> 6;     // ported bit-shift, Dexed's own pitchmoddepth_ derivation
    float pitch_units = (float)pmd_scaled * lfo.delay_progress;
    if (wheel_units > pitch_units) pitch_units = wheel_units;
    float pitch_sens_frac = (float)DX7_PITCH_MOD_SENS[p.pms & 7] / 255.0f;
    pitch_cents_out = FM_LFO_PMD_MAX_CENTS * (pitch_units / 255.0f) * pitch_sens_frac * bipolar;

    int amd_scaled = (p.amd * 165) >> 6;     // same ported bit-shift, Dexed's own ampmoddepth_ derivation
    float amp_units = (float)amd_scaled * lfo.delay_progress;
    if (wheel_units > amp_units) amp_units = wheel_units;
    // 256 here where the pitch side has 255: Dexed's amp path loses one more
    // bit than its pitch path (`>>8` then `>>24` against a single `>>39`), so
    // full AMD reaches 255/256 of the scale, not 255/255. Deliberate, not a
    // copy-paste slip -- F6 measured it.
    amp_mod_out = (amp_units / 256.0f) * amp_signal;
}
