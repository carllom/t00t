#pragma once

#include "audio_common.h"
#include <cstdint>

enum EnvState : uint8_t { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// Envelope configuration. Shared across voices that use the same shape.
// Attack is linear (additive per sample), decay/release are exponential
// (multiplicative per sample) for natural amplitude decay.
struct EnvConfig {
    float attack_rate;    // per-sample increment during attack (0→1.0)
    float decay_coeff;    // per-sample multiplier during decay (< 1.0)
    float sustain_level;  // held level while gate is true (0.0–1.0)
    float release_coeff;  // per-sample multiplier during release (< 1.0)
};

// Build an EnvConfig from human-readable parameters.
//   attack_ms:   time from 0 to peak (0 = instant)
//   decay_ms:    time from peak to sustain level (0 = instant)
//   sustain_pct: sustain level 0–100 (percentage of full scale)
//   release_ms:  time from sustain level to 0 (0 = instant)
EnvConfig env_config(uint16_t attack_ms, uint16_t decay_ms,
                     uint16_t sustain_pct, uint16_t release_ms);

// Per-voice envelope state. Lives on Core 1 only.
struct Envelope {
    EnvState state;
    float level;

    void init() {
        state = ENV_IDLE;
        level = 0.0f;
    }

    // Start attack from zero (new note)
    void trigger() {
        state = ENV_ATTACK;
        level = 0.0f;
    }

    // Transition to release from current level
    void release() {
        if (state != ENV_IDLE) {
            state = ENV_RELEASE;
        }
    }

    bool active() const {
        return state != ENV_IDLE;
    }

    // Advance one sample. Returns current level (0.0–1.0).
    inline float advance(const EnvConfig &cfg) {
        switch (state) {
            case ENV_ATTACK:
                level += cfg.attack_rate;
                if (level >= 1.0f) {
                    level = 1.0f;
                    state = ENV_DECAY;
                }
                break;
            case ENV_DECAY:
                level = cfg.sustain_level + (level - cfg.sustain_level) * cfg.decay_coeff;
                if (level - cfg.sustain_level < 0.0001f) {
                    level = cfg.sustain_level;
                    state = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                break;
            case ENV_RELEASE:
                level *= cfg.release_coeff;
                if (level < 0.0001f) {
                    level = 0.0f;
                    state = ENV_IDLE;
                }
                break;
            case ENV_IDLE:
                break;
        }
        return level;
    }
};
