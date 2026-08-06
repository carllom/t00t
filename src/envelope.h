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

    // Advance by `n` samples in one O(1) step, for sub-block-rate rendering:
    // the caller linearly ramps every intermediate sample between the level
    // before this call and the value returned, rather than calling advance()
    // n times (see the subtractive engine's sub-block rendering, issue #12).
    // Attack is linear so this is exact; decay/release need the caller to
    // pass the n-sample-ahead coefficient (coeff^n), precomputed once since
    // n is fixed — recomputing pow() here every call would defeat the point.
    // A state transition that would land mid-block (e.g. attack completing)
    // is deferred to the next block boundary instead of the exact sample;
    // bounded by one sub-block's length (~1.45 ms at n=64), inaudible.
    inline float advance_block(const EnvConfig &cfg, float decay_coeff_n,
                                float release_coeff_n, uint32_t n) {
        switch (state) {
            case ENV_ATTACK:
                level += cfg.attack_rate * (float)n;
                if (level >= 1.0f) {
                    level = 1.0f;
                    state = ENV_DECAY;
                }
                break;
            case ENV_DECAY:
                level = cfg.sustain_level + (level - cfg.sustain_level) * decay_coeff_n;
                if (level - cfg.sustain_level < 0.0001f) {
                    level = cfg.sustain_level;
                    state = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                break;
            case ENV_RELEASE:
                level *= release_coeff_n;
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
