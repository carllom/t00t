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
#include "engine.h"
#include "chip_platform.h"
#include "speaker_sim.h"
#include "patch/multi_page.h"
#include "patch/quick_edit_page.h"
#include "pico/time.h"
#include <cstdio>

#ifndef HAS_ENCODER
#define HAS_ENCODER 0
#endif
#if HAS_ENCODER
#include "encoder_nav.h"
#endif

// Chip status display (Core 0, low priority): the shared Widget/Header/Page
// library applied to chip -- Performance page (Header's Resource bar folding
// voices+CPU, the current Patch, and the Multi's FXMIX/FX P1/FX P2/FX type as
// compact Value bars/Label; chip has no mod wheel, so the slot below the FX
// rows is SPKR, the speaker sim preset CC17 picks). EDIT is the Quick-edit
// Page (patch/quick_edit_page.h): the last-played Part's quick-edit
// parameters, Compare and Store. MULTI (patch/multi_page.h) lists the
// Multi's Parts and loads and stores Multis. DIAG carries the exact-value
// detail Resource bar deliberately sacrifices (CPU%, per-voice activity,
// last note) plus the per-voice Part/table-row grid. EDIT, MULTI and DIAG
// are reachable via the rotary encoder (encoder_nav.h) where one's wired
// (HAS_ENCODER), Performance-only otherwise.

static const uint16_t COL_BG        = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE     = gfx_rgb(200, 120, 20);  // amber -- distinct from the other engines' bars
static const uint16_t COL_TITLE_FG  = gfx_rgb(255, 255, 255);
static const uint16_t COL_LABEL     = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE     = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND       = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF       = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL      = gfx_rgb(70, 130, 180);
static const uint16_t COL_HELD      = gfx_rgb(255, 200, 60);   // gated (key down)
static const uint16_t COL_RINGING   = gfx_rgb(120, 140, 160);  // released, still audible
static const uint16_t COL_SEL       = gfx_rgb(220, 150, 30);   // amber: selected row
static const uint16_t COL_SEL_OFF   = gfx_rgb(60, 40, 10);
static const uint16_t COL_ACT       = gfx_rgb(40, 180, 80);    // green: value being changed
static const uint16_t COL_ACT_OFF   = gfx_rgb(10, 50, 20);

static constexpr int BODY_Y0 = 36;

// Performance page rows. FX type/FXMIX and FX P1/FX P2 pair up two per row
// (scale 1's char budget leaves room side by side); SPKR sits alone on its
// own row below.
static constexpr int PRESET_Y  = BODY_Y0;
static constexpr int FX_ROW1_Y = 60;
static constexpr int FX_ROW2_Y = 82;
static constexpr int SPKR_Y    = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;

// DIAG page rows
static constexpr int CPU_Y    = BODY_Y0;
static constexpr int VOICES_Y = 60;
static constexpr int VGRID_Y  = 80;
static constexpr int NOTE_Y   = 112;
static constexpr int GRID_ROW0 = 140;
static constexpr int DIAG_BAR_W = 232;

// Per-voice grid: voices 0..GRID_VOICES-1 (MAX_VOICES=32 is too many for a
// legible one-cell-per-voice grid alongside everything else on this panel --
// voice_alloc's allocate() always scans from v=0 first, so this covers
// whatever's sounding for anything up to 8-note polyphony). 4 columns x 2
// rows, "voice:Part/table-row", held vs. ringing-out colour-coded.
static constexpr int GRID_VOICES = 8, GRID_ROW_H = 14, GRID_W = 240 / 4, GRID_CH = 9;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *FX_TYPE_NAMES[FX_COUNT] = { "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE" };

enum { PAGE_PERFORMANCE = 0, PAGE_EDIT = 1, PAGE_MULTI = 2, PAGE_DIAG = 3, PAGE_COUNT = 4 };
static const char *const PAGE_NAMES[PAGE_COUNT] = { "PERF", "EDIT", "MULTI", "DIAG" };

