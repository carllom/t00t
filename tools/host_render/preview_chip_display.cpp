// Host-buildable visual preview of chip's Performance/DIAG Page layout
// (src/engines/chip/display.cpp) against representative synthetic
// telemetry -- lets the layout be eyeballed as a PNG before flashing real
// hardware. Not a unit test: duplicates display.cpp's widget calls with
// fixed sample data, same reason preview_opl_display.cpp /
// preview_fm_display.cpp do.

#include "../../src/wslcd/header.h"
#include "../../src/wslcd/resource_bar.h"
#include "../../src/wslcd/value_row.h"
#include "../../src/wslcd/value_bar.h"
#include "../../src/wslcd/percentage_bar.h"
#include "../../src/wslcd/activity_grid.h"
#include "../../src/wslcd/label.h"
#include "lcd_st7789.h"

#include <cstdio>
#include <cstdint>

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(200, 120, 20);
static const uint16_t COL_TITLE_FG = gfx_rgb(255, 255, 255);
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL    = gfx_rgb(70, 130, 180);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_HELD    = gfx_rgb(255, 200, 60);
static const uint16_t COL_RINGING = gfx_rgb(120, 140, 160);

static constexpr int BODY_Y0 = 36;
static constexpr int PRESET_Y = BODY_Y0, FX_ROW1_Y = 60, FX_ROW2_Y = 82, SPKR_Y = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;
static constexpr int DIAG_BAR_W = 232;
static constexpr int CPU_Y = BODY_Y0, VOICES_Y = 60, VGRID_Y = 80, NOTE_Y = 112, GRID_ROW0 = 140;
static constexpr int GRID_ROW_H = 14, GRID_W = 240 / 4, GRID_CH = 9;
static constexpr int MAX_VOICES = 32;
static constexpr int GRID_VOICES = 8;

static const char *FX_TYPE_NAMES[3] = { "OFF", "DELAY", "REVERB" };
static const char *SPEAKER_NAMES[5] = { "1702 monitor", "portable TV", "Game Boy", "arcade cab",
                                         "bypass" };

// Writes lcd_stub_fb out as a binary PPM (P6): unswap gfx_rgb()'s wire-format
// byte order, then expand RGB565 back to 8-bit components.
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", LCD_W, LCD_H);
    for (uint32_t i = 0; i < (uint32_t)LCD_W * LCD_H; i++) {
        uint16_t wire = lcd_stub_fb[i];
        uint16_t v = (uint16_t)((wire >> 8) | (wire << 8));  // undo gfx_rgb()'s byte swap
        uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((v >> 5) & 0x3F) << 2);
        uint8_t b = (uint8_t)((v & 0x1F) << 3);
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static Header<2> hdr;
static ResourceBar resource_bar;

int main() {
    lcd_stub_clear(0);

    // Chrome: shared across both Pages.
    header_draw_module_name(hdr, "CHIP", LCD_W, COL_TITLE_FG, COL_TITLE);
    int active_voices = 11, load = 47;
    int row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, active_voices, MAX_VOICES, (float)load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);

    // ---- Performance page ----
    header_draw_page_row(hdr, 0, "PERF", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1,
                          8);

    ValueRow<int> preset_row;
    char text[24];
    snprintf(text, sizeof(text), "#%-3d %.10s", 4, "VIBRATO_LD");
    value_row_draw_value(preset_row, 4, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);

    int fx_mix = 55, fx_p1 = 90, fx_p2 = 30;
    ValueBar<int> fxmix_bar, fx_p1_bar, fx_p2_bar;
    Label<int> fx_type_label, speaker_label;

    label_draw_value(fx_type_label, 1, FX_TYPE_NAMES[1], BAR_X, FX_ROW1_Y, 14, 1, COL_VALUE, COL_BG);
    value_bar_draw_value(fxmix_bar, fx_mix, "FXMIX", fx_mix / 127.0f, BAR_X2, FX_ROW1_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(fx_p1_bar, fx_p1, "FX P1", fx_p1 / 127.0f, BAR_X, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
    value_bar_draw_value(fx_p2_bar, fx_p2, "FX P2", fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    label_draw_value(speaker_label, 2, SPEAKER_NAMES[2], BAR_X, SPKR_Y, 14, 1, COL_VALUE, COL_BG);

    write_ppm("/tmp/claude-1000/-home-carl-t00t/9b60f263-674b-4d9d-aa37-7211fbf4344a/scratchpad/chip_performance.ppm");

    // ---- DIAG page ----
    gfx_fill_rect(0, BODY_Y0, LCD_W, LCD_H - BODY_Y0, COL_BG);
    header_draw_page_row(hdr, 1, "DIAG", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1,
                          8);

    PercentageBar cpu_bar;
    percentage_bar_draw_value(cpu_bar, (float)load, "CPU", BAR_X, CPU_Y, DIAG_BAR_W, 20, 2,
                               COL_VALUE, COL_OFF);

    ValueRow<int> voices_row;
    char vtext[8];
    snprintf(vtext, sizeof(vtext), "%d/%d", active_voices, MAX_VOICES);
    value_row_draw_value(voices_row, active_voices, vtext, BAR_X, VOICES_Y, 8, 2, COL_VALUE, COL_BG);

    ActivityGrid<MAX_VOICES> voice_grid;
    bool active[MAX_VOICES] = {};
    for (int i = 0; i < active_voices; i++) active[i * 3 % MAX_VOICES] = true;
    activity_grid_draw(voice_grid, active, BAR_X, VGRID_Y, kActivityGridCellPitch, COL_SND, COL_OFF);

    ValueRow<int> note_row;
    char ntext[20];
    snprintf(ntext, sizeof(ntext), "%s%d v%d", "C", 4, 100);
    value_row_draw_value(note_row, 1, ntext, BAR_X, NOTE_Y, 18, 2, COL_VALUE, COL_BG);

    ValueRow<int> grid_cells[GRID_VOICES];
    for (int v = 0; v < GRID_VOICES; v++) {
        int col = v % 4, row = v / 4;
        int x = col * GRID_W, y = GRID_ROW0 + row * GRID_ROW_H;
        char gtext[GRID_CH + 1];
        bool cell_active = active[v];
        uint16_t fg = COL_LABEL;
        if (cell_active) {
            snprintf(gtext, sizeof(gtext), "%d:%d/%02d", v, v % 6 + 1, (v * 7) % 100);
            fg = (v % 2 == 0) ? COL_HELD : COL_RINGING;
        } else {
            gtext[0] = '\0';
        }
        value_row_draw_value(grid_cells[v], v * 10 + (cell_active ? 1 : 0), gtext, x, y, GRID_CH, 1,
                              fg, COL_BG);
    }

    write_ppm("/tmp/claude-1000/-home-carl-t00t/9b60f263-674b-4d9d-aa37-7211fbf4344a/scratchpad/chip_diag.ppm");

    return 0;
}
