// Host-buildable visual preview of OPL's new Performance/DIAG Page layout
// (src/engines/opl/display.cpp) against representative synthetic telemetry
// -- lets the layout be eyeballed as a PNG before flashing real hardware.
// Not a unit test: duplicates display.cpp's widget calls with fixed sample
// data (display.cpp itself pulls live pico-sdk/voice_alloc/midi_controller
// state that isn't host-buildable), same reason test_header.cpp and friends
// call Widgets directly rather than exercising a real engine's display.cpp.

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

static const uint16_t COL_BG        = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE     = gfx_rgb(150, 90, 30);
static const uint16_t COL_TITLE_FG  = gfx_rgb(255, 255, 255);
static const uint16_t COL_VALUE     = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND       = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF       = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL      = gfx_rgb(70, 130, 180);
static const uint16_t COL_CARRIER   = gfx_rgb(60, 220, 90);
static const uint16_t COL_MODULATOR = gfx_rgb(80, 180, 255);
static const uint16_t COL_FEEDBACK  = gfx_rgb(255, 200, 60);

static constexpr int BODY_Y0 = 36;
static constexpr int PRESET_Y = BODY_Y0, FX_ROW1_Y = 60, FX_ROW2_Y = 82, MOD_Y = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;
static constexpr int DIAG_BAR_W = 232;
static constexpr int CPU_Y = BODY_Y0, VOICES_Y = 60, VGRID_Y = 80, NOTE_Y = 104, ALGO_Y = 132;
static constexpr int ALGO_CELL_PITCH = 24, ALGO_CELL_W = 20, ALGO_CELL_H = 16;
static constexpr int MAX_VOICES = 9;

static const char *FX_TYPE_NAMES[3] = { "OFF", "DELAY", "REVERB" };

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

static void draw_algo_indicator(int algorithm_add, int feedback) {
    for (int i = 0; i < 2; i++) {
        bool carrier = algorithm_add || (i == 1);
        uint16_t fg = carrier ? COL_CARRIER : COL_MODULATOR;
        uint16_t fill = (i == 0 && feedback > 0) ? COL_FEEDBACK : fg;
        int x = i * ALGO_CELL_PITCH + 2;
        gfx_fill_rect(x, ALGO_Y, ALGO_CELL_W, ALGO_CELL_H, fill);
        char op_label[3];
        snprintf(op_label, sizeof(op_label), "%d", i + 1);
        gfx_text(x + 6, ALGO_Y + 3, op_label, COL_BG, fill, 1);
    }
}

int main() {
    lcd_stub_clear(0);

    // Chrome: shared across both Pages.
    header_draw_module_name(hdr, "OPL", LCD_W, COL_TITLE_FG, COL_TITLE);
    int active_voices = 5, load = 62;
    int row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, active_voices, MAX_VOICES, (float)load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);

    // ---- Performance page ----
    header_draw_page_row(hdr, 0, "PERF", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1,
                          8);

    ValueRow<int> preset_row;
    char text[24];
    snprintf(text, sizeof(text), "#%-3d %.10s", 3, "OPL BELL");
    value_row_draw_value(preset_row, 3, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);

    int fx_mix = 91, fx_p1 = 40, fx_p2 = 64, mod = 127;
    ValueBar<int> fxmix_bar, fx_p1_bar, fx_p2_bar, mod_bar;
    Label<int> fx_type_label;

    label_draw_value(fx_type_label, 2, FX_TYPE_NAMES[2], BAR_X, FX_ROW1_Y, 14, 1, COL_VALUE, COL_BG);
    value_bar_draw_value(fxmix_bar, fx_mix, "FXMIX", fx_mix / 127.0f, BAR_X2, FX_ROW1_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(fx_p1_bar, fx_p1, "FX P1", fx_p1 / 127.0f, BAR_X, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
    value_bar_draw_value(fx_p2_bar, fx_p2, "FX P2", fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(mod_bar, mod, "MOD", mod / 127.0f, BAR_X, MOD_Y, BAR_HALF_W, BAR_H, 1,
                          COL_VALUE, COL_FILL, COL_OFF);

    write_ppm("/tmp/claude-1000/-home-carl-t00t/9b60f263-674b-4d9d-aa37-7211fbf4344a/scratchpad/opl_performance.ppm");

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
    bool active[MAX_VOICES] = { true, true, false, true, false, true, false, false, true };
    activity_grid_draw(voice_grid, active, BAR_X, VGRID_Y, kActivityGridCellPitch, COL_SND, COL_OFF);

    ValueRow<int> note_row;
    char ntext[20];
    snprintf(ntext, sizeof(ntext), "%s%d v%d c%d", "C", 4, 100, 1);
    value_row_draw_value(note_row, 1, ntext, BAR_X, NOTE_Y, 18, 2, COL_VALUE, COL_BG);

    draw_algo_indicator(0, 3);  // FM algorithm, feedback 3 -- OPL_PATCH_LEAD's own shape

    write_ppm("/tmp/claude-1000/-home-carl-t00t/9b60f263-674b-4d9d-aa37-7211fbf4344a/scratchpad/opl_diag.ppm");

    return 0;
}
