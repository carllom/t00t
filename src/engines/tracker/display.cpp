#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "player_task.h"
#include "pico/time.h"
#include <cstdio>

// Playback position display (#24, tracker.md "Display"): order/pattern/row,
// per-channel activity, and song title/tracker name/channel count. Reads
// tracker_player_ui_state()/tracker_player_song() -- both Core-0-local state
// the player task already holds, so no reverse channel from Core 1 is
// needed; the snapshot is one tick ahead of what's audible, invisible at
// 20ms (tracker.md, player_task.h's TrackerUiState comment).
//
// No persistent framebuffer: every primitive here goes through gfx.cpp's
// existing tile/DMA path, whose only backing storage is its shared
// s_glyph scratch (8*3 x 8*3 x uint16_t = 1,152 B, already paid for by every
// engine's display). This file adds no buffer of its own -- static state
// below is a handful of uint32_t change-detection latches, well under 100 B
// -- so the SRAM cost of this display path is that shared 1,152 B, a small
// fraction of the sample budget (tracker.md: "350-400 KB of SRAM").

static const uint16_t COL_BG    = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE = gfx_rgb(30, 90, 160);
static const uint16_t COL_LABEL = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE = gfx_rgb(240, 240, 240);
static const uint16_t COL_ON    = gfx_rgb(60, 220, 90);   // channel active this tick
static const uint16_t COL_OFF   = gfx_rgb(28, 28, 34);    // channel silent

// --- layout ---
static constexpr int ROW_SONG = 36, ROW_TRKR = 56, ROW_CH = 76,
                      ROW_ORD = 98, ROW_PAT = 118, ROW_ROW = 138,
                      ROW_CHLABEL = 162, CH_ROW1_Y = 182, CH_ROW2_Y = 200;
static constexpr int VAL_X = 60;
static constexpr int NAME_CH = 22;  // (LCD_W - VAL_X) / 8   @scale1 -- fits XM's 20/24-char name fields
static constexpr int NUM_CH  = 11;  // (LCD_W - VAL_X) / 16  @scale2
// Up to 16 channel-activity cells per row, 2 rows -- covers the format's
// 2-32 channel range (blob_format.py) without the cells shrinking below a
// legible size. Pitch/width match the subtractive engine's voice dots.
static constexpr int CELL_PITCH = 15, CELL_W = 13, CELL_H = 14;

static void draw_label(int y, const char *s) {
    char b[5];
    snprintf(b, sizeof(b), "%-4s", s);
    gfx_text(0, y, b, COL_LABEL, COL_BG, 2);
}

// Name fields (song title, tracker name) at scale 1 -- the extra width over
// scale 2 is what lets a full 20-char XM name field actually fit.
static void draw_name(int y, const char *s) {
    char b[NAME_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", NAME_CH, NAME_CH, s);
    gfx_text(VAL_X, y, b, COL_VALUE, COL_BG, 1);
}

static void draw_num(int y, const char *s) {
    char b[NUM_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", NUM_CH, NUM_CH, s);
    gfx_text(VAL_X, y, b, COL_VALUE, COL_BG, 2);
}

static void draw_cell(uint32_t i, bool active) {
    int x = (int)(i % 16) * CELL_PITCH + 1;
    int y = (i < 16) ? CH_ROW1_Y : CH_ROW2_Y;
    gfx_fill_rect(x, y, CELL_W, CELL_H, active ? COL_ON : COL_OFF);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    // Title bar, centred as in the other engines' display.cpp ("t00t" = 4
    // chars x 8px x scale 3 = 96px wide).
    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);

    draw_label(ROW_SONG, "SONG");
    draw_label(ROW_TRKR, "TRKR");
    draw_label(ROW_CH,   "CH");
    draw_label(ROW_ORD,  "ORD");
    draw_label(ROW_PAT,  "PAT");
    draw_label(ROW_ROW,  "ROW");
    gfx_text(0, ROW_CHLABEL, "CHANNELS", COL_LABEL, COL_BG, 2);

    // Song metadata is fixed for the process lifetime (no song-swap UI) --
    // painted once here, never revisited by display_task().
    const SongHeader *song = tracker_player_song();
    draw_name(ROW_SONG, song->name);
    draw_name(ROW_TRKR, song->tracker_name);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", song->num_channels);
    draw_num(ROW_CH, buf);

    lcd_set_backlight(100);
}

void display_task() {
    // Self-limit refresh rate; cheap early-out on most main-loop passes.
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(50);  // ~20 Hz

    static bool first = true;
    static uint32_t last_order = 0xFFFFFFFF, last_pat = 0xFFFFFFFF, last_row = 0xFFFFFFFF;
    static uint32_t last_mask = 0;

    TrackerUiState ui;
    tracker_player_ui_state(&ui);
    const SongHeader *song = tracker_player_song();

    char buf[16];
    if (first || ui.order_idx != last_order) {
        snprintf(buf, sizeof(buf), "%u/%u", ui.order_idx + 1, song->num_orders);
        draw_num(ROW_ORD, buf);
        last_order = ui.order_idx;
    }
    if (first || ui.pattern_idx != last_pat) {
        snprintf(buf, sizeof(buf), "%u/%u", ui.pattern_idx, song->num_patterns);
        draw_num(ROW_PAT, buf);
        last_pat = ui.pattern_idx;
    }
    if (first || ui.row != last_row) {
        snprintf(buf, sizeof(buf), "%u", ui.row);
        draw_num(ROW_ROW, buf);
        last_row = ui.row;
    }
    if (first || ui.active_mask != last_mask) {
        for (uint32_t c = 0; c < song->num_channels; c++) {
            bool a = (ui.active_mask & (1u << c)) != 0;
            bool wa = (last_mask & (1u << c)) != 0;
            if (first || a != wa) draw_cell(c, a);
        }
        last_mask = ui.active_mask;
    }

    first = false;
}

// No bring-up test pattern for this engine yet (unused by the live UI on
// every engine, groovebox leaves it empty too -- see wslcd/display.h).
void display_bringup_test() {}
