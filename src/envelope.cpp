#include "envelope.h"
#include <cmath>

// Compute exponential decay coefficient for a given time constant.
// After time_ms, level decays to ~0.001 (-60dB) of its starting value.
// coeff = exp(ln(0.001) / num_samples) ≈ exp(-6.9078 / num_samples)
static float compute_decay_coeff(uint16_t time_ms) {
    if (time_ms == 0) return 0.0f;  // instant decay
    float samples = (float)time_ms * (float)SAMPLE_RATE / 1000.0f;
    return expf(-6.9078f / samples);
}

EnvConfig env_config(uint16_t attack_ms, uint16_t decay_ms,
                     uint16_t sustain_pct, uint16_t release_ms) {
    EnvConfig cfg;

    if (sustain_pct > 100) sustain_pct = 100;
    cfg.sustain_level = (float)sustain_pct / 100.0f;

    // Attack: linear ramp from 0 to 1.0 over attack_ms
    if (attack_ms == 0) {
        cfg.attack_rate = 1.0f;  // instant
    } else {
        float samples = (float)attack_ms * (float)SAMPLE_RATE / 1000.0f;
        cfg.attack_rate = 1.0f / samples;
    }

    // Decay: exponential from 1.0 toward sustain_level
    cfg.decay_coeff = compute_decay_coeff(decay_ms);

    // Release: exponential from sustain_level toward 0
    cfg.release_coeff = compute_decay_coeff(release_ms);

    return cfg;
}
