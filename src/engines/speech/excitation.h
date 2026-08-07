#pragma once

#include "osc/noise.h"
#include <cmath>
#include <cstdint>

// Glottal excitation (#28, speech.md "Excitation"): a two-slope triangular
// pulse train -- rising ramp, faster falling ramp -- driven by a bare Q32
// phase accumulator (phase wraps at 2^32 == one glottal period, no
// wavetable). Spectral tilt matters more than pulse shape at this fidelity
// target; a Rosenberg or LF model buys nothing here. No pico-sdk dependency,
// so this is shared by the device engine (audio_engine.cpp) and
// tools/host_render/render_speech.cpp.
//
// jitter/shimmer/vibrato (speech.md P4, #36) below: perfectly periodic
// (jitter == shimmer == 0, lfo_depth == 0) still reproduces "the perfectly
// periodic, unmistakably robotic 1978 sound" from #28/#29 exactly --
// everything here is an additive perturbation on top of the unchanged pulse
// train above, not a replacement for it.

// Fraction of the period spent on the rising ramp. > 0.5 makes the fall
// faster than the rise, which is what gives the pulse train its buzzy,
// harmonically rich spectral tilt (a symmetric triangle would roll off
// faster and sound duller).
inline constexpr float GLOTTAL_OPEN_QUOTIENT = 0.6f;

// Returns a bipolar sample in [-1, 1] for the given phase (0 = start of
// rising ramp, wrapping at 2^32).
inline float glottal_pulse(uint32_t phase) {
    float t = (float)phase * (1.0f / 4294967296.0f);  // 0..1
    float y = (t < GLOTTAL_OPEN_QUOTIENT)
        ? (t / GLOTTAL_OPEN_QUOTIENT)
        : (1.0f - (t - GLOTTAL_OPEN_QUOTIENT) / (1.0f - GLOTTAL_OPEN_QUOTIENT));
    return y * 2.0f - 1.0f;
}

// Q32 phase increment for a glottal period at `freq_hz`, rendered at `fs`
// (Hz). Takes fs as a parameter rather than baking in SPEECH_RATE so this
// stays usable from render.h, which has no engine.h/pico-sdk dependency
// (see render.h's own header comment).
inline uint32_t glottal_phase_inc(float freq_hz, float fs) {
    return (uint32_t)(freq_hz / fs * 4294967296.0);
}

// Vibrato (#36, speech.md "Vibrato LFO"): "at sub-block rate like the
// subtractive engine's after #12" -- one sine sample per sub-block, not per
// glottal cycle and not per output sample. Depth is 0..1 mapping onto
// +/-VIBRATO_MAX_SEMITONES, the same "musical range, not a raw Hz knob"
// choice tract.h's FORMANT_SHIFT_MIN/MAX made for formant_shift.
inline constexpr float VIBRATO_MAX_SEMITONES = 2.0f;

// Live `lfo_rate` CC range (#36, speech.md "CC map"): CC76 (GM's standard
// "Vibrato Rate") maps its 0-127 onto 0..this, same shape as tract.h's
// FORMANT_SHIFT_MIN/MAX. 6 Hz sits at a natural, singable vibrato rate near
// the top of the range rather than in the middle, since most of the
// musically useful range (3-7 Hz) is compressed into roughly the CC's upper
// half; a hard ceiling well past that is still reachable for an
// intentionally-fast tremolo-like effect.
inline constexpr float SPEECH_LFO_RATE_HZ_MAX = 10.0f;

// Advances `lfo_phase` (0..1, wrapped) by however many native frames this
// sub-block actually covers -- not a fixed SPEECH_SUBBLOCK, since the last
// sub-block of a render call can be shorter -- and returns sin(2*pi*phase).
inline float glottal_vibrato_advance(float &lfo_phase, float lfo_rate_hz, uint32_t frames, float fs) {
    lfo_phase += lfo_rate_hz * (float)frames / fs;
    lfo_phase -= floorf(lfo_phase);
    return sinf(lfo_phase * 2.0f * (float)M_PI);
}

// Applies one sub-block's vibrato sample to a nominal glottal phase
// increment: semitone offset -> pitch multiplier, exact same "musical," not
// linear-Hz, mapping every pitched control in this engine uses.
inline uint32_t glottal_vibrato_inc(uint32_t base_inc, float lfo_val, float lfo_depth) {
    float semitones = lfo_val * lfo_depth * VIBRATO_MAX_SEMITONES;
    float mult = exp2f(semitones * (1.0f / 12.0f));
    return (uint32_t)((float)base_inc * mult);
}

// Jitter (#36, speech.md "Jitter and shimmer"): per-cycle randomisation of
// the pitch period, drawn once per glottal cycle -- render.h's inner loop
// detects the cycle boundary itself (phase wraparound) and calls this only
// then, not per sample. Per-sample randomisation would read as FM noise
// riding the pulse train, not the period-to-period wobble that makes a
// voice sound less mechanically perfect; a real glottis can't change its
// period mid-pulse either. jitter == 0 always returns `base_inc` unchanged
// (the LFSR draw is skipped entirely, not just multiplied by zero) so a
// jitter-off render is bit-exact with pre-#36 behaviour.
inline constexpr float JITTER_MAX_FRACTION = 0.06f;  // +/-6% period at jitter=255

inline uint32_t glottal_jitter_inc(uint32_t base_inc, uint8_t jitter, uint16_t &lfsr) {
    if (jitter == 0) return base_inc;
    float r = (float)osc_noise(lfsr) * (1.0f / 32768.0f);  // -1..1
    float frac = r * ((float)jitter / 255.0f) * JITTER_MAX_FRACTION;
    return (uint32_t)((float)base_inc * (1.0f + frac));
}

// Shimmer: the amplitude equivalent of jitter, same per-cycle cadence and
// the same "== 0 skips the draw entirely" contract. Applied as a multiplier
// on the voiced excitation sample, not on `cur_amp` itself, so it perturbs
// only the periodic glottal source and not the gate-declick envelope.
inline constexpr float SHIMMER_MAX_FRACTION = 0.15f;  // +/-15% amplitude at shimmer=255

inline float glottal_shimmer_mult(uint8_t shimmer, uint16_t &lfsr) {
    if (shimmer == 0) return 1.0f;
    float r = (float)osc_noise(lfsr) * (1.0f / 32768.0f);
    float mult = 1.0f + r * ((float)shimmer / 255.0f) * SHIMMER_MAX_FRACTION;
    return mult < 0.0f ? 0.0f : mult;
}
