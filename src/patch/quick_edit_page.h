#pragma once

#include "quick_edit.h"
#include "wslcd/gfx.h"
#include "wslcd/label.h"
#include "wslcd/value_bar.h"
#include "wslcd/value_row.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// Rendering of the Quick-edit Page (quick_edit.h) from the shared Widget
// library, for any module: a head row (channel, where the Patch is stored,
// its name, the edited marker), one Value bar per quick-edit parameter
// showing what is heard, COMPARE and STORE, and a hint line; the Store dialog (slot,
// what it holds, the name being edited) replaces the rows while storing.
// Shared by the device display and the host preview.

struct QuickEditPageStyle {
    uint16_t fg, bg;
    uint16_t fill, off;          // a row at rest
    uint16_t sel_fill, sel_off;  // the selected row
    uint16_t act_fill, act_off;  // the row being changed
    int x, y0, w, panel_h;       // body area: from y0 to panel_h
};

// Where a Patch is stored, as shown: "F0.03" (factory bank 0, slot 3),
// "U012" (user slot 12), "--" (not from a bank).
inline void patch_origin_text(const PatchOrigin &o, char *out, size_t n) {
    switch (o.source) {
        case PatchSource::FACTORY: snprintf(out, n, "F%u.%02u", o.bank, o.slot); return;
        case PatchSource::USER:    snprintf(out, n, "U%03u", o.slot); return;
        case PatchSource::NONE:    snprintf(out, n, "--"); return;
    }
}

// How far along its range `value` is, 0..1, on the parameter's curve.
inline float param_fill(const ParamDescriptor &d, int32_t value) {
    const float range = (float)d.max - (float)d.min;
    if (range <= 0.0f) return 0.0f;
    const float off = (float)value - (float)d.min;
    if (d.curve == ParamCurve::EXPONENTIAL) return logf(1.0f + off) / logf(1.0f + range);
    return off / range;
}

template <typename Module>
struct QuickEditPage {
    using QuickEdit = QuickEditT<Module>;

    struct RowKey {
        int32_t value;
        uint8_t state;  // 0 at rest, 1 selected, 2 being changed
        bool operator==(const RowKey &o) const { return value == o.value && state == o.state; }
    };
    struct HeadKey {
        uint32_t revision;
        uint8_t part;
        bool operator==(const HeadKey &o) const { return revision == o.revision && part == o.part; }
    };
    struct NameKey {
        char draft[kPatchNameLen + 1];
        uint32_t cursor;
        uint8_t mode;
        bool operator==(const NameKey &o) const {
            return cursor == o.cursor && mode == o.mode && strcmp(draft, o.draft) == 0;
        }
    };

    ValueRow<HeadKey> head;
    ValueBar<RowKey> rows[QuickEdit::kQuickCount];
    ValueBar<RowKey> compare_bar, store_bar;
    ValueRow<uint32_t> slot_row, over_row;
    ValueRow<NameKey> name_field;
    Label<uint32_t> hint;
    bool drawn = false;
    bool dialog = false;  // which body layout is on screen
};

// Forget everything drawn, so the next draw repaints the whole body (after
// another Page has been showing).
template <typename Module>
void quick_edit_page_invalidate(QuickEditPage<Module> &pg) {
    pg = QuickEditPage<Module>{};
}

namespace quick_edit_page_detail {

inline bool in_dialog(QuickEditMode m) {
    return m == QuickEditMode::STORE_SLOT || m == QuickEditMode::NAME_CURSOR || m == QuickEditMode::NAME_CHAR;
}

inline const char *hint_text(QuickEditMode m, QuickEditStore result) {
    if (result == QuickEditStore::OK) return "STORED";
    if (result == QuickEditStore::FAILED) return "STORE FAILED";
    switch (m) {
        case QuickEditMode::BROWSE:      return "PRESS:EDIT  TURN:PAGE";
        case QuickEditMode::SELECT:      return "TURN:ROW PRESS:OK HOLD:BACK";
        case QuickEditMode::VALUE:       return "TURN:VALUE  PRESS:DONE";
        case QuickEditMode::STORE_SLOT:  return "TURN:SLOT  PRESS:NAME";
        case QuickEditMode::NAME_CURSOR: return "TURN:MOVE  PRESS:EDIT/OK";
        case QuickEditMode::NAME_CHAR:   return "TURN:LETTER  PRESS:DONE";
    }
    return "";
}

}  // namespace quick_edit_page_detail

