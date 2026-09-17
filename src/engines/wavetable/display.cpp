#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "header.h"
#include "resource_bar.h"
#include "value_row.h"
#include "value_bar.h"
#include "label.h"
#include "refresh_cadence.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "tables.h"
#include "pico/time.h"
#include <cstdio>

// Wavetable engine skeleton status display (Core 0, low priority): a single
// required Performance page, the shared Widget/Header/Page library
// (CONTEXT.md's Widget catalog) applied minimally -- Header's Resource bar
// folding voices+CPU, the current wavetable bank (Program Change), the live
// wave-position scan (CC1), and the shared FX chain as compact Value bars/
// Label, same shape every other module's Performance page uses. No DIAG
// page yet -- module_wavetable.md's Display section is otherwise
// undesigned; see its Future/TODO.

static const uint16_t COL_BG       = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE    = gfx_rgb(130, 60, 170);   // violet -- distinct from sibling engines' bars
static const uint16_t COL_TITLE_FG = gfx_rgb(255, 255, 255);
static const uint16_t COL_VALUE    = gfx_rgb(240, 240, 240);
static const uint16_t COL_OFF      = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL     = gfx_rgb(70, 130, 180);

static constexpr int BODY_Y0    = 24;
static constexpr int PRESET_Y   = BODY_Y0;
static constexpr int WAVEPOS_Y  = 48;
static constexpr int FX_ROW1_Y  = 72;
static constexpr int FX_ROW2_Y  = 94;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16, FULL_W = 232;

static const char *FX_TYPE_NAMES[FX_COUNT] = { "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE" };

enum { PAGE_PERFORMANCE = 0, PAGE_COUNT = 1 };

static Header<PAGE_COUNT> hdr;
static ResourceBar resource_bar;

static ValueRow<uint8_t> preset_row;
static ValueBar<uint8_t> wavepos_bar, fxmix_bar, fx_p1_bar, fx_p2_bar;
static Label<uint8_t> fx_type_label;

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);
    lcd_set_backlight(100);
}

void display_task() {
    static PageRefreshCadence cadence{};  // base 10Hz -- nothing here needs the faster override
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(1000 / refresh_rate_hz(cadence));

    static bool first = true;

    header_draw_module_name(hdr, "WAVETABLE", LCD_W, COL_TITLE_FG, COL_TITLE);

    uint32_t snd = voice_alloc_active_mask();
    uint8_t  load = audio_engine_load();
    int      row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, __builtin_popcount(snd), MAX_VOICES, load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);
    // No header_draw_page_row() call: a single-Page module never draws row 2
    // (Header<1>, CONTEXT.md's Page entry).

    if (first) {
        gfx_fill_rect(0, BODY_Y0, LCD_W, LCD_H - BODY_Y0, COL_BG);
    }

    MidiUiState m;
    midi_controller_ui_state(&m);

    if (!preset_row.initialized || preset_row.value != m.program) {
        const char *name = m.program < WT_BANK_COUNT ? WT_BANK_NAMES[m.program] : "?";
        char text[24];
        snprintf(text, sizeof(text), "#%-3d %.10s", m.program, name);
        value_row_draw_value(preset_row, m.program, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);
    }

    value_bar_draw_value(wavepos_bar, m.mod, "WAVEPOS", m.mod / 127.0f, BAR_X, WAVEPOS_Y,
                          FULL_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    uint8_t fx_type = m.fx_type < FX_COUNT ? m.fx_type : 0;
    label_draw_value(fx_type_label, m.fx_type, FX_TYPE_NAMES[fx_type], BAR_X, FX_ROW1_Y, 14, 1,
                      COL_VALUE, COL_BG);
    value_bar_draw_value(fxmix_bar, m.fx_mix, "FXMIX", m.fx_mix / 127.0f, BAR_X2, FX_ROW1_Y,
                          BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(fx_p1_bar, m.fx_p1, "FX P1", m.fx_p1 / 127.0f, BAR_X, FX_ROW2_Y,
                          BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
    value_bar_draw_value(fx_p2_bar, m.fx_p2, "FX P2", m.fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y,
                          BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    first = false;
}

void display_bringup_test() {}
