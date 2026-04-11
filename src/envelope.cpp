#include "envelope.h"

// Compute per-sample rate: delta / (time_ms * SAMPLE_RATE / 1000)
// Returns at least 1 if delta > 0, to guarantee progress.
static int32_t compute_rate(int32_t delta, uint16_t time_ms) {
    if (time_ms == 0 || delta <= 0) return delta > 0 ? delta : 0;
    uint32_t samples = (uint32_t)time_ms * SAMPLE_RATE / 1000;
    int32_t rate = delta / (int32_t)samples;
    return rate > 0 ? rate : 1;
}

EnvConfig env_config(uint16_t attack_ms, uint16_t decay_ms,
                     uint16_t sustain_pct, uint16_t release_ms) {
    EnvConfig cfg;

    // Clamp sustain to 100%
    if (sustain_pct > 100) sustain_pct = 100;
    cfg.sustain_level = ENV_MAX * sustain_pct / 100;

    cfg.attack_rate  = compute_rate(ENV_MAX, attack_ms);
    cfg.decay_rate   = compute_rate(ENV_MAX - cfg.sustain_level, decay_ms);
    cfg.release_rate = compute_rate(cfg.sustain_level, release_ms);

    // If sustain is 0 and release_rate ends up 0, force it to avoid stuck voices
    if (cfg.sustain_level == 0 && cfg.release_rate == 0) {
        cfg.release_rate = 1;
    }

    return cfg;
}