// Draw the Page, repainting only what changed since the last draw.
template <typename Module>
void quick_edit_page_draw(QuickEditPage<Module> &pg, const QuickEditT<Module> &qe,
                          const PatchPlatformT<Module> &platform, const QuickEditPageStyle &st) {
    using QuickEdit = QuickEditT<Module>;
    using Page = QuickEditPage<Module>;
    using namespace quick_edit_page_detail;
    constexpr int kRowPitch = 20, kBarH = 16, kRowsY = 20;
    const int chars = st.w / 8;
    const QuickEditMode mode = qe.mode();
    const uint8_t part = qe.part();

    const bool dialog = in_dialog(mode);
    if (!pg.drawn || dialog != pg.dialog) {
        const Page fresh{};
        for (auto &r : pg.rows) r = fresh.rows[0];
        pg.compare_bar = fresh.compare_bar;
        pg.store_bar = fresh.store_bar;
        pg.slot_row = fresh.slot_row;
        pg.over_row = fresh.over_row;
        pg.name_field = fresh.name_field;
        gfx_fill_rect(0, st.y0 + kRowsY - 4, st.x * 2 + st.w, st.panel_h - (st.y0 + kRowsY - 4) - 20, st.bg);
        pg.dialog = dialog;
        pg.drawn = true;
    }

    // Head: "CH1  F0.00 Fairlite*" -- at most 28 characters.
    {
        char origin[8], text[40];
        patch_origin_text(platform.origin(part), origin, sizeof origin);
        snprintf(text, sizeof text, "CH%-2u %-5s %s%s", part + 1, origin, platform.name(part),
                 platform.edited(part) ? "*" : "");
        value_row_draw_value(pg.head, typename Page::HeadKey{ platform.revision(part), part }, text, st.x,
                             st.y0, chars, 1, st.fg, st.bg);
    }

    const int rows_y = st.y0 + kRowsY;
    if (!dialog) {
        auto row_state = [&](uint32_t row) -> uint8_t {
            if (qe.row() != row || mode == QuickEditMode::BROWSE) return 0;
            return mode == QuickEditMode::VALUE ? 2 : 1;
        };
        auto fill_of = [&](uint8_t s) { return s == 2 ? st.act_fill : s == 1 ? st.sel_fill : st.fill; };
        auto off_of = [&](uint8_t s) { return s == 2 ? st.act_off : s == 1 ? st.sel_off : st.off; };

        for (uint32_t r = 0; r < QuickEdit::kQuickCount; r++) {
            const uint32_t id = QuickEdit::param_id(r);
            const ParamDescriptor &d = Module::kDescriptors[id];
            const int32_t v = QuickEdit::shown_param(platform, part, id);
            char value[20], text[40];
            param_format(d, v, value, sizeof value);
            snprintf(text, sizeof text, "%-9s%s", d.name, value);
            const uint8_t s = row_state(r);
            value_bar_draw_value(pg.rows[r], typename Page::RowKey{ v, s }, text, param_fill(d, v), st.x,
                                 rows_y + (int)r * kRowPitch, st.w, kBarH, 1, st.fg, fill_of(s), off_of(s));
        }

        const int y = rows_y + (int)QuickEdit::kQuickCount * kRowPitch + 4;
        const int half = (st.w - 4) / 2;
        const bool cmp = platform.comparing(part);
        const uint8_t cs = row_state(QuickEdit::kCompareRow), ss = row_state(QuickEdit::kStoreRow);
        // Buttons: solid amber while selected, solid green while Compare is on.
        value_bar_draw_value(pg.compare_bar, typename Page::RowKey{ cmp, cs }, cmp ? "COMPARE ON" : "COMPARE",
                             (cs || cmp) ? 1.0f : 0.0f, st.x, y, half, kBarH, 1, st.fg,
                             cs ? st.sel_fill : st.act_fill, st.off);
        value_bar_draw_value(pg.store_bar, typename Page::RowKey{ 0, ss }, "STORE", ss ? 1.0f : 0.0f,
                             st.x + half + 4, y, half, kBarH, 1, st.fg, st.sel_fill, st.off);
    } else {
        const uint32_t slot = qe.store_slot();
        char text[40], name[kPatchNameLen + 1];
        snprintf(text, sizeof text, "STORE U%03lu", (unsigned long)slot);
        value_row_draw_value(pg.slot_row, slot | ((uint32_t)mode << 16), text, st.x, rows_y, st.w / 16, 2,
                             mode == QuickEditMode::STORE_SLOT ? st.sel_fill : st.fg, st.bg);
        if (platform.user_slot_name(slot, name)) snprintf(text, sizeof text, "OVER: %s", name);
        else snprintf(text, sizeof text, "EMPTY SLOT");
        value_row_draw_value(pg.over_row, slot, text, st.x, rows_y + 22, chars, 1, st.fg, st.bg);

        // The name, eight characters per line at scale 2, then OK; the
        // cursor's cell is highlighted while naming.
        typename Page::NameKey key{};
        memcpy(key.draft, qe.draft(), sizeof key.draft);
        key.cursor = qe.name_cursor();
        key.mode = (uint8_t)mode;
        if (!pg.name_field.initialized || !(pg.name_field.value == key)) {
            const bool naming = mode != QuickEditMode::STORE_SLOT;
            const uint16_t hl = mode == QuickEditMode::NAME_CHAR ? st.act_fill : st.sel_fill;
            for (uint32_t i = 0; i < kPatchNameLen; i++) {
                const char cell[2] = { qe.draft()[i], '\0' };
                const bool at = naming && key.cursor == i;
                gfx_text(st.x + (int)(i % 8) * 16, rows_y + 40 + (int)(i / 8) * 20, cell, at ? st.bg : st.fg,
                         at ? hl : st.off, 2);
            }
            const bool at_ok = naming && key.cursor == kPatchNameLen;
            gfx_text(st.x, rows_y + 84, " OK ", at_ok ? st.bg : st.fg, at_ok ? st.sel_fill : st.off, 2);
            pg.name_field.value = key;
            pg.name_field.initialized = true;
        }
    }

    const uint32_t hint_key = (uint32_t)mode | ((uint32_t)qe.store_result() << 8);
    label_draw_value(pg.hint, hint_key, hint_text(mode, qe.store_result()), st.x, st.panel_h - 16, chars, 1,
                     st.fg, st.bg);
}
