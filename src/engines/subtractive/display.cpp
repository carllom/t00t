#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "header.h"
#include "resource_bar.h"
#include "value_row.h"
#include "value_bar.h"
#include "percentage_bar.h"
#include "activity_grid.h"
#include "label.h"
#include "page.h"
#include "refresh_cadence.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "presets.h"
#include "pico/time.h"
#include <cstdio>

#ifndef HAS_ENCODER
#define HAS_ENCODER 0
#endif
#if HAS_ENCODER
#include "encoder_nav.h"
#endif

// Subtractive status display (Core 0, low priority): the shared Widget/
// Header/Page library (#124) applied to subtractive, same Performance-page
// layout as OPL's/FM's/chip's own (docs/module_opl.md, module_fm.md,
// module_chip.md's Display sections) -- Header's Resource bar folding
// voices+CPU, the current preset, and FXMIX/FX P1/FX P2/FX type/MOD as
// compact Value bars/Label. DIAG carries the exact-value detail Resource
// bar drops (CPU%, per-voice activity, last note) -- reachable via the
// rotary encoder (encoder_nav.h) where one's wired (HAS_ENCODER),
// Performance-only otherwise.
//
// No per-channel patch or algorithm concept here (subtractive is a single
// global preset played across all channels/voices, per module_subtractive.md
// "fixed to one note/channel/preset"), so DIAG has no multitimbral grid or
// algorithm indicator the way FM's/OPL's own do -- CPU/voices/note is the
// whole of it.

static const uint16_t COL_BG       = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE    = gfx_rgb(30, 90, 160);   // blue
static const uint16_t COL_TITLE_FG = gfx_rgb(255, 255, 255);
static const uint16_t COL_LABEL    = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE    = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND      = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF      = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL     = gfx_rgb(70, 130, 180);

static constexpr int BODY_Y0 = 36;

// Performance page rows -- identical shape to OPL's/FM's/chip's own.
static constexpr int PRESET_Y  = BODY_Y0;
static constexpr int FX_ROW1_Y = 60;
static constexpr int FX_ROW2_Y = 82;
static constexpr int MOD_Y     = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;

// DIAG page rows
static constexpr int CPU_Y     = BODY_Y0;
static constexpr int VOICES_Y  = 60;
static constexpr int VGRID_Y   = 80;
static constexpr int NOTE_Y    = 104;
static constexpr int DIAG_BAR_W = 232;

static const char *PRESET_NAMES[PRESET_COUNT] = {
    "Fairlite", "Sq-PWM", "Saw-Flt", "Marimba", "LowStr5", "VoiceAh", "VoiceArr", "VoiceRrr",
    "Sitar2", "Zither2", "ElecPian",
};
static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *FX_TYPE_NAMES[FX_COUNT] = { "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS" };

enum { PAGE_PERFORMANCE = 0, PAGE_DIAG = 1, PAGE_COUNT = 2 };

struct NoteKey {
    uint8_t note, velocity;
    bool operator==(const NoteKey &o) const { return note == o.note && velocity == o.velocity; }
    bool operator!=(const NoteKey &o) const { return !(*this == o); }
};

static Header<PAGE_COUNT> hdr;
static ResourceBar resource_bar;

static ValueRow<uint8_t> preset_row;
static ValueBar<uint8_t> fxmix_bar, fx_p1_bar, fx_p2_bar, mod_bar;
static Label<uint8_t> fx_type_label;

static PercentageBar cpu_bar;
static ValueRow<uint32_t> voices_row;
static ActivityGrid<(int)MAX_VOICES> voice_grid;
static ValueRow<NoteKey> note_row;

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);
    lcd_set_backlight(100);

#if HAS_ENCODER
    encoder_nav_init(PAGE_COUNT, PAGE_PERFORMANCE);
#endif
}

