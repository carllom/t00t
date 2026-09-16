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
#include "speaker_sim.h"
#include "engine.h"
#include "instruments.h"
#include "ay_instruments.h"
#include "pico/time.h"
#include <cstdio>

#ifndef HAS_ENCODER
#define HAS_ENCODER 0
#endif
#if HAS_ENCODER
#include "encoder_nav.h"
#endif

// Chip status display (Core 0, low priority): the shared Widget/Header/Page
// library (#124) applied to chip, same Performance-page layout as OPL's and
// FM's own (docs/module_opl.md / docs/module_fm.md's Display sections) --
// Header's Resource bar folding voices+CPU, the current instrument, and
// FXMIX/FX P1/FX P2/FX type as compact Value bars/Label -- except chip has
// no continuous mod-wheel-style modifier, so the MOD slot is instead SPKR:
// the active speaker simulation preset (CC17), a selection with a name
// rather than a 0-127 value, shown as a Label exactly like FX type. DIAG
// carries the exact-value detail Resource bar drops (CPU%, per-voice
// activity, last note) plus the per-voice instrument/wave-table-row grid --
// reachable via the rotary encoder (encoder_nav.h) where one's wired
// (HAS_ENCODER), Performance-only otherwise.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(200, 120, 20);   // amber -- distinct from
                                                               // subtractive's blue / speech's violet
static const uint16_t COL_TITLE_FG = gfx_rgb(255, 255, 255);
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL    = gfx_rgb(70, 130, 180);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);     // plain sounding indicator (ActivityGrid)
static const uint16_t COL_HELD    = gfx_rgb(255, 200, 60);    // gated (key down)
static const uint16_t COL_RINGING = gfx_rgb(120, 140, 160);   // released, still audible

static constexpr uint8_t TOTAL_INSTRUMENT_COUNT = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;

static constexpr int BODY_Y0 = 36;

// Performance page rows -- identical shape to OPL's/FM's own, except the
// half-width slot below the FX rows is SPKR (a Label) rather than MOD (a
// Value bar).
static constexpr int PRESET_Y  = BODY_Y0;
static constexpr int FX_ROW1_Y = 60;
static constexpr int FX_ROW2_Y = 82;
static constexpr int SPKR_Y    = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;

// DIAG page rows
static constexpr int CPU_Y     = BODY_Y0;
static constexpr int VOICES_Y  = 60;
static constexpr int VGRID_Y   = 80;
static constexpr int NOTE_Y    = 112;
static constexpr int GRID_ROW0 = 140;
static constexpr int DIAG_BAR_W = 232;

// Per-voice grid: voices 0..GRID_VOICES-1 (MAX_VOICES=32 is too many for a
// legible one-cell-per-voice grid alongside everything else on this panel --
// voice_alloc's allocate() always scans from v=0 first, so this covers
// whatever's sounding for anything up to 8-note polyphony). 4 columns x 2
// rows, "voice:instrument/wave-row" (combined instrument-space number), held
// vs. ringing-out colour-coded.
static constexpr int GRID_VOICES = 8, GRID_ROW_H = 14, GRID_W = 240 / 4, GRID_CH = 9;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *FX_TYPE_NAMES[FX_COUNT] = { "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE" };

enum { PAGE_PERFORMANCE = 0, PAGE_DIAG = 1, PAGE_COUNT = 2 };

// Combined-space instrument name lookup (module_chip.md §12.4) -- `combined`
// is whatever CC16/Program Change actually sent: < INSTRUMENT_COUNT is a SID
// patch, the rest is AY's (input_subsystem.cpp's own split).
static const char *combined_instrument_name(uint8_t combined) {
    if (combined < INSTRUMENT_COUNT) return INSTRUMENT_NAMES[combined];
    uint8_t ay_idx = (uint8_t)(combined - INSTRUMENT_COUNT);
    return ay_idx < AY_INSTRUMENT_COUNT ? AY_INSTRUMENT_NAMES[ay_idx] : "?";
}

