#pragma once

#include <cstdint>

// 808 hand-clap amplitude contour: a few fast, re-struck bursts (~10 ms apart)
// that model the individual hand impacts, followed by a longer decaying tail
// (the room reverberation). Drives a band-passed noise voice. Per-voice state,
// Core 1 only — analogous to Envelope but with the multi-burst shape.
struct ClapEnv {
    float    level;
    uint32_t counter;   // samples since the last burst boundary
    uint8_t  burst;     // bursts elapsed
    bool     active_;

    void init()    { level = 0.0f; counter = 0; burst = 0; active_ = false; }
    void trigger() { level = 1.0f; counter = 0; burst = 0; active_ = true; }
    bool active() const { return active_; }

    // interval:    samples between burst re-strikes
    // num_bursts:  fast re-strikes before the tail begins
    // burst_coeff: per-sample exponential decay during the burst phase
    // tail_coeff:  per-sample exponential decay of the final tail
    inline float advance(uint32_t interval, uint8_t num_bursts,
                         float burst_coeff, float tail_coeff) {
        if (!active_) return 0.0f;
        counter++;
        if (burst < num_bursts) {
            level *= burst_coeff;
            if (counter >= interval) {
                counter = 0;
                burst++;
                level = 1.0f;   // re-strike; the last one starts the tail at full level
            }
        } else {
            level *= tail_coeff;
            if (level < 0.0001f) { level = 0.0f; active_ = false; }
        }
        return level;
    }
};
