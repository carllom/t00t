#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "presets.h"
#include "pico/time.h"
#include <cstdio>

// --- palette (wire-format RGB565) ---
static const uint16_t COL_BG     = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE  = gfx_rgb(30, 90, 160);
static const uint16_t COL_LABEL  = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE  = gfx_rgb(240, 240, 240);
// Voice dot: fill = sounding, border = note pressed/held.
static const uint16_t COL_SND    = gfx_rgb(60, 220, 90);    // sounding (envelope active)
static const uint16_t COL_OFF    = gfx_rgb(28, 28, 34);     // silent
static const uint16_t COL_PRESS  = gfx_rgb(255, 255, 255);  // pressed border
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);

// --- layout ---
static constexpr int LABEL_X = 4;
static constexpr int VAL_X   = 104;
static constexpr int VAL_CH  = 8;     // value field width in chars (VAL_X..232 @2x)
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116,
                     ROW_PRESET = 138, ROW_BEND = 160, ROW_MOD = 182,
                     ROW_FXTYPE = 200, ROW_FXA = 220, ROW_FXB = 240, ROW_FXMIX = 260;
static constexpr int VBAR_Y = 56, VBAR_H = 14, VCELL_PITCH = 15, VCELL_W = 13;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

static const char *PRESET_NAMES[PRESET_COUNT] = {
    "Fairlite", "Sq-PWM", "Saw-Flt", "Marimba", "LowStr5", "VoiceAh", "VoiceArr", "VoiceRrr",
    "Sitar2", "Zither2", "ElecPian",
};
static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *FX_NAMES[FX_COUNT] = { "Off", "Delay", "Reverb" };

// Draw a value into its 8-char field. Padding to VAL_CH clears any longer
// previous value in a single blit (gfx_text fills the glyph-cell background).
static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

// Dynamic label at x=0 (padded to 6 chars so a shorter new label clears the old).
static void draw_label(int y, const char *s) {
    char b[7];
    snprintf(b, sizeof(b), "%-6s", s);
    gfx_text(0, y, b, COL_LABEL, COL_BG, 2);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    // Title bar + static labels (drawn once). Centre the title so it clears the
    // panel's rounded corners ("t00t" = 4 chars × 8px × scale 3 = 96px wide).
    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);
    gfx_text(0, ROW_VOICES, "VOICES", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,    "CPU",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,   "NOTE",   COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_PRESET, "PRESET", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_BEND,   "BEND",   COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_MOD,    "MOD",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_FXTYPE, "FX",     COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_FXMIX,  "MIX",    COL_LABEL, COL_BG, 2);
    // FX param labels (ROW_FXA/ROW_FXB) are drawn by display_task per effect type.

    lcd_set_backlight(100);
}

