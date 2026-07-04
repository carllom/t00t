#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "engine.h"           // FX_COUNT, FX_* (via engine_base.h)
#include "pico/time.h"
#include <cstdio>

// Groovebox status display (Core 0, low priority). Mirrors the subtractive
// display's chrome but drops preset/bend/mod in favour of a MODE line
// (303 vs DRUM). Reuses the shared gfx + LCD driver.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(160, 60, 30);   // amber-ish groovebox bar
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 8;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116,
                     ROW_MODE = 138, ROW_FXTYPE = 176, ROW_FXA = 196,
                     ROW_FXB = 216, ROW_FXMIX = 236;
static constexpr int VBAR_Y = 56, VCELL_PITCH = 15, VCELL_W = 13, VBAR_H = 14;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *FX_NAMES[FX_COUNT] = { "Off", "Delay", "Reverb" };

static constexpr uint8_t DRUM_CHANNEL = 9;   // must match midi_controller.cpp

static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

static void draw_label(int y, const char *s) {
    char b[7];
    snprintf(b, sizeof(b), "%-6s", s);
    gfx_text(0, y, b, COL_LABEL, COL_BG, 2);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);
    gfx_text(0, ROW_VOICES, "VOICES", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,    "CPU",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,   "NOTE",   COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_MODE,   "MODE",   COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_FXTYPE, "FX",     COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_FXMIX,  "MIX",    COL_LABEL, COL_BG, 2);

    lcd_set_backlight(100);
}

void display_task() {
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(50);   // ~20 Hz

    static bool     first = true;
    static uint16_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };

    uint16_t snd  = voice_alloc_active_mask();   // sounding (Core 1 envelope bitmap)
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
        for (int i = 0; i < MAX_VOICES; i++) {
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

    if (first || m.last_channel != last_midi.last_channel) {
        const char *mode = (m.last_channel == DRUM_CHANNEL) ? "DRUM" : "303";
        draw_val(ROW_MODE, mode, COL_VALUE);
    }

    bool fx_type_changed = first || m.fx_type != last_midi.fx_type;
    if (fx_type_changed) {
        const char *tn = m.fx_type < FX_COUNT ? FX_NAMES[m.fx_type] : "?";
        draw_val(ROW_FXTYPE, tn, m.fx_type == FX_OFF ? COL_LABEL : COL_LOAD_MID);
        const char *la = "P1", *lb = "P2";
        if (m.fx_type == FX_DELAY)  { la = "FBK";  lb = "TIME"; }
        if (m.fx_type == FX_REVERB) { la = "SIZE"; lb = "DAMP"; }
        draw_label(ROW_FXA, la);
        draw_label(ROW_FXB, lb);
    }
    if (fx_type_changed || m.fx_p1 != last_midi.fx_p1) {
        snprintf(buf, sizeof(buf), "%d%%", m.fx_p1 * 100 / 127);
        draw_val(ROW_FXA, buf, COL_VALUE);
    }
    if (fx_type_changed || m.fx_p2 != last_midi.fx_p2) {
        if (m.fx_type == FX_DELAY) {
            snprintf(buf, sizeof(buf), "%dms", 20 + m.fx_p2 * 980 / 127);
        } else {
            snprintf(buf, sizeof(buf), "%d%%", m.fx_p2 * 100 / 127);
        }
        draw_val(ROW_FXB, buf, COL_VALUE);
    }
    if (fx_type_changed || m.fx_mix != last_midi.fx_mix) {
        snprintf(buf, sizeof(buf), "%d%%", m.fx_mix * 100 / 127);
        draw_val(ROW_FXMIX, buf, m.fx_mix ? COL_LOAD_MID : COL_VALUE);
    }

    last_midi = m;
    first = false;
}

void display_bringup_test() {}