// VoiceParams.instrument is a per-table index (engine.h's own comment on
// that field); this converts it back to the one combined number the rest
// of the screen uses, given the voice's own type.
static uint8_t combined_instrument_index(VoiceType type, uint8_t instrument) {
    if (type == VT_AY) return (uint8_t)(INSTRUMENT_COUNT + instrument);
    return instrument;
}

struct NoteKey {
    uint8_t note, velocity;
    bool operator==(const NoteKey &o) const { return note == o.note && velocity == o.velocity; }
    bool operator!=(const NoteKey &o) const { return !(*this == o); }
};

struct GridCellKey {
    bool active;
    VoiceType type;
    uint8_t instrument, wave_pos;
    bool operator==(const GridCellKey &o) const {
        return active == o.active && type == o.type && instrument == o.instrument &&
               wave_pos == o.wave_pos;
    }
    bool operator!=(const GridCellKey &o) const { return !(*this == o); }
};

static Header<PAGE_COUNT> hdr;
static ResourceBar resource_bar;

static ValueRow<uint8_t> preset_row;
static ValueBar<uint8_t> fxmix_bar, fx_p1_bar, fx_p2_bar;
static Label<uint8_t> fx_type_label, speaker_label;

static PercentageBar cpu_bar;
static ValueRow<uint32_t> voices_row;
static ActivityGrid<(int)MAX_VOICES> voice_grid;
static ValueRow<NoteKey> note_row;
static ValueRow<GridCellKey> grid_cells[GRID_VOICES];

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

    header_draw_module_name(hdr, "CHIP", LCD_W, COL_TITLE_FG, COL_TITLE);

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
            speaker_label.initialized = false;
        } else {
            cpu_bar.bar.initialized = false;
            voices_row.initialized = false;
            voice_grid.initialized = false;
            note_row.initialized = false;
            for (auto &cell : grid_cells) cell.initialized = false;
        }
        last_page = page;
    }

    MidiUiState m;
    midi_controller_ui_state(&m);
    uint8_t speaker = chip_speaker_preset_ui();

    if (page == PAGE_PERFORMANCE) {
        uint8_t combined = m.program < TOTAL_INSTRUMENT_COUNT ? m.program : 0;
        if (!preset_row.initialized || preset_row.value != combined) {
            char text[24];
            snprintf(text, sizeof(text), "#%-3d %.10s", combined, combined_instrument_name(combined));
            value_row_draw_value(preset_row, combined, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);
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

        const char *speaker_name = speaker < SPEAKER_PRESET_COUNT ? SPEAKER_PRESET_NAMES[speaker] : "?";
        label_draw_value(speaker_label, speaker, speaker_name, BAR_X, SPKR_Y, 14, 1, COL_VALUE,
                          COL_BG);
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

        for (uint32_t v = 0; v < GRID_VOICES; v++) {
            ChipVoiceUiState ui;
            chip_voice_ui_state(v, &ui);
            bool sounding = (snd >> v) & 1u;
            bool active_cell = sounding && ui.type != VT_SILENT;
            GridCellKey key{ active_cell, ui.type, ui.instrument, ui.wave_pos };
            if (!grid_cells[v].initialized || grid_cells[v].value != key) {
                int col = (int)(v % 4), row = (int)(v / 4);
                int x = col * GRID_W, y = GRID_ROW0 + row * GRID_ROW_H;
                char text[GRID_CH + 1];
                uint16_t fg = COL_LABEL;
                if (active_cell) {
                    uint8_t combined = combined_instrument_index(ui.type, ui.instrument);
                    snprintf(text, sizeof(text), "%lu:%u/%u", (unsigned long)v, combined,
                             ui.wave_pos);
                    fg = ui.held ? COL_HELD : COL_RINGING;
                } else {
                    text[0] = '\0';
                }
                value_row_draw_value(grid_cells[v], key, text, x, y, GRID_CH, 1, fg, COL_BG);
            }
        }
    }

    first = false;
}

void display_bringup_test() {}
