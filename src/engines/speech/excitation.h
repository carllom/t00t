#pragma once

#include <cstdint>

// Glottal excitation (#28, speech.md "Excitation"): a two-slope triangular
// pulse train -- rising ramp, faster falling ramp -- driven by a bare Q32
// phase accumulator (phase wraps at 2^32 == one glottal period, no
// wavetable). Spectral tilt matters more than pulse shape at this fidelity
// target; a Rosenberg or LF model buys nothing here. No pico-sdk dependency,
// so this is shared by the device engine (audio_engine.cpp) and
// tools/host_render/render_speech.cpp.
//
// jitter/shimmer (speech.md) are deliberately absent from this slice --
// perfectly periodic is "the perfectly periodic, unmistakably robotic 1978
// sound," and adding randomisation is P4 work.

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
