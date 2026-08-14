#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "speaker_sim.h"
#include "engine.h"
#include "pico/time.h"
#include <cstdio>

// Chip status display (Core 0, low priority). sid.md §1 P5's "LCD UI" --
// same chrome/status-row shape as subtractive's and speech's display.cpp
// (VOICES/CPU/NOTE), plus two chip-specific rows P5 introduced: the active
// speaker preset (speaker_sim.h) and current instrument, and a compact
// per-voice table/wave-row grid (engine.h's ChipVoiceUiState, §15 open
// question 3's "current table row, active instrument").
//
// MAX_VOICES here is 32, not speech's 8 -- too many for a full one-cell-
// per-voice grid on this panel, so the grid below is fixed to voices 0-7
// rather than scrolling or compacting active voices into place. voice_alloc
// fills low-numbered slots first (its three-tier scan always starts at
// v = 0, src/voice_alloc.cpp), so in practice this covers whatever's
// sounding for anything up to 8-note polyphony -- past that, the VOICES
// count row is still exact, only the grid stops being the full picture.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(200, 120, 20);   // amber -- distinct from
                                                               // subtractive's blue / speech's violet
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);
static const uint16_t COL_HELD    = gfx_rgb(255, 200, 60);    // gated (key down)
static const uint16_t COL_RINGING = gfx_rgb(120, 140, 160);   // released, still audible

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 12;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116;
static constexpr int ROW_SPEAKER = 156, ROW_INSTR = 176;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

// Fixed 8-voice grid (voices 0-7 of MAX_VOICES -- see file header).
static constexpr int GRID_ROW0 = 210, GRID_ROW_H = 12, GRID_W = 240 / 4, GRID_CH = 9;
static constexpr int GRID_VOICES = 8;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

static void draw_grid_cell(uint32_t v, const char *text, uint16_t fg) {
    int col = (int)(v % 4), row = (int)(v / 4);
    int x = col * GRID_W, y = GRID_ROW0 + row * GRID_ROW_H;
    char b[GRID_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", GRID_CH, GRID_CH, text);
    gfx_text(x, y, b, fg, COL_BG, 1);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);
    gfx_text(0, ROW_VOICES,  "VOICES",  COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,     "CPU",     COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,    "NOTE",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_SPEAKER, "SPKR",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_INSTR,   "INSTR",   COL_LABEL, COL_BG, 2);

    lcd_set_backlight(100);
}

void display_task() {
    // Redraws at ~10 Hz -- same rate every other engine's display settled
    // on (speech's #37 acceptance criterion); Core 0 wall-clock only, no
    // effect on Core 1's render deadline.
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(100);

    static bool     first = true;
    static uint32_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    static uint8_t  last_speaker = 0xFF;
    static bool     last_held[GRID_VOICES] = {};
    static uint8_t  last_instr[GRID_VOICES], last_wave_pos[GRID_VOICES];

    uint32_t snd  = voice_alloc_active_mask();
    uint8_t  load = audio_engine_load();
    MidiUiState m;
    midi_controller_ui_state(&m);
    uint8_t speaker = chip_speaker_preset_ui();

    char buf[16];

    if (first || snd != last_snd) {
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

    if (first || speaker != last_speaker) {
        const char *name = speaker < SPEAKER_PRESET_COUNT ? SPEAKER_PRESET_NAMES[speaker] : "?";
        draw_val(ROW_SPEAKER, name, COL_VALUE);
        last_speaker = speaker;
    }

    if (first || m.program != last_midi.program) {
        snprintf(buf, sizeof(buf), "%d", m.program);
        draw_val(ROW_INSTR, buf, COL_VALUE);
    }
    last_midi = m;

    for (uint32_t v = 0; v < GRID_VOICES; v++) {
        ChipVoiceUiState ui;
        chip_voice_ui_state(v, &ui);
        bool sounding = (snd >> v) & 1u;
        if (first || ui.held != last_held[v] || ui.instrument != last_instr[v] || ui.wave_pos != last_wave_pos[v]) {
            if (sounding) {
                snprintf(buf, sizeof(buf), "%u:%u/%u", v, ui.instrument, ui.wave_pos);
                draw_grid_cell(v, buf, ui.held ? COL_HELD : COL_RINGING);
            } else {
                draw_grid_cell(v, "", COL_LABEL);
            }
            last_held[v] = ui.held;
            last_instr[v] = ui.instrument;
            last_wave_pos[v] = ui.wave_pos;
        }
    }

    first = false;
}

void display_bringup_test() {}
