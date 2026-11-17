#pragma once

#include "part_slot.h"
#include <cstdint>

// The Core 1 side of one Part: the newest cooked patch the render loop
// reads live parameters from, plus the one it replaced.
//
// take() once per buffer, after latching the voice block. Note-on fields
// never come from here: Core 0 copies them into the voice's own parameters
// at note-on (PatchPlatformT::cooked()), together with the load ID of the
// Patch the note belongs to. A sounding note follows current() only while
// that load ID matches load_id(); take() reports a newly loaded Patch as
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
        uint16_t load_id;
        if (!slot.consume(fresh, load_id)) return PartTake::NONE;
        prev_ = cur_;
        prev_load_ = cur_load_;
        cur_ = fresh;
        cur_load_ = load_id;
        return cur_load_ == prev_load_ ? PartTake::EDIT : PartTake::LOAD;
    }

    const Cooked &current() const { return cur_; }
    uint16_t load_id() const { return cur_load_; }

    // The patch current() replaced at the last take(), and its load ID.
    const Cooked &previous() const { return prev_; }
    uint16_t previous_load_id() const { return prev_load_; }

private:
    Cooked cur_{};
    Cooked prev_{};
    uint16_t cur_load_ = 0;
    uint16_t prev_load_ = 0;
};