struct NoteKey {
    uint8_t note, velocity, channel;
    bool operator==(const NoteKey &o) const {
        return note == o.note && velocity == o.velocity && channel == o.channel;
    }
    bool operator!=(const NoteKey &o) const { return !(*this == o); }
};

struct GridCellKey {
    bool active;
    VoiceType type;
    uint8_t part, wave_pos;
    bool held;
    bool operator==(const GridCellKey &o) const {
        return active == o.active && type == o.type && part == o.part && wave_pos == o.wave_pos && held == o.held;
    }
    bool operator!=(const GridCellKey &o) const { return !(*this == o); }
};

static Header<PAGE_COUNT> hdr;
static ResourceBar resource_bar;

static ValueRow<uint32_t> preset_row;  // keyed by the Part and its platform revision
static ValueBar<uint8_t> fxmix_bar, fx_p1_bar, fx_p2_bar;
static Label<uint8_t> fx_type_label, speaker_label;

static PercentageBar cpu_bar;
static ValueRow<uint32_t> voices_row;
static ActivityGrid<(int)MAX_VOICES> voice_grid;
static ValueRow<NoteKey> note_row;
static ValueRow<GridCellKey> grid_cells[GRID_VOICES];

using ChipQuickEdit = QuickEditT<ChipModule>;
static ChipQuickEdit &quick_edit() {
    static ChipQuickEdit qe(chip_platform());
    return qe;
}
static QuickEditPage<ChipModule> quick_edit_page;

using ChipMultiEdit = MultiEditT<ChipModule>;
static ChipMultiEdit &multi_edit() {
    static ChipMultiEdit me(chip_multi());
    return me;
}
static MultiPage<ChipModule> multi_page;

static const EditPageStyle kEditStyle = {
    COL_VALUE, COL_BG, COL_FILL, COL_OFF, COL_SEL, COL_SEL_OFF, COL_ACT, COL_ACT_OFF,
    BAR_X, BODY_Y0, DIAG_BAR_W, LCD_H,
};

// The Part the UI shows for MIDI channel `channel`: the lowest Part on it,
// else Part 0.
static uint8_t shown_part(uint8_t channel) {
    const uint8_t part = chip_multi().first_part_on_channel(channel);
    return part < CHIP_PART_COUNT ? part : 0;
}

#if HAS_ENCODER
// While EDIT or MULTI is showing, that Page gets every UI command first --
// EDIT on the last-played Part as of now rather than as of the last redraw.
static bool claim_ui_command(UiCommand command) {
    const uint8_t page = encoder_nav_page_index();
    if (page == PAGE_MULTI) return multi_edit().command(command);
    if (page != PAGE_EDIT) return false;
    MidiUiState m;
    midi_controller_ui_state(&m);
    quick_edit().follow_part(shown_part(m.last_channel));
    return quick_edit().command(command);
}
#endif

void display_init() {
    lcd_init();
    lcd_fill(COL_BG);
    lcd_set_backlight(100);

#if HAS_ENCODER
    encoder_nav_init(PAGE_COUNT, PAGE_PERFORMANCE);
    encoder_nav_set_claim(claim_ui_command);
#endif
}

