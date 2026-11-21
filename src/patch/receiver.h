#pragma once

#include "part_slot.h"
#include <cstdint>

// The Core 1 side of one Part: the newest cooked patch the render loop
// reads live parameters from, plus the one it replaced.
//
// take() once per buffer, after latching the voice block. A note carries
// the load ID of the Patch it belongs to. Its note-on fields reach it one
// of two ways: Core 0 copies them into the voice's own parameters at
// note-on (PatchPlatformT::cooked()), or, for a module whose note-on
// fields are too large for that, Core 1 copies them from here when the
// note starts (note_patch()). A sounding note follows current() only while
// its load ID matches load_id(); take() reports a newly loaded Patch as
// LOAD so the engine can leave the old Patch's notes on their old live
// values (previous()).

enum class PartTake : uint8_t {
    NONE,  // nothing new published
    EDIT,  // the same Patch, edited
    LOAD,  // a different Patch was loaded
};

template <typename Cooked>
class PartReceiverT {
public:
    PartTake take(PartSlotT<Cooked> &slot) {
        Cooked fresh;
        uint16_t load_id, seq;
        if (!slot.consume(fresh, load_id, seq)) return PartTake::NONE;
        prev_ = cur_;
        prev_load_ = cur_load_;
        cur_ = fresh;
        cur_load_ = load_id;
        cur_seq_ = seq;
        return cur_load_ == prev_load_ ? PartTake::EDIT : PartTake::LOAD;
    }

    // The cooked patch a note of Patch load `load_id`, played with cook
    // `cook_seq` (PatchPlatformT::cook_seq()), starts on: the current cook
    // if it is that cook or a later one of the same Patch, the previous
    // cook if the note belongs to the Patch it replaced; null if the
    // note's cook hasn't arrived yet, so the note waits a buffer.
    const Cooked *note_patch(uint16_t load_id, uint16_t cook_seq) const {
        if (load_id == cur_load_) return seq_after(cook_seq, cur_seq_) ? nullptr : &cur_;
        if (load_id == prev_load_) return &prev_;
        // A load Core 1 hasn't seen yet, or one replaced before it ever arrived.
        return seq_after(load_id, cur_load_) ? nullptr : &cur_;
    }

    const Cooked &current() const { return cur_; }
    uint16_t load_id() const { return cur_load_; }
    uint16_t seq() const { return cur_seq_; }  // the cook sequence number of current()

    // The patch current() replaced at the last take(), and its load ID.
    const Cooked &previous() const { return prev_; }
    uint16_t previous_load_id() const { return prev_load_; }

private:
    // Whether 16-bit counter `a` is later than `b`.
    static bool seq_after(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) > 0; }

    Cooked cur_{};
    Cooked prev_{};
    uint16_t cur_load_ = 0;
    uint16_t prev_load_ = 0;
    uint16_t cur_seq_ = 0;
};
