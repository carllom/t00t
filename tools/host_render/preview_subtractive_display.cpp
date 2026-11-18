// Host-buildable visual preview of subtractive's Pages
// (src/engines/subtractive/display.cpp) against representative synthetic
// telemetry -- lets the layout be eyeballed as an image before flashing real
// hardware. Not a unit test. The Performance and DIAG Pages repeat
// display.cpp's widget calls with fixed sample data, since display.cpp
// itself needs the device's telemetry; the EDIT Page is drawn by the
// device's own quick_edit_page_draw(), driven by UI commands into the real
// subtractive module on RAM-backed flash.
//
//   preview_subtractive_display [out_dir]   writes subtractive_*.ppm (default: .)

#include "../../src/wslcd/header.h"
#include "../../src/wslcd/resource_bar.h"
#include "../../src/wslcd/value_row.h"
#include "../../src/wslcd/value_bar.h"
#include "../../src/wslcd/percentage_bar.h"
#include "../../src/wslcd/activity_grid.h"
#include "../../src/wslcd/label.h"
#include "lcd_st7789.h"
#include "factory_bank.h"
#include "patch.h"
#include "patch/quick_edit_page.h"
#include "ram_flash.h"

#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <string>

static const uint16_t COL_BG       = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE    = gfx_rgb(30, 90, 160);
static const uint16_t COL_TITLE_FG = gfx_rgb(255, 255, 255);
static const uint16_t COL_VALUE    = gfx_rgb(240, 240, 240);
static const uint16_t COL_OFF      = gfx_rgb(28, 28, 34);
static const uint16_t COL_FILL     = gfx_rgb(70, 130, 180);
static const uint16_t COL_SND      = gfx_rgb(60, 220, 90);
static const uint16_t COL_SEL      = gfx_rgb(220, 150, 30);
static const uint16_t COL_SEL_OFF  = gfx_rgb(60, 40, 10);
static const uint16_t COL_ACT      = gfx_rgb(40, 180, 80);
static const uint16_t COL_ACT_OFF  = gfx_rgb(10, 50, 20);

static constexpr int BODY_Y0 = 36;
static constexpr int PRESET_Y = BODY_Y0, FX_ROW1_Y = 60, FX_ROW2_Y = 82, MOD_Y = 104;
static constexpr int BAR_X = 4, BAR_X2 = 122, BAR_HALF_W = 114, BAR_H = 16;
static constexpr int DIAG_BAR_W = 232;
static constexpr int CPU_Y = BODY_Y0, VOICES_Y = 60, VGRID_Y = 80, NOTE_Y = 104;
static constexpr int PREVIEW_VOICES = 16;  // MAX_VOICES, without engine.h's own

static const char *FX_TYPE_NAMES[3] = { "OFF", "DELAY", "REVERB" };

// Writes lcd_stub_fb out as a binary PPM (P6): unswap gfx_rgb()'s wire-format
// byte order, then expand RGB565 back to 8-bit components.
static std::string g_out_dir = ".";

static void write_ppm(const char *file) {
    const std::string path = g_out_dir + "/" + file;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
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
    printf("wrote %s\n", path.c_str());
}

static Header<3> hdr;

PartSlotT<SubCookedPatch> sub_part_slots[SUB_PART_COUNT];

// The EDIT Page in a few representative states, drawn incrementally the way
// display_task() draws it, one image per state.
static void preview_edit_page() {
    static RamFlash flash;
    static UserBank user(ModuleId::SUBTRACTIVE, user_flash(flash));
    static const FactoryBank banks[] = { { kSubFactoryBank, kSubFactoryCount } };
    static PatchPlatformT<SubtractiveModule> platform(sub_part_slots, banks, 1, &user);
    static QuickEditT<SubtractiveModule> qe(platform);
    static QuickEditPage<SubtractiveModule> pg;
    const QuickEditPageStyle style = { COL_VALUE, COL_BG, COL_FILL, COL_OFF, COL_SEL, COL_SEL_OFF,
                                       COL_ACT, COL_ACT_OFF, BAR_X, BODY_Y0, DIAG_BAR_W, LCD_H };
    platform.load_factory(0, 0, 2);  // Saw-Flt
    user.store_patch(0, kSubFactoryBank[0].data, kSubFactoryBank[0].len);  // U000 holds Fairlite

    gfx_fill_rect(0, BODY_Y0, LCD_W, LCD_H - BODY_Y0, COL_BG);
    header_draw_page_row(hdr, 1, "EDIT", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1, 8);
    quick_edit_page_invalidate(pg);
    auto step = [&](std::initializer_list<UiCommand> commands, const char *file) {
        for (UiCommand c : commands) qe.command(c);
        quick_edit_page_draw(pg, qe, platform, style);
        if (file) write_ppm(file);
    };
    using C = UiCommand;
    step({}, "subtractive_edit_browse.ppm");
    // CUTOFF row (WAVE, LFO RATE, CUTOFF): edit it down a few steps.
    step({ C::ENTER, C::PLUS, C::PLUS, C::ENTER, C::MINUS, C::MINUS, C::MINUS }, "subtractive_edit_value.ppm");
    step({ C::ENTER, C::PLUS, C::PLUS, C::PLUS }, "subtractive_edit_compare_row.ppm");
    // STORE, past U000 (taken) to U001, rename the first letter.
    step({ C::PLUS, C::ENTER, C::ENTER, C::PLUS, C::ENTER, C::PLUS }, "subtractive_edit_store_name.ppm");
}
static ResourceBar resource_bar;