void display_task() {
    // Self-limit refresh rate; cheap early-out on most main-loop passes.
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(50);   // ~20 Hz

    // Change-detection state (force a full first paint).
    static bool     first = true;
    static uint16_t last_snd = 0;
    static uint16_t last_gate = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0, 0xFF, 0x7FFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    uint16_t snd  = voice_alloc_active_mask();  // still sounding (envelope active)
    uint16_t gate = voice_alloc_gated_mask();   // note pressed/held
    uint8_t  load = audio_engine_load();
    MidiUiState m;
    midi_controller_ui_state(&m);

    char buf[16];

    // Draw one voice dot: fill shows "sounding", a border shows "pressed".
    auto draw_cell = [](int i, bool sounding, bool pressed) {
        int x = i * VCELL_PITCH + 1;
        uint16_t fill = sounding ? COL_SND : COL_OFF;
        gfx_fill_rect(x, VBAR_Y, VCELL_W, VBAR_H, pressed ? COL_PRESS : fill);
        gfx_fill_rect(x + 2, VBAR_Y + 2, VCELL_W - 4, VBAR_H - 4, fill);
    };

    // Voices: per-cell bar (redraw a cell when its sounding/pressed state
    // changes) + sounding count.
    if (first || snd != last_snd || gate != last_gate) {
        for (int i = 0; i < MAX_VOICES; i++) {
            bool s = snd & (1u << i), p = gate & (1u << i);
            bool ws = last_snd & (1u << i), wp = last_gate & (1u << i);
            if (first || s != ws || p != wp) draw_cell(i, s, p);
        }
        if (first || snd != last_snd) {
            snprintf(buf, sizeof(buf), "%d/%d", __builtin_popcount(snd), MAX_VOICES);
            draw_val(ROW_VOICES, buf, COL_VALUE);
        }
        last_snd = snd;
        last_gate = gate;
    }

    // CPU: percentage + load bar (quantise to avoid churn on tiny EMA wiggles).
    if (first || (load > last_load ? load - last_load : last_load - load) >= 2) {
        uint16_t c = load < 50 ? COL_LOAD_LO : (load < 80 ? COL_LOAD_MID : COL_LOAD_HI);
        snprintf(buf, sizeof(buf), "%d%%", load);
        draw_val(ROW_CPU, buf, c);
        int fill = load * CBAR_W / 100;
        gfx_fill_rect(CBAR_X, CBAR_Y, fill, CBAR_H, c);
        gfx_fill_rect(CBAR_X + fill, CBAR_Y, CBAR_W - fill, CBAR_H, COL_OFF);
        last_load = load;
    }

    // Note + velocity.
    if (first || m.last_note != last_midi.last_note || m.last_velocity != last_midi.last_velocity) {
        if (m.last_note == 0xFF) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int oct = m.last_note / 12 - 1;
            snprintf(buf, sizeof(buf), "%s%d v%d", NOTE_NAMES[m.last_note % 12], oct, m.last_velocity);
        }
        draw_val(ROW_NOTE, buf, COL_VALUE);
    }

    if (first || m.program != last_midi.program) {
        const char *name = m.program < PRESET_COUNT ? PRESET_NAMES[m.program] : "?";
        draw_val(ROW_PRESET, name, COL_VALUE);
    }

    if (first || m.bend != last_midi.bend) {
        snprintf(buf, sizeof(buf), "%+d", m.bend);
        draw_val(ROW_BEND, buf, COL_VALUE);
    }

    if (first || m.mod != last_midi.mod) {
        snprintf(buf, sizeof(buf), "%d", m.mod);
        draw_val(ROW_MOD, buf, COL_VALUE);
    }

    // Effect: type + two type-dependent params + mix. On a type change, repaint
    // the type value and the two variable labels, and force the values to redraw.
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

void display_bringup_test() {
    // Colour bars across the top third.
    static const uint16_t bars[] = {
        gfx_rgb(255, 0, 0), gfx_rgb(0, 255, 0), gfx_rgb(0, 0, 255),
        gfx_rgb(255, 255, 0), gfx_rgb(0, 255, 255), gfx_rgb(255, 0, 255),
    };
    const int nbars = sizeof(bars) / sizeof(bars[0]);
    int bw = LCD_W / nbars;
    for (int i = 0; i < nbars; i++) {
        gfx_fill_rect(i * bw, 0, bw, 80, bars[i]);
    }

    // Gradient band in the middle.
    // (gfx_gradient paints the whole panel; instead draw a framed area.)
    gfx_fill_rect(0, 80, LCD_W, 120, gfx_rgb(16, 16, 24));

    // Text banner. Strings are sized to fit the 240px width (10ch @3x = 240,
    // 15ch @2x = 240).
    uint16_t white = gfx_rgb(255, 255, 255);
    uint16_t bg    = gfx_rgb(16, 16, 24);
    uint16_t amber = gfx_rgb(255, 180, 0);
    gfx_text(0, 100, "hello t00t", amber, bg, 3);
    gfx_text(0, 140, "ST7789P 240x284", white, bg, 2);
    gfx_text(0, 165, "SPI1+DMA ok", white, bg, 2);

    // Bottom strip.
    gfx_fill_rect(0, 200, LCD_W, LCD_H - 200, gfx_rgb(0, 0, 0));
    gfx_text(0, 220, "core0 low-pri", gfx_rgb(120, 200, 120), gfx_rgb(0, 0, 0), 2);
}
