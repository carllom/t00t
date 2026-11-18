#pragma once

#include "stored_format.h"
#include "wslcd/page.h"
#include <cstdint>

// Editing a Patch or Multi name with the UI commands: a cursor over the
// kPatchNameLen characters plus a final OK field.
//
//   cursor mode  +/- move the cursor; short press on a character -> char
//                mode, on OK -> DONE; long press -> CANCELLED
//   char mode    +/- step the character through kChars (wrapping); short
//                press -> cursor mode; long press -> CANCELLED

enum class NameEdit : uint8_t { EDITING, DONE, CANCELLED };

class NameEditor {
public:
    // Characters a name can be edited to, space first.
    static constexpr char kChars[] =
        " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-+._/#&!";

    // Start on `name`, in cursor mode on its first character.
    void start(const char *name) {
        bool ended = false;
        for (uint32_t i = 0; i < kPatchNameLen; i++) {
            if (!ended && name[i] == '\0') ended = true;
            draft_[i] = ended ? ' ' : name[i];
        }
        draft_[kPatchNameLen] = '\0';
        cursor_ = 0;
        char_mode_ = false;
    }

    NameEdit command(UiCommand c) {
        if (c == UiCommand::EXIT) return NameEdit::CANCELLED;
        if (char_mode_) {
            constexpr uint32_t n = sizeof kChars - 1;
            const uint32_t i = char_index(draft_[cursor_]);
            if (c == UiCommand::PLUS) draft_[cursor_] = kChars[(i + 1) % n];
            if (c == UiCommand::MINUS) draft_[cursor_] = kChars[(i + n - 1) % n];
            if (c == UiCommand::ENTER) char_mode_ = false;
            return NameEdit::EDITING;
        }
        if (c == UiCommand::PLUS && cursor_ < kPatchNameLen) cursor_++;
        if (c == UiCommand::MINUS && cursor_ > 0) cursor_--;
        if (c == UiCommand::ENTER) {
            if (cursor_ == kPatchNameLen) return NameEdit::DONE;
            char_mode_ = true;
        }
        return NameEdit::EDITING;
    }

    // The name being edited, space-padded to kPatchNameLen.
    const char *draft() const { return draft_; }
    // 0..kPatchNameLen - 1 a character, kPatchNameLen the OK field.
    uint32_t cursor() const { return cursor_; }
    bool char_mode() const { return char_mode_; }

    // The draft with trailing spaces dropped, into `out` (kPatchNameLen + 1).
    void name(char *out) const {
        for (uint32_t i = 0; i <= kPatchNameLen; i++) out[i] = draft_[i];
        for (int32_t i = kPatchNameLen - 1; i >= 0 && out[i] == ' '; i--) out[i] = '\0';
    }

private:
    static uint32_t char_index(char c) {
        for (uint32_t i = 0; i + 1 < sizeof kChars; i++) {
            if (kChars[i] == c) return i;
        }
        return 0;
    }

    char draft_[kPatchNameLen + 1] = {};
    uint32_t cursor_ = 0;
    bool char_mode_ = false;
};
