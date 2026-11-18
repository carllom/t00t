#pragma once

#include "edit_view.h"
#include "multi_edit.h"
#include "wslcd/gfx.h"
#include "wslcd/label.h"
#include "wslcd/value_bar.h"
#include "wslcd/value_row.h"
#include <cstdio>

// Rendering of the MULTI Page (multi_edit.h) from the shared Widget
// library, for any module: a head row (the Multi's slot and name), one
// Value bar per Part (number, channel, Patch name, filled to its volume),
// LOAD and STORE, and a hint line; the slot choice and the name being
// edited replace the Parts while loading or storing. Shared by the device
// display and the host preview.

template <typename Module>
struct MultiPage {
    using MultiEdit = MultiEditT<Module>;
    static constexpr uint32_t kPartCount = Module::kPartCount;

    struct Key {
        uint32_t multi_revision;
        uint32_t patch_revision;
        uint8_t state;  // buttons: 0 at rest, 1 selected
        bool operator==(const Key &o) const {
            return multi_revision == o.multi_revision && patch_revision == o.patch_revision && state == o.state;
        }
    };
    struct SlotKey {
        uint32_t slot;
        uint8_t mode;
        bool operator==(const SlotKey &o) const { return slot == o.slot && mode == o.mode; }
    };

    ValueRow<Key> head;
    ValueBar<Key> parts[kPartCount];
    ValueBar<Key> load_bar, store_bar;
    ValueRow<SlotKey> slot_row, holds_row;
    NameField name_field;
    Label<uint32_t> hint;
    bool drawn = false;
    bool dialog = false;
};

// Forget everything drawn, so the next draw repaints the whole body.
template <typename Module>
void multi_page_invalidate(MultiPage<Module> &pg) {
    pg = MultiPage<Module>{};
}

namespace multi_page_detail {

inline const char *hint_text(MultiEditMode m, MultiEditReport report) {
    switch (report) {
        case MultiEditReport::LOADED: return "LOADED";
        case MultiEditReport::STORED: return "STORED";
        case MultiEditReport::EMPTY:  return "EMPTY SLOT";
        case MultiEditReport::FAILED: return "FAILED";
        case MultiEditReport::NONE:   break;
    }
    switch (m) {
        case MultiEditMode::BROWSE:      return "PRESS:SELECT  TURN:PAGE";
        case MultiEditMode::SELECT:      return "TURN:LOAD/STORE  PRESS:OK";
        case MultiEditMode::LOAD_SLOT:   return "TURN:SLOT  PRESS:LOAD";
        case MultiEditMode::STORE_SLOT:  return "TURN:SLOT  PRESS:NAME";
        case MultiEditMode::NAME_CURSOR: return "TURN:MOVE  PRESS:EDIT/OK";
        case MultiEditMode::NAME_CHAR:   return "TURN:LETTER  PRESS:DONE";
    }
    return "";
}

}  // namespace multi_page_detail