int main(int argc, char **argv) {
    if (argc > 1) g_out_dir = argv[1];
    lcd_stub_clear(0);

    // Chrome: shared across both Pages.
    header_draw_module_name(hdr, "SUBTRACTIVE", LCD_W, COL_TITLE_FG, COL_TITLE);
    int active_voices = 6, load = 38;
    int row1_margin = gfx_corner_safe_margin(kHeaderRow1Y, kHeaderRow1Y + kHeaderRow1H);
    resource_bar_draw_value(resource_bar, active_voices, PREVIEW_VOICES, (float)load,
                             LCD_W - kResourceBarMaxPx - row1_margin,
                             kHeaderRow1Y + (kHeaderRow1H - 8) / 2, 8, COL_BG);

    // ---- Performance page ----
    header_draw_page_row(hdr, 0, "PERF", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1,
                          8);

    ValueRow<int> preset_row;
    char text[24];
    snprintf(text, sizeof(text), "#%-3d %.10s", 2, "Saw-Flt");
    value_row_draw_value(preset_row, 2, text, 4, PRESET_Y, 28, 1, COL_VALUE, COL_BG);

    int fx_mix = 70, fx_p1 = 55, fx_p2 = 20, mod = 60;
    ValueBar<int> fxmix_bar, fx_p1_bar, fx_p2_bar, mod_bar;
    Label<int> fx_type_label;

    label_draw_value(fx_type_label, 1, FX_TYPE_NAMES[1], BAR_X, FX_ROW1_Y, 14, 1, COL_VALUE, COL_BG);
    value_bar_draw_value(fxmix_bar, fx_mix, "FXMIX", fx_mix / 127.0f, BAR_X2, FX_ROW1_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(fx_p1_bar, fx_p1, "FX P1", fx_p1 / 127.0f, BAR_X, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);
    value_bar_draw_value(fx_p2_bar, fx_p2, "FX P2", fx_p2 / 127.0f, BAR_X2, FX_ROW2_Y, BAR_HALF_W,
                          BAR_H, 1, COL_VALUE, COL_FILL, COL_OFF);

    value_bar_draw_value(mod_bar, mod, "MOD", mod / 127.0f, BAR_X, MOD_Y, BAR_HALF_W, BAR_H, 1,
                          COL_VALUE, COL_FILL, COL_OFF);

    write_ppm("subtractive_performance.ppm");

    // ---- DIAG page ----
    gfx_fill_rect(0, BODY_Y0, LCD_W, LCD_H - BODY_Y0, COL_BG);
    header_draw_page_row(hdr, 2, "DIAG", LCD_W, COL_TITLE_FG, COL_TITLE, COL_TITLE_FG, COL_OFF, 1,
                          8);

    PercentageBar cpu_bar;
    percentage_bar_draw_value(cpu_bar, (float)load, "CPU", BAR_X, CPU_Y, DIAG_BAR_W, 20, 2,
                               COL_VALUE, COL_OFF);

    ValueRow<int> voices_row;
    char vtext[8];
    snprintf(vtext, sizeof(vtext), "%d/%d", active_voices, PREVIEW_VOICES);
    value_row_draw_value(voices_row, active_voices, vtext, BAR_X, VOICES_Y, 8, 2, COL_VALUE, COL_BG);

    ActivityGrid<PREVIEW_VOICES> voice_grid;
    bool active[PREVIEW_VOICES] = { true, true, false, true, false, true, false, false,
                                 false, false, false, false, false, false, false, false };
    activity_grid_draw(voice_grid, active, BAR_X, VGRID_Y, kActivityGridCellPitch, COL_SND, COL_OFF);

    ValueRow<int> note_row;
    char ntext[20];
    snprintf(ntext, sizeof(ntext), "%s%d v%d", "C", 4, 100);
    value_row_draw_value(note_row, 1, ntext, BAR_X, NOTE_Y, 18, 2, COL_VALUE, COL_BG);

    write_ppm("subtractive_diag.ppm");

    preview_edit_page();
    return 0;
}
