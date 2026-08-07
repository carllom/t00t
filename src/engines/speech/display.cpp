#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "engine.h"
#include "phonemes.h"
#include "pico/time.h"
#include <cstdio>

// Speech status display (Core 0, low priority). Mirrors the groovebox
// display's chrome (buttonless breadboard, no presets.h) with a PHON row
// in place of MODE -- speech.md's Open Question #4 ("current phoneme ...
// is the obvious answer and is cheap at ~10 Hz redraw"), and doubles as the
// debug visibility #28 needed: confirms program-change is actually landing
// and driving vp.phoneme, independent of whether the timbre change is
// obvious by ear. FX rows dropped for now -- not part of #28, revisit if
// delay/reverb becomes a real requirement for this engine.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(90, 30, 160);   // violet-ish speech bar
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 8;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116, ROW_PHONEME = 138;
static constexpr int VBAR_Y = 56, VCELL_PITCH = 15, VCELL_W = 13, VBAR_H = 14;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
// PHONEME_LABELS (phonemes.h, #32) replaces this row's old hand-typed,
// 12-entry copy -- that copy would have silently gone stale the moment the
// generated table grew past it.

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
    gfx_text(0, ROW_PHONEME, "PHON",  COL_LABEL, COL_BG, 2);

    lcd_set_backlight(100);
}

void display_task() {
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(50);   // ~20 Hz

    static bool     first = true;
    static uint32_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };

    uint32_t snd  = voice_alloc_active_mask();   // sounding (Core 1 gate bitmap)
    uint8_t  load = audio_engine_load();
    MidiUiState m;
    midi_controller_ui_state(&m);

    char buf[16];

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

    if (first || m.last_note != last_midi.last_note || m.last_velocity != last_midi.last_velocity) {
        if (m.last_note == 0xFF) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int oct = m.last_note / 12 - 1;
            snprintf(buf, sizeof(buf), "%s%d v%d", NOTE_NAMES[m.last_note % 12], oct, m.last_velocity);
        }
        draw_val(ROW_NOTE, buf, COL_VALUE);
    }

    // The whole point of this row (#28 debugging): m.program is
    // channel_phoneme[channel] at the moment of the *last note-on* -- so this
    // updates on note-on, not on program change itself (matches "affects
    // future notes only"). If PC really isn't landing, this value will
    // never move no matter how many program changes are sent.
    if (first || m.program != last_midi.program) {
        const char *name = m.program < PHONEME_COUNT ? PHONEME_LABELS[m.program] : "?";
        draw_val(ROW_PHONEME, name, COL_VALUE);
    }

    last_midi = m;
    first = false;
}

void display_bringup_test() {}