template <typename Module>
void multi_page_draw(MultiPage<Module> &pg, const MultiEditT<Module> &me, const MultiT<Module> &multi,
                     const PatchPlatformT<Module> &platform, const EditPageStyle &st) {
    using Page = MultiPage<Module>;
    using Key = typename Page::Key;
    constexpr int kPartPitch = 11, kPartH = 10, kRowsY = 16;
    const int chars = st.w / 8;
    const MultiEditMode mode = me.mode();
    const bool dialog = mode != MultiEditMode::BROWSE && mode != MultiEditMode::SELECT;
    const int rows_y = st.y0 + kRowsY;

    if (!pg.drawn || dialog != pg.dialog) {
        const Page fresh{};
        for (auto &p : pg.parts) p = fresh.parts[0];
        pg.load_bar = fresh.load_bar;
        pg.store_bar = fresh.store_bar;
        pg.slot_row = fresh.slot_row;
        pg.holds_row = fresh.holds_row;
        pg.name_field = fresh.name_field;
        gfx_fill_rect(0, rows_y - 2, st.x * 2 + st.w, st.panel_h - (rows_y - 2) - 20, st.bg);
        pg.dialog = dialog;
        pg.drawn = true;
    }

    // Head: "MULTI M03 MY MULTI"
    {
        char slot[8], text[40];
        if (multi.origin_slot() >= 0) snprintf(slot, sizeof slot, "M%02ld", (long)multi.origin_slot());
        else snprintf(slot, sizeof slot, "--");
        snprintf(text, sizeof text, "MULTI %-3s %s", slot, multi.name());
        value_row_draw_value(pg.head, Key{ multi.revision(), 0, 0 }, text, st.x, st.y0, chars, 1, st.fg, st.bg);
    }

    if (!dialog) {
        for (uint32_t p = 0; p < Page::kPartCount; p++) {
            const int32_t channel = multi.part_param((uint8_t)p, PART_P_CHANNEL);
            const int32_t volume = multi.part_param((uint8_t)p, PART_P_VOLUME);
            char text[40];
            if (channel == kPartChannelOff) {
                snprintf(text, sizeof text, "P%-2lu OFF", (unsigned long)(p + 1));
            } else {
                snprintf(text, sizeof text, "P%-2lu CH%-2ld %s", (unsigned long)(p + 1), (long)(channel + 1),
                         platform.name((uint8_t)p));
            }
            const float fill = channel == kPartChannelOff ? 0.0f : (float)volume / 127.0f;
            value_bar_draw_value(pg.parts[p], Key{ multi.revision(), platform.revision((uint8_t)p), 0 }, text, fill,
                                 st.x, rows_y + (int)p * kPartPitch, st.w, kPartH, 1, st.fg, st.fill, st.off);
        }
        const int y = rows_y + (int)Page::kPartCount * kPartPitch + 4;
        const int half = (st.w - 4) / 2;
        const bool active = mode == MultiEditMode::SELECT;
        const uint8_t ls = active && me.row() == MultiEditT<Module>::kLoadRow;
        const uint8_t ss = active && me.row() == MultiEditT<Module>::kStoreRow;
        value_bar_draw_value(pg.load_bar, Key{ 0, 0, ls }, "LOAD", ls ? 1.0f : 0.0f, st.x, y, half, 16, 1, st.fg,
                             st.sel_fill, st.off);
        value_bar_draw_value(pg.store_bar, Key{ 0, 0, ss }, "STORE", ss ? 1.0f : 0.0f, st.x + half + 4, y, half, 16, 1,
                             st.fg, st.sel_fill, st.off);
    } else {
        const uint32_t slot = me.slot();
        const bool loading = mode == MultiEditMode::LOAD_SLOT;
        const bool choosing = loading || mode == MultiEditMode::STORE_SLOT;
        char text[40], name[kPatchNameLen + 1];
        snprintf(text, sizeof text, "%s M%02lu", loading ? "LOAD" : "STORE", (unsigned long)slot);
        value_row_draw_value(pg.slot_row, typename Page::SlotKey{ slot, (uint8_t)mode }, text, st.x, rows_y,
                             st.w / 16, 2, choosing ? st.sel_fill : st.fg, st.bg);
        if (multi.user_slot_name(slot, name)) snprintf(text, sizeof text, "%s %s", loading ? "HOLDS:" : "OVER:", name);
        else snprintf(text, sizeof text, "EMPTY SLOT");
        value_row_draw_value(pg.holds_row, typename Page::SlotKey{ slot, 0 }, text, st.x, rows_y + 22, chars, 1, st.fg,
                             st.bg);
        if (!loading) {
            const uint8_t naming = mode == MultiEditMode::NAME_CHAR ? 2 : mode == MultiEditMode::NAME_CURSOR ? 1 : 0;
            name_field_draw(pg.name_field, me.draft(), me.name_cursor(), naming, st.x, rows_y + 40, st);
        }
    }

    const uint32_t hint_key = (uint32_t)mode | ((uint32_t)me.report() << 8);
    label_draw_value(pg.hint, hint_key, multi_page_detail::hint_text(mode, me.report()), st.x, st.panel_h - 16, chars,
                     1, st.fg, st.bg);
}
