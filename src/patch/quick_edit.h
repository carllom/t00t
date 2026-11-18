#pragma once

#include "name_editor.h"
#include "platform.h"
#include "wslcd/page.h"
#include <cstdint>

// The Quick-edit Page's interaction: what the four UI commands mean while
// it is showing (CONTEXT.md "Quick-edit Page", "UI command"). Module-
// agnostic: the rows are the module's quick-edit-flagged parameters in ID
// order, then COMPARE and STORE. Core 0 only; rendering is the module's.
//
//   BROWSE       +/- and long press are left to Page navigation (not
//                claimed); short press starts editing: SELECT, first row.
//   SELECT       +/- move between rows (wrapping); short press acts on the
//                row: a parameter -> VALUE, COMPARE toggles Compare,
//                STORE -> STORE_SLOT; long press -> BROWSE.
//   VALUE        +/- step the parameter (param_step()) in the Edit buffer;
//                short or long press -> SELECT.
//   STORE_SLOT   +/- pick the user slot (wrapping); short press ->
//                NAME_CURSOR; long press -> SELECT, nothing stored.
//   NAME_CURSOR  the name (name_editor.h): cursor mode; OK renames and
//   NAME_CHAR    stores, then -> SELECT; long press -> SELECT, nothing
//                stored. NAME_CHAR is the editor's char mode.
//
// While browsing, the Page follows the Part it is told to (the last-played
// one); once editing starts it stays on that Part until backed out. A load
// into the Part closes the Store dialog, so a Store never pairs one
// Patch's name with another's values.

enum class QuickEditMode : uint8_t { BROWSE, SELECT, VALUE, STORE_SLOT, NAME_CURSOR, NAME_CHAR };

// The outcome of the last Store, shown until the next command.
enum class QuickEditStore : uint8_t { NONE, OK, FAILED };

template <typename Module>
class QuickEditT {
public:
    using Platform = PatchPlatformT<Module>;

    static constexpr uint32_t count_quick() {
        uint32_t n = 0;
        for (const ParamDescriptor &d : Module::kDescriptors) n += d.quick_edit ? 1 : 0;
        return n;
    }
    static constexpr uint32_t kQuickCount = count_quick();
    static_assert(kQuickCount >= 4 && kQuickCount <= 8, "a Quick-edit Page shows 4-8 parameters");
    static constexpr uint32_t kCompareRow = kQuickCount;
    static constexpr uint32_t kStoreRow = kQuickCount + 1;
    static constexpr uint32_t kRowCount = kQuickCount + 2;

    static constexpr auto &kNameChars = NameEditor::kChars;

    // Parameter ID of parameter row `row` (< kQuickCount).
    static constexpr uint32_t param_id(uint32_t row) {
        for (uint32_t id = 0; id < Platform::kParamCount; id++) {
            if (Module::kDescriptors[id].quick_edit && row-- == 0) return id;
        }
        return Platform::kParamCount;
    }

    // The value a parameter row shows: what is heard -- the stored version
    // while comparing, else the Edit buffer.
    static int32_t shown_param(const Platform &platform, uint8_t part, uint32_t id) {
        return platform.comparing(part) ? platform.stored_param(part, id) : platform.param(part, id);
    }

    explicit QuickEditT(Platform &platform) : platform_(platform) {}

    // Handle one UI command; false if it is left to Page navigation.
    bool command(UiCommand c);

    // The Part to show while browsing; ignored while editing or if unknown.
    // Also closes the Store dialog if the Part has loaded another Patch.
    void follow_part(uint8_t part) {
        if (mode_ == QuickEditMode::BROWSE && part < Platform::kPartCount) part_ = part;
        close_store_if_reloaded();
    }

    QuickEditMode mode() const {
        if (mode_ == QuickEditMode::NAME_CURSOR && names_.char_mode()) return QuickEditMode::NAME_CHAR;
        return mode_;
    }
    uint8_t part() const { return part_; }
    uint32_t row() const { return row_; }
    uint32_t store_slot() const { return slot_; }
    // The name being edited, space-padded to kPatchNameLen.
    const char *draft() const { return names_.draft(); }
    // 0..kPatchNameLen - 1 a character, kPatchNameLen the OK field.
    uint32_t name_cursor() const { return names_.cursor(); }
    QuickEditStore store_result() const { return store_result_; }

private:
    void start_store();
    void finish_store();

