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
// display's chrome (buttonless breadboard, no presets.h) with a per-voice
// phoneme grid and F1/F2 formant-space plot in place of the old single MODE
// row -- #37 / module_speech.md's Open Question 2 ("current phoneme plus a
// formant-space plot ... genuinely useful ... the same F1/F2 view the vowel-
// chart verification in #32 uses"). This supersedes #28's single PHON row
// (last note-on's channel program, useful only for "did PC land at all"):
// the per-voice grid below shows the same information per sounding voice,
// which the single row couldn't. FX rows dropped for now -- not part of
// #28/#37, revisit if delay/reverb becomes a real requirement for this
// engine.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(90, 30, 160);   // violet-ish speech bar
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);
static const uint16_t COL_PLOT_BG = gfx_rgb(14, 14, 20);
static const uint16_t COL_PLOT_BORDER = gfx_rgb(70, 70, 90);

// One colour per voice slot, shared between the phoneme grid text and its
// formant-plot dot so the two views of the same voice are visually linked.
static const uint16_t VOICE_COLORS[MAX_VOICES] = {
    gfx_rgb(255, 80, 80), gfx_rgb(80, 180, 255), gfx_rgb(255, 200, 60), gfx_rgb(120, 255, 120),
    gfx_rgb(220, 120, 255), gfx_rgb(255, 140, 60), gfx_rgb(80, 255, 220), gfx_rgb(255, 255, 255),
};
static_assert(MAX_VOICES <= 8, "VOICE_COLORS only has 8 entries");

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 8;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116;
static constexpr int VBAR_Y = 56, VCELL_PITCH = 15, VCELL_W = 13, VBAR_H = 14;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

// Per-voice phoneme grid (#37 acceptance: "current phoneme shown per active
// voice"): 2 columns x 4 rows, one cell per voice (MAX_VOICES == 8).
static constexpr int PHON_ROW0 = 138, PHON_ROW_H = 12, PHON_COL_W = 120, PHON_CH = 10;

// F1/F2 formant-space plot (#37: "F1 on one axis, F2 on the other, a dot per
// voice moving as the segment sequencer ramps between targets"). Oriented
// like the conventional IPA vowel chart: F1 (low = close vowel) increases
// downward, F2 (high = front vowel) increases leftward. Range covers every
// vowel in phonemes.h's PHONEME_TARGETS with headroom (#32's I=270/2290 and
// W=290/610 are the extremes) -- consonants mostly share a fixed F1/F2
// placeholder and cluster near centre, which is correct: this tract has no
// place-of-articulation formant data for them yet.
static constexpr int PLOT_X = 4, PLOT_Y = 190, PLOT_W = 232, PLOT_H = 90;
static constexpr int PLOT_DOT = 5;
static constexpr float F1_MIN = 200.0f, F1_MAX = 800.0f;
static constexpr float F2_MIN = 500.0f, F2_MAX = 2400.0f;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

// `text` == "" clears the cell (voice inactive -- #37: only active voices
// show a phoneme). Padded to PHON_CH so a shorter new string still overwrites
// every glyph cell the previous, longer one occupied.
static void draw_phon_cell(uint32_t v, const char *text, uint16_t fg) {
    int col = (int)(v % 2), row = (int)(v / 2);
    int x = col * PHON_COL_W, y = PHON_ROW0 + row * PHON_ROW_H;
    char b[PHON_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", PHON_CH, PHON_CH, text);
    gfx_text(x, y, b, fg, COL_BG, 1);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Maps (F1, F2) in Hz to a top-left pixel origin for the dot, inside the
// plot's 1px-inset interior.
static void formant_to_xy(uint16_t f1, uint16_t f2, int &x, int &y) {
    float fx = clampf((float)f2, F2_MIN, F2_MAX);
    float fy = clampf((float)f1, F1_MIN, F1_MAX);
    int inner_x = PLOT_X + 1, inner_y = PLOT_Y + 1;
    int inner_w = PLOT_W - 2 - PLOT_DOT, inner_h = PLOT_H - 2 - PLOT_DOT;
    x = inner_x + (int)((F2_MAX - fx) / (F2_MAX - F2_MIN) * (float)inner_w);
    y = inner_y + (int)((fy - F1_MIN) / (F1_MAX - F1_MIN) * (float)inner_h);
}

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);

    gfx_fill_rect(0, 0, LCD_W, 30, COL_TITLE);
    gfx_text((LCD_W - 96) / 2, 3, "t00t", gfx_rgb(255, 255, 255), COL_TITLE, 3);
    gfx_text(0, ROW_VOICES, "VOICES", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,    "CPU",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,   "NOTE",   COL_LABEL, COL_BG, 2);

    // Plot border, painted once -- display_task only ever clears/redraws the
    // 1px-inset interior, so this frame never needs to be touched again.
    gfx_fill_rect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, COL_PLOT_BORDER);
    gfx_fill_rect(PLOT_X + 1, PLOT_Y + 1, PLOT_W - 2, PLOT_H - 2, COL_PLOT_BG);

    lcd_set_backlight(100);
}

void display_task() {
    // Redraws at ~10 Hz (#37 acceptance criterion) -- far below the segment
    // rate, plenty for a monitor that isn't trying to be an animation, and
    // this is Core 0 wall-clock time, so it has no effect on Core 1's render
    // deadline either way.
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(100);

    static bool     first = true;
    static uint32_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    static uint8_t  last_ph[MAX_VOICES];
    static bool     last_active[MAX_VOICES] = {};
    if (first) for (uint32_t v = 0; v < MAX_VOICES; v++) last_ph[v] = PHONEME_COUNT;

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
    last_midi = m;

    // Per-voice phoneme grid + formant plot (#37). Pull each voice's current
    // telemetry once and reuse it for both.
    SpeechVoiceUiState ui[MAX_VOICES];
    for (uint32_t v = 0; v < MAX_VOICES; v++) speech_voice_ui_state(v, &ui[v]);

    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (first || ui[v].phoneme != last_ph[v] || ui[v].active != last_active[v]) {
            if (ui[v].active) {
                const char *label = ui[v].phoneme < PHONEME_COUNT ? PHONEME_LABELS[ui[v].phoneme] : "?";
                snprintf(buf, sizeof(buf), "V%u %s", v, label);
                draw_phon_cell(v, buf, VOICE_COLORS[v]);
            } else {
                draw_phon_cell(v, "", COL_LABEL);
            }
            last_ph[v] = ui[v].phoneme;
            last_active[v] = ui[v].active;
        }
    }

    // Plot: full interior clear + redraw every tick. F1/F2 ramp continuously
    // while a voice sounds, so per-dot change detection would still redraw
    // almost every tick anyway -- a flat clear/redraw is simpler and, per
    // gfx.cpp's comment, a solid fill is a single cheap DMA transfer
    // regardless of the rect's pixel count.
    gfx_fill_rect(PLOT_X + 1, PLOT_Y + 1, PLOT_W - 2, PLOT_H - 2, COL_PLOT_BG);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (!ui[v].active) continue;
        int x, y;
        formant_to_xy(ui[v].f1_hz, ui[v].f2_hz, x, y);
        gfx_fill_rect(x, y, PLOT_DOT, PLOT_DOT, VOICE_COLORS[v]);
    }

    first = false;
}

void display_bringup_test() {}
