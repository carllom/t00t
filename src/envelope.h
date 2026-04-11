#pragma once

#include "audio_common.h"
#include <cstdint>

static constexpr int32_t ENV_MAX = 32767;

enum EnvState : uint8_t { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// Pre-computed envelope rates. Shared across voices that use the same shape.
// Separates configuration (Core 0 / global) from per-voice state.
struct EnvConfig {
    int32_t attack_rate;    // per-sample increment during attack (0→peak)
    int32_t decay_rate;     // per-sample decrement during decay (peak→sustain)
    int32_t sustain_level;  // held level while gate is true (0–32767)
    int32_t release_rate;   // per-sample decrement during release (sustain→0)
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
    int32_t level;

    void init() {
        state = ENV_IDLE;
        level = 0;
    }

    // Start attack from zero (new note)
    void trigger() {
        state = ENV_ATTACK;
        level = 0;
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

    // Advance one sample. Returns current level (0–32767).
    inline int32_t advance(const EnvConfig &cfg) {
        switch (state) {
            case ENV_ATTACK:
                level += cfg.attack_rate;
                if (level >= ENV_MAX) {
                    level = ENV_MAX;
                    state = ENV_DECAY;
                }
                break;
            case ENV_DECAY:
                level -= cfg.decay_rate;
                if (level <= cfg.sustain_level) {
                    level = cfg.sustain_level;
                    state = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                break;
            case ENV_RELEASE:
                level -= cfg.release_rate;
                if (level <= 0) {
                    level = 0;
                    state = ENV_IDLE;
                }
                break;
            case ENV_IDLE:
                break;
        }
        return level;
    }
};
