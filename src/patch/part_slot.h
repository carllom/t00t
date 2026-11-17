#pragma once

#include <atomic>
#include <cstdint>

// One Part's cooked-patch slot: a double buffer that hands whole cooked
// patches from Core 0 to Core 1 without locks and without tearing.
//
// `state_` packs a publish generation with the index of the buffer that
// generation lives in; Core 1 copies that buffer out and acknowledges by
// echoing `state_` into `ack_`. Core 0 only writes the spare buffer while
// the latest publish is acknowledged, so it never writes a buffer Core 1
// might still be copying; until then try_publish() refuses and the caller
// retries later.
//
// Each publish has a generation number (1, 2, ...) both sides can read, so a
// consumer can tell which publish a piece of Core 0 state was derived from.
template <typename Cooked>
class PartSlotT {
public:
    // Core 0. False if Core 1 hasn't taken the previous publish yet;
    // otherwise publishes and stores its generation in `*generation`.
    bool try_publish(const Cooked &cooked, uint32_t *generation = nullptr) {
        uint32_t s = state_.load(std::memory_order_relaxed);
        if (ack_.load(std::memory_order_acquire) != s) return false;
        uint32_t idx = (s & 1u) ^ 1u;
        uint32_t next = (((s >> 1) + 1u) << 1) | idx;
        buf_[idx] = cooked;
        state_.store(next, std::memory_order_release);
        if (generation) *generation = next >> 1;
        return true;
    }

    // Core 1, once per buffer. Copies the newest published patch into `out`,
    // stores its generation in `*generation` and returns true, or returns
    // false if nothing new was published.
    bool consume(Cooked &out, uint32_t *generation = nullptr) {
        uint32_t s = state_.load(std::memory_order_acquire);
        if (s == ack_.load(std::memory_order_relaxed)) return false;
        out = buf_[s & 1u];
        ack_.store(s, std::memory_order_release);
        if (generation) *generation = s >> 1;
        return true;
    }

private:
    Cooked buf_[2]{};
    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> ack_{0};
};
