#pragma once

#include "multi.h"
#include "name_editor.h"
#include "wslcd/page.h"
#include <cstdint>

// The MULTI Page's interaction: selecting (loading) and storing Multis
// from the device -- the only way besides SysEx a Multi changes as a
// whole. Module-agnostic; rendering is the module's (multi_page.h).
//
//   BROWSE       +/- and long press are left to Page navigation (not
//                claimed); short press -> SELECT, on LOAD.
//   SELECT       +/- toggle between LOAD and STORE; short press -> that
//                row's slot choice; long press -> BROWSE.
//   LOAD_SLOT    +/- pick the Multi slot (wrapping); short press loads it
//                (an empty slot is reported, nothing loaded) -> SELECT;
//                long press -> SELECT.
//   STORE_SLOT   +/- pick the Multi slot; short press -> NAME_CURSOR; long
//                press -> SELECT.
//   NAME_CURSOR  the name (name_editor.h); OK renames and stores ->
//   NAME_CHAR    SELECT; long press -> SELECT, nothing stored.

enum class MultiEditMode : uint8_t { BROWSE, SELECT, LOAD_SLOT, STORE_SLOT, NAME_CURSOR, NAME_CHAR };

// The outcome of the last load or Store, shown until the next command.
enum class MultiEditReport : uint8_t { NONE, LOADED, STORED, EMPTY, FAILED };

template <typename Module>
class MultiEditT {
public:
    using Multi = MultiT<Module>;
    static constexpr uint32_t kLoadRow = 0;
    static constexpr uint32_t kStoreRow = 1;
    static constexpr uint32_t kRowCount = 2;

    explicit MultiEditT(Multi &multi) : multi_(multi) {}

    // Handle one UI command; false if it is left to Page navigation.
    bool command(UiCommand c) {
        const bool plus = c == UiCommand::PLUS, minus = c == UiCommand::MINUS;
        const bool enter = c == UiCommand::ENTER, exit = c == UiCommand::EXIT;
        if (mode_ == MultiEditMode::BROWSE) {
            if (!enter) return false;
            mode_ = MultiEditMode::SELECT;
            row_ = kLoadRow;
            report_ = MultiEditReport::NONE;
            return true;
        }
        report_ = MultiEditReport::NONE;

        switch (mode_) {
            case MultiEditMode::SELECT:
                if (plus || minus) row_ = (row_ + 1) % kRowCount;
                if (exit) mode_ = MultiEditMode::BROWSE;
                if (enter && row_ == kLoadRow) start_slot(MultiEditMode::LOAD_SLOT);
                if (enter && row_ == kStoreRow) start_slot(MultiEditMode::STORE_SLOT);
                break;
            case MultiEditMode::LOAD_SLOT:
            case MultiEditMode::STORE_SLOT:
                if (plus) slot_ = (slot_ + 1) % kUserMultiSlots;
                if (minus) slot_ = (slot_ + kUserMultiSlots - 1) % kUserMultiSlots;
                if (exit) mode_ = MultiEditMode::SELECT;
                if (enter && mode_ == MultiEditMode::LOAD_SLOT) load();
                if (enter && mode_ == MultiEditMode::STORE_SLOT) {
                    names_.start(multi_.name());
                    mode_ = MultiEditMode::NAME_CURSOR;
                }
                break;
            case MultiEditMode::NAME_CURSOR:
            case MultiEditMode::NAME_CHAR:
                switch (names_.command(c)) {
                    case NameEdit::DONE: store(); break;
                    case NameEdit::CANCELLED: mode_ = MultiEditMode::SELECT; break;
                    case NameEdit::EDITING: break;
                }
                break;
            case MultiEditMode::BROWSE:
                break;
        }
        return true;
    }

    MultiEditMode mode() const {
        if (mode_ == MultiEditMode::NAME_CURSOR && names_.char_mode()) return MultiEditMode::NAME_CHAR;
        return mode_;
    }
    uint32_t row() const { return row_; }
    uint32_t slot() const { return slot_; }
    const char *draft() const { return names_.draft(); }
    uint32_t name_cursor() const { return names_.cursor(); }
    MultiEditReport report() const { return report_; }

private:
    // Into a slot choice: the Multi's own slot if it has one, else the
    // first slot holding a Multi (to load) or the first empty one (to store).
    void start_slot(MultiEditMode mode) {
        mode_ = mode;
        slot_ = 0;
        if (multi_.origin_slot() >= 0) {
            slot_ = (uint32_t)multi_.origin_slot();
            return;
        }
        const bool want_stored = mode == MultiEditMode::LOAD_SLOT;
        char name[kPatchNameLen + 1];
        for (uint32_t s = 0; s < kUserMultiSlots; s++) {
            if (multi_.user_slot_name(s, name) == want_stored) {
                slot_ = s;
                return;
            }
        }
    }

    void load() {
        const PatchLoadResult r = multi_.load(slot_);
        report_ = r == PatchLoadResult::OK            ? MultiEditReport::LOADED
                : r == PatchLoadResult::NO_SUCH_PATCH ? MultiEditReport::EMPTY
                                                      : MultiEditReport::FAILED;
        mode_ = MultiEditMode::SELECT;
    }

    // Rename to the draft and store; a failed Store puts the old name back.
    void store() {
        char old_name[kPatchNameLen + 1], name[kPatchNameLen + 1];
        for (uint32_t i = 0; i <= kPatchNameLen; i++) old_name[i] = multi_.name()[i];
        names_.name(name);
        multi_.rename(name);
        if (multi_.store(slot_) == StoreResult::OK) {
            report_ = MultiEditReport::STORED;
        } else {
            multi_.rename(old_name);
            report_ = MultiEditReport::FAILED;
        }
        mode_ = MultiEditMode::SELECT;
    }

    Multi &multi_;
    MultiEditMode mode_ = MultiEditMode::BROWSE;
    uint32_t row_ = kLoadRow;
    uint32_t slot_ = 0;
    NameEditor names_;
    MultiEditReport report_ = MultiEditReport::NONE;
};