void display_task() {
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;

    static bool    first = true;
    static uint8_t last_page = 0xFF;

#if HAS_ENCODER
    uint8_t page = encoder_nav_page_index();
#else
    uint8_t page = PAGE_PERFORMANCE;
#endif
    // EDIT and MULTI take the faster rate so the encoder feels immediate.
    const PageRefreshCadence cadence{ page == PAGE_EDIT || page == PAGE_MULTI };
    next = make_timeout_time_ms(1000 / refresh_rate_hz(cadence));

    header_draw_module_name(hdr, "CHIP", LCD_W, COL_TITLE_FG, COL_TITLE);

    uint32_t snd = voice_alloc_active_mask();
    uint8_t  load = audio_engine_load();
    int      row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, __builtin_popcount(snd), MAX_VOICES, load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);

    header_draw_page_row(hdr, page, PAGE_NAMES[page < PAGE_COUNT ? page : 0], LCD_W,
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
        } else if (page == PAGE_EDIT) {
            quick_edit_page_invalidate(quick_edit_page);
        } else if (page == PAGE_MULTI) {
            multi_page_invalidate(multi_page);
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
    const PatchPlatformT<ChipModule> &platform = chip_platform();
    const uint8_t part = shown_part(m.last_channel);
    const uint32_t patch_key = (platform.revision(part) << 4) | part;

    if (page == PAGE_PERFORMANCE) {
        if (!preset_row.initialized || preset_row.value != patch_key) {
            char text[24];
            snprintf(text, sizeof(text), "#%-3d %.16s%s", (int)chip_multi().part_param(part, PART_P_PROGRAM),
                     platform.name(part),
                     platform.edited(part) ? "*" : "");
            value_row_draw_value(preset_row, patch_key, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);
        }

        // The FX belong to the Multi.
        const MultiT<ChipModule> &multi = chip_multi();
        const uint8_t fx_type = (uint8_t)multi.common(COMMON_P_FX_TYPE);
        const uint8_t fx_mix = (uint8_t)multi.common(COMMON_P_FX_MIX);
        const uint8_t fx_p1 = (uint8_t)multi.common(COMMON_P_FX_P1);
        const uint8_t fx_p2 = (uint8_t)multi.common(COMMON_P_FX_P2);
        label_draw_value(fx_type_label, fx_type, FX_TYPE_NAMES[fx_type < FX_COUNT ? fx_type : 0], BAR_X, FX_ROW1_Y,
                          14, 1, COL_VALUE, COL_BG);
        value_bar_draw_value(fxmix_bar, fx_mix, "FXMIX", fx_mix / 127.0f, BAR_X2, FX_ROW1_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

        value_bar_draw_value(fx_p1_bar, fx_p1, "FX P1", fx_p1 / 127.0f, BAR_X, FX_ROW2_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
        value_bar_draw_value(fx_p2_bar, fx_p2, "FX P2", fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y,
                              BAR_HALF_W, BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

        const uint8_t speaker = chip_speaker_preset_ui();
        const char *speaker_name = speaker < SPEAKER_PRESET_COUNT ? SPEAKER_PRESET_NAMES[speaker] : "?";
        label_draw_value(speaker_label, speaker, speaker_name, BAR_X, SPKR_Y, 14, 1, COL_VALUE, COL_BG);
    } else if (page == PAGE_EDIT) {
        quick_edit().follow_part(part);
        quick_edit_page_draw(quick_edit_page, quick_edit(), platform, kEditStyle);
    } else if (page == PAGE_MULTI) {
        multi_page_draw(multi_page, multi_edit(), chip_multi(), platform, kEditStyle);
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

        NoteKey note{ m.last_note, m.last_velocity, m.last_channel };
        if (!note_row.initialized || note_row.value != note) {
            char text[20];
            if (m.last_note == 0xFF) {
                snprintf(text, sizeof(text), "--");
            } else {
                int oct = m.last_note / 12 - 1;
                snprintf(text, sizeof(text), "%s%d v%d c%d", NOTE_NAMES[m.last_note % 12], oct,
                         m.last_velocity, m.last_channel + 1);
            }
            value_row_draw_value(note_row, note, text, BAR_X, NOTE_Y, 18, 2, COL_VALUE, COL_BG);
        }

        for (uint32_t v = 0; v < GRID_VOICES; v++) {
            ChipVoiceUiState ui;
            chip_voice_ui_state(v, &ui);
            const bool active_cell = ((snd >> v) & 1u) && ui.type != VT_SILENT;
            const GridCellKey key{ active_cell, ui.type, ui.part, ui.wave_pos, ui.held };
            if (!grid_cells[v].initialized || grid_cells[v].value != key) {
                const int x = (int)(v % 4) * GRID_W, y = GRID_ROW0 + (int)(v / 4) * GRID_ROW_H;
                char text[GRID_CH + 1];
                uint16_t fg = COL_LABEL;
                if (active_cell) {
                    snprintf(text, sizeof(text), "%lu:P%u/%u", (unsigned long)v, ui.part + 1u, ui.wave_pos);
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
