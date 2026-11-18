#pragma once

#include "stored_format.h"
#include "wslcd/gfx.h"
#include <cstdint>
#include <cstring>

// Drawing shared by the editing Pages (quick_edit_page.h, multi_page.h).

struct EditPageStyle {
    uint16_t fg, bg;
    uint16_t fill, off;          // a row at rest
    uint16_t sel_fill, sel_off;  // the selected row
    uint16_t act_fill, act_off;  // the row being changed
    int x, y0, w, panel_h;       // body area: from y0 to panel_h
};

// A name being edited (name_editor.h): eight characters per line at
// scale 2, then OK, with the cursor's cell highlighted while naming
// (amber, green while changing the character). Redraws only on change.
struct NameField {
    struct Key {
        char draft[kPatchNameLen + 1];
        uint32_t cursor;
        uint8_t state;  // 0 not naming, 1 cursor, 2 changing a character
        bool operator==(const Key &o) const {
            return cursor == o.cursor && state == o.state && strcmp(draft, o.draft) == 0;
        }
    };
    Key value{};
    bool initialized = false;
};

inline void name_field_draw(NameField &f, const char *draft, uint32_t cursor, uint8_t state, int x, int y,
                            const EditPageStyle &st) {
    NameField::Key key{};
    memcpy(key.draft, draft, sizeof key.draft);
    key.cursor = cursor;
    key.state = state;
    if (f.initialized && f.value == key) return;
    const uint16_t hl = state == 2 ? st.act_fill : st.sel_fill;
    for (uint32_t i = 0; i < kPatchNameLen; i++) {
        const char cell[2] = { draft[i], '\0' };
        const bool at = state && cursor == i;
        gfx_text(x + (int)(i % 8) * 16, y + (int)(i / 8) * 20, cell, at ? st.bg : st.fg, at ? hl : st.off, 2);
    }
    const bool at_ok = state && cursor == kPatchNameLen;
    gfx_text(x, y + 44, " OK ", at_ok ? st.bg : st.fg, at_ok ? st.sel_fill : st.off, 2);
    f.value = key;
    f.initialized = true;
}
