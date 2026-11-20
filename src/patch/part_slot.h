#pragma once

#include <atomic>
#include <cstdint>

// One Part's cooked-patch slot: a double buffer that hands whole cooked
// patches from Core 0 to Core 1 without locks and without tearing.
//
// `state_` packs a publish count with the index of the buffer that publish
// lives in; Core 1 copies that buffer out and acknowledges by echoing
// `state_` into `ack_`. Core 0 only writes the spare buffer while the
// latest publish is acknowledged, so it never writes a buffer Core 1 might
// still be copying; until then try_publish() refuses and the caller retries
// later.
//
// Each publish carries a load ID that changes only when a different Patch
// is loaded, so Core 1 can tell a new Patch from an edit of the same one,
// and a cook sequence number that changes on every cook, so Core 1 can
// tell whether a note's cook has arrived yet.
template <typename Cooked>
class PartSlotT {
public:
    // Core 0. False if Core 1 hasn't taken the previous publish yet.
    bool try_publish(const Cooked &cooked, uint16_t load_id, uint16_t seq = 0) {
        uint32_t s = state_.load(std::memory_order_relaxed);
        if (ack_.load(std::memory_order_acquire) != s) return false;
        uint32_t idx = (s & 1u) ^ 1u;
        buf_[idx] = cooked;
        load_[idx] = load_id;
        seq_[idx] = seq;
        state_.store((((s >> 1) + 1u) << 1) | idx, std::memory_order_release);
        return true;
    }

    // Core 1, once per buffer. Copies the newest published patch into `out`
    // and its load ID and cook sequence number into `load_id` and `seq`, and
    // returns true; or returns false if nothing new was published.
    bool consume(Cooked &out, uint16_t &load_id, uint16_t &seq) {
        uint32_t s = state_.load(std::memory_order_acquire);
        if (s == ack_.load(std::memory_order_relaxed)) return false;
        out = buf_[s & 1u];
        load_id = load_[s & 1u];
        seq = seq_[s & 1u];
        ack_.store(s, std::memory_order_release);
        return true;
    }

    bool consume(Cooked &out, uint16_t &load_id) {
        uint16_t seq;
        return consume(out, load_id, seq);
    }

    bool consume(Cooked &out) {
        uint16_t load_id;
        return consume(out, load_id);
    }

private:
    Cooked buf_[2]{};
    uint16_t load_[2]{};
    uint16_t seq_[2]{};
    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> ack_{0};
};