void display_task() {
    static PageRefreshCadence cadence{};  // base 10Hz -- neither Page needs the faster override
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(1000 / refresh_rate_hz(cadence));

    static bool    first = true;
    static uint8_t last_page = 0xFF;

#if HAS_ENCODER
    uint8_t page = encoder_nav_page_index();
#else
    uint8_t page = PAGE_PERFORMANCE;
#endif

    header_draw_module_name(hdr, "SUBTRACTIVE", LCD_W, COL_TITLE_FG, COL_TITLE);

    uint32_t snd = voice_alloc_active_mask();
    uint8_t  load = audio_engine_load();
    int      row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, __builtin_popcount(snd), MAX_VOICES, load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);

    header_draw_page_row(hdr, page, page == PAGE_PERFORMANCE ? "PERF" : "DIAG", LCD_W,
                          COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1, 8);

    if (first || page != last_page) {
        gfx_fill_rect(0, BODY_Y0, LCD_W, LCD_H - BODY_Y0, COL_BG);
        if (page == PAGE_PERFORMANCE) {
            preset_row.initialized = false;
            fxmix_bar.initialized = false;
            fx_p1_bar.initialized = false;
            fx_p2_bar.initialized = false;
            fx_type_label.initialized = false;
            mod_bar.initialized = false;
        } else {
            cpu_bar.bar.initialized = false;
            voices_row.initialized = false;
            voice_grid.initialized = false;
            note_row.initialized = false;
        }
        last_page = page;
    }

    MidiUiState m;
    midi_controller_ui_state(&m);

    if (page == PAGE_PERFORMANCE) {
        if (!preset_row.initialized || preset_row.value != m.program) {
            const char *name = m.program < PRESET_COUNT ? PRESET_NAMES[m.program] : "?";
            char text[24];
            snprintf(text, sizeof(text), "#%-3d %.10s", m.program, name);
            value_row_draw_value(preset_row, m.program, text, 4, PRESET_Y, 28, 1, COL_VALUE,
                                  COL_BG);
        }

        uint8_t fx_type = m.fx_type < FX_COUNT ? m.fx_type : 0;
        label_draw_value(fx_type_label, m.fx_type, FX_TYPE_NAMES[fx_type], BAR_X, FX_ROW1_Y, 14, 1,
                          COL_VALUE, COL_BG);
        value_bar_draw_value(fxmix_bar, m.fx_mix, "FXMIX", m.fx_mix / 127.0f, BAR_X2, FX_ROW1_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

        value_bar_draw_value(fx_p1_bar, m.fx_p1, "FX P1", m.fx_p1 / 127.0f, BAR_X, FX_ROW2_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
        value_bar_draw_value(fx_p2_bar, m.fx_p2, "FX P2", m.fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

        value_bar_draw_value(mod_bar, m.mod, "MOD", m.mod / 127.0f, BAR_X, MOD_Y, BAR_HALF_W, BAR_H,
                              1, COL_VALUE, COL_FILL, COL_OFF);
    } else {
        percentage_bar_draw_value(cpu_bar, load, "CPU", BAR_X, CPU_Y, DIAG_BAR_W, 20, 2, COL_VALUE,
                                   COL_OFF);

        uint32_t voice_count = (uint32_t)__builtin_popcount(snd);
        if (!voices_row.initialized || voices_row.value != voice_count) {
            char text[8];
            snprintf(text, sizeof(text), "%lu/%d", (unsigned long)voice_count, (int)MAX_VOICES);
            value_row_draw_value(voices_row, voice_count, text, BAR_X, VOICES_Y, 8, 2, COL_VALUE,
                                  COL_BG);
        }

        bool active[MAX_VOICES];
        for (uint32_t i = 0; i < MAX_VOICES; i++) active[i] = snd & (1u << i);
        activity_grid_draw(voice_grid, active, BAR_X, VGRID_Y, kActivityGridCellPitch, COL_SND,
                            COL_OFF);

        NoteKey note{ m.last_note, m.last_velocity };
        if (!note_row.initialized || note_row.value != note) {
            char text[20];
            if (m.last_note == 0xFF) {
                snprintf(text, sizeof(text), "--");
            } else {
                int oct = m.last_note / 12 - 1;
                snprintf(text, sizeof(text), "%s%d v%d", NOTE_NAMES[m.last_note % 12], oct,
                         m.last_velocity);
            }
            value_row_draw_value(note_row, note, text, BAR_X, NOTE_Y, 18, 2, COL_VALUE, COL_BG);
        }
    }

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
