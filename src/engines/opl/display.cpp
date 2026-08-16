#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "engine.h"
#include "pico/time.h"
#include <cstdio>

// OPL status display (Core 0, low priority). Same chrome/status-row shape as
// every other engine's display (VOICES/CPU/NOTE), plus the current patch
// (name, resolved for whichever channel most recently triggered a note) and
// a one-cell-per-operator algorithm indicator -- "F" (FM chain) or "A"
// (additive) plus a feedback marker, since OPL only ever has two operators
// and two possible algorithms.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(150, 90, 30);   // amber -- distinct from the other engines' bars
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);
static const uint16_t COL_CARRIER = gfx_rgb(60, 220, 90);
static const uint16_t COL_MODULATOR = gfx_rgb(80, 180, 255);
static const uint16_t COL_FEEDBACK = gfx_rgb(255, 200, 60);

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 12;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116;
static constexpr int ROW_PATCH = 156;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

static constexpr int VBAR_Y = 56, VCELL_PITCH = 14, VCELL_W = 12, VBAR_H = 14;
static_assert(MAX_VOICES * VCELL_PITCH <= 240, "voice bar must fit LCD_W");

static constexpr int NOTE_VAL_X = 68, NOTE_VAL_Y = ROW_NOTE + 4, NOTE_CH = 16;

static constexpr int PATCH_X = 0, PATCH_CH = 30;

// Two cells: op0 then op1, colour = carrier (writes to OUT) vs modulator.
// Feedback (op0 only, real hardware) marks op0 yellow when nonzero.
static constexpr int ALGO_Y = 176, ALGO_CELL_PITCH = 24, ALGO_CELL_W = 20, ALGO_CELL_H = 16;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);
    gfx_text(0, ROW_VOICES, "VOICES", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,    "CPU",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,   "NOTE",   COL_LABEL, COL_BG, 2);

    lcd_set_backlight(100);
}

void display_task() {
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(100);

    static bool     first = true;
    static uint32_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    static const OplPatch *last_patch = nullptr;

    uint32_t snd  = voice_alloc_active_mask();
    uint8_t  load = audio_engine_load();
    MidiUiState m;
    midi_controller_ui_state(&m);

    char buf[24];

    auto draw_cell = [](int i, bool sounding) {
        int x = i * VCELL_PITCH + 1;
        uint16_t fill = sounding ? COL_SND : COL_OFF;
        gfx_fill_rect(x, VBAR_Y, VCELL_W, VBAR_H, fill);
    };

    if (first || snd != last_snd) {
        for (int i = 0; i < (int)MAX_VOICES; i++) {
            bool s = snd & (1u << i), ws = last_snd & (1u << i);
            if (first || s != ws) draw_cell(i, s);
        }
        snprintf(buf, sizeof(buf), "%d/%d", __builtin_popcount(snd), MAX_VOICES);
        draw_val(ROW_VOICES, buf, COL_VALUE);
        last_snd = snd;
    }

    if (first || (load > last_load ? load - last_load : last_load - load) >= 2) {
        uint16_t c = load < 50 ? COL_LOAD_LO : (load < 80 ? COL_LOAD_MID : COL_LOAD_HI);
        snprintf(buf, sizeof(buf), "%d%%", load);
        draw_val(ROW_CPU, buf, c);
        int fill = load * CBAR_W / 100;
        gfx_fill_rect(CBAR_X, CBAR_Y, fill, CBAR_H, c);
        gfx_fill_rect(CBAR_X + fill, CBAR_Y, CBAR_W - fill, CBAR_H, COL_OFF);
        last_load = load;
    }

    if (first || m.last_note != last_midi.last_note || m.last_velocity != last_midi.last_velocity
              || m.last_channel != last_midi.last_channel) {
        if (m.last_note == 0xFF) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int oct = m.last_note / 12 - 1;
            snprintf(buf, sizeof(buf), "%s%d v%d c%d", NOTE_NAMES[m.last_note % 12], oct,
                     m.last_velocity, m.last_channel + 1);
        }
        char note_padded[NOTE_CH + 1];
        snprintf(note_padded, sizeof(note_padded), "%-*.*s", NOTE_CH, NOTE_CH, buf);
        gfx_text(NOTE_VAL_X, NOTE_VAL_Y, note_padded, COL_VALUE, COL_BG, 1);
    }

    // Patch + algorithm: whichever channel's Program Change/CC16 (or
    // note-on) landed most recently.
    const OplPatch *patch = opl_channel_patch(m.last_channel);
    if (first || patch != last_patch) {
        char raw[24];
        snprintf(raw, sizeof(raw), "#%-3d %.10s", m.program, patch->name);
        char padded[PATCH_CH + 1];
        snprintf(padded, sizeof(padded), "%-*.*s", PATCH_CH, PATCH_CH, raw);
        gfx_text(PATCH_X, ROW_PATCH, padded, COL_VALUE, COL_BG, 1);

        for (uint8_t i = 0; i < 2; i++) {
            bool carrier = (patch->algorithm == OPL_ALGO_ADD) || (i == 1);
            uint16_t fg = carrier ? COL_CARRIER : COL_MODULATOR;
            uint16_t fill = (i == 0 && patch->feedback > 0) ? COL_FEEDBACK : fg;
            int x = i * ALGO_CELL_PITCH + 2;
            gfx_fill_rect(x, ALGO_Y, ALGO_CELL_W, ALGO_CELL_H, fill);
            char op_label[3];
            snprintf(op_label, sizeof(op_label), "%u", i + 1);
            gfx_text(x + 6, ALGO_Y + 3, op_label, COL_BG, fill, 1);
        }
        last_patch = patch;
    }
    last_midi = m;

    first = false;
}

void display_bringup_test() {}