    bool in_store() const {
        return mode_ == QuickEditMode::STORE_SLOT || mode_ == QuickEditMode::NAME_CURSOR;
    }
    void close_store_if_reloaded() {
        if (in_store() && platform_.load_id(part_) != store_load_id_) mode_ = QuickEditMode::SELECT;
    }

    Platform &platform_;
    QuickEditMode mode_ = QuickEditMode::BROWSE;
    uint8_t part_ = 0;
    uint32_t row_ = 0;
    uint32_t slot_ = 0;
    NameEditor names_;
    uint16_t store_load_id_ = 0;  // the Part's load when the Store dialog opened
    QuickEditStore store_result_ = QuickEditStore::NONE;
};

template <typename Module>
bool QuickEditT<Module>::command(UiCommand c) {
    const bool plus = c == UiCommand::PLUS, minus = c == UiCommand::MINUS;
    const bool enter = c == UiCommand::ENTER, exit = c == UiCommand::EXIT;
    if (mode_ == QuickEditMode::BROWSE) {
        if (!enter) return false;
        mode_ = QuickEditMode::SELECT;
        row_ = 0;
        store_result_ = QuickEditStore::NONE;
        return true;
    }
    store_result_ = QuickEditStore::NONE;
    if (in_store() && platform_.load_id(part_) != store_load_id_) {
        mode_ = QuickEditMode::SELECT;
        return true;
    }

    switch (mode_) {
        case QuickEditMode::SELECT:
            if (plus) row_ = (row_ + 1) % kRowCount;
            if (minus) row_ = (row_ + kRowCount - 1) % kRowCount;
            if (exit) mode_ = QuickEditMode::BROWSE;
            if (enter) {
                if (row_ < kQuickCount) mode_ = QuickEditMode::VALUE;
                else if (row_ == kCompareRow) platform_.set_compare(part_, !platform_.comparing(part_));
                else start_store();
            }
            break;
        case QuickEditMode::VALUE:
            if (plus || minus) {
                const uint32_t id = param_id(row_);
                const int32_t v = platform_.param(part_, id);
                platform_.set_param(part_, id, param_step(Module::kDescriptors[id], v, plus ? 1 : -1));
            }
            if (enter || exit) mode_ = QuickEditMode::SELECT;
            break;
        case QuickEditMode::STORE_SLOT:
            if (plus) slot_ = (slot_ + 1) % kUserPatchSlots;
            if (minus) slot_ = (slot_ + kUserPatchSlots - 1) % kUserPatchSlots;
            if (enter) mode_ = QuickEditMode::NAME_CURSOR;
            if (exit) mode_ = QuickEditMode::SELECT;
            break;
        case QuickEditMode::NAME_CURSOR:
        case QuickEditMode::NAME_CHAR:
            switch (names_.command(c)) {
                case NameEdit::DONE: finish_store(); break;
                case NameEdit::CANCELLED: mode_ = QuickEditMode::SELECT; break;
                case NameEdit::EDITING: break;
            }
            break;
        case QuickEditMode::BROWSE:
            break;
    }
    return true;
}

// Into STORE_SLOT: the slot the Patch came from if it is a user Patch,
// else the first empty one; the draft is the Edit buffer's name.
template <typename Module>
void QuickEditT<Module>::start_store() {
    const PatchOrigin o = platform_.origin(part_);
    slot_ = 0;
    if (o.source == PatchSource::USER) {
        slot_ = o.slot;
    } else {
        char name[kPatchNameLen + 1];
        for (uint32_t s = 0; s < kUserPatchSlots; s++) {
            if (!platform_.user_slot_name(s, name)) { slot_ = s; break; }
        }
    }
    names_.start(platform_.name(part_));
    store_load_id_ = platform_.load_id(part_);
    mode_ = QuickEditMode::STORE_SLOT;
}

// Rename to the draft (trailing spaces dropped) and store; a failed Store
// puts the old name back.
template <typename Module>
void QuickEditT<Module>::finish_store() {
    char old_name[kPatchNameLen + 1];
    const char *cur = platform_.name(part_);
    for (uint32_t i = 0; i <= kPatchNameLen; i++) old_name[i] = cur[i];

    char name[kPatchNameLen + 1];
    names_.name(name);

    platform_.rename(part_, name);
    if (platform_.store(part_, slot_) == StoreResult::OK) {
        store_result_ = QuickEditStore::OK;
    } else {
        platform_.rename(part_, old_name);
        store_result_ = QuickEditStore::FAILED;
    }
    mode_ = QuickEditMode::SELECT;
}
