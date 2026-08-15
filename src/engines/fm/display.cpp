#include "display.h"
#include "lcd_st7789.h"
#include "gfx.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "engine.h"
#include "pico/time.h"
#include <cstdio>

// FM status display (Core 0, low priority), module_fm.md "Display". Same
// chrome/status-row shape as the other engines' displays (VOICES/CPU/NOTE),
// plus FM-specific rows: the current patch (bank index + DX7 voice name,
// resolved for whichever channel most recently triggered a note) and its
// algorithm's operator roles (carrier/modulator/feedback, read straight off
// FmOpParams -- no algorithm number is stored anywhere at runtime, so this
// is derived, not looked up). A compact per-voice grid shows each sounding
// voice's channel and patch, which is what makes multitimbral use (several
// channels, different patches, at once) visible rather than assumed.
//
// MAX_VOICES here is 16 -- too many for a full one-cell-per-voice grid
// alongside everything else on this panel, so the grid below is fixed to
// voices 0-7. voice_alloc's allocate() always scans from v = 0 first
// (src/voice_alloc.cpp), so this covers whatever's sounding for anything up
// to 8-note polyphony; past that, the VOICES count row is still exact, only
// the grid stops being the full picture.

static const uint16_t COL_BG      = gfx_rgb(0, 0, 0);
static const uint16_t COL_TITLE   = gfx_rgb(30, 150, 70);   // green -- distinct from the other engines' bars
static const uint16_t COL_LABEL   = gfx_rgb(110, 120, 140);
static const uint16_t COL_VALUE   = gfx_rgb(240, 240, 240);
static const uint16_t COL_SND     = gfx_rgb(60, 220, 90);
static const uint16_t COL_OFF     = gfx_rgb(28, 28, 34);
static const uint16_t COL_LOAD_LO = gfx_rgb(60, 200, 90);
static const uint16_t COL_LOAD_MID = gfx_rgb(240, 180, 0);
static const uint16_t COL_LOAD_HI = gfx_rgb(230, 60, 50);
static const uint16_t COL_CARRIER = gfx_rgb(60, 220, 90);    // op writes to the final mix
static const uint16_t COL_MODULATOR = gfx_rgb(80, 180, 255); // op feeds another op
static const uint16_t COL_FEEDBACK = gfx_rgb(255, 200, 60);  // op self-modulates (either role)

static constexpr int VAL_X  = 104;
static constexpr int VAL_CH = 12;
static constexpr int ROW_VOICES = 36, ROW_CPU = 76, ROW_NOTE = 116;
static constexpr int ROW_PATCH = 156;
static constexpr int CBAR_X = 4, CBAR_Y = 96, CBAR_W = 232, CBAR_H = 12;

static constexpr int VBAR_Y = 56, VCELL_PITCH = 14, VCELL_W = 12, VBAR_H = 14;
static_assert(MAX_VOICES * VCELL_PITCH <= 240, "voice bar must fit LCD_W");

// NOTE's own value doesn't fit VAL_X/VAL_CH's scale-2 budget once the
// channel is appended -- worst case "C#-1 v127 c16" is 14 characters, and
// at scale 2 (16px/glyph) even flush against the label's right edge only
// leaves room for 11. Scale 1, positioned right after the label instead,
// same treatment the patch row above already needed.
static constexpr int NOTE_VAL_X = 68, NOTE_VAL_Y = ROW_NOTE + 4, NOTE_CH = 16;

// Patch name row: "#12 BRASS   1" -- index can run to 3 digits (up to 128
// patches from a multi-file conversion), name is DX7's fixed 10 characters.
// Scale 1, its own column (VAL_CH's scale-2 budget doesn't fit this).
static constexpr int PATCH_X = 0, PATCH_CH = 30;

// One cell per operator (FM_NUM_OPS == 6), colour = role. No per-algorithm
// diagram: carrier/modulator/feedback is what op.h/patch.h actually derive
// routing from, and it fits six small tiles legibly at MAX_SCALE.
static constexpr int ALGO_Y = 176, ALGO_CELL_PITCH = 24, ALGO_CELL_W = 20, ALGO_CELL_H = 16;

// 4 columns x 2 rows, voices 0-7 (see the cap's rationale above). Each cell:
// "voice:channel/patch" (channel 1-based for a player's sake), e.g. "3:2/07"
// -- voice 3, MIDI channel 2, patch index 7.
static constexpr int GRID_ROW0 = 210, GRID_ROW_H = 14, GRID_W = 240 / 4, GRID_CH = 9;
static constexpr int GRID_VOICES = 8;

static const char *NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void draw_val(int y, const char *raw, uint16_t fg) {
    char b[VAL_CH + 1];
    snprintf(b, sizeof(b), "%-*.*s", VAL_CH, VAL_CH, raw);
    gfx_text(VAL_X, y, b, fg, COL_BG, 2);
}

static void draw_grid_cell(uint32_t i, const char *text, uint16_t fg) {
    int col = (int)(i % 4), row = (int)(i / 4);
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
    gfx_text(0, ROW_VOICES, "VOICES", COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_CPU,    "CPU",    COL_LABEL, COL_BG, 2);
    gfx_text(0, ROW_NOTE,   "NOTE",   COL_LABEL, COL_BG, 2);

    lcd_set_backlight(100);
}

void display_task() {
    // Redraws at ~10 Hz -- plenty for a status monitor that isn't trying to
    // be an animation, and this is Core 0 wall-clock time, so it has no
    // effect on Core 1's render deadline either way.
    static absolute_time_t next = {0};
    if (!time_reached(next)) return;
    next = make_timeout_time_ms(100);

    static bool     first = true;
    static uint32_t last_snd = 0;
    static uint8_t  last_load = 0xFF;
    static MidiUiState last_midi = { 0xFE, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    static const FmPatch *last_patch = nullptr;
    static bool     last_grid_active[GRID_VOICES] = {};
    static uint8_t  last_grid_channel[GRID_VOICES] = {};
    static uint8_t  last_grid_program[GRID_VOICES] = {};

    uint32_t snd  = voice_alloc_active_mask();   // sounding (Core 1 gate bitmap)
    uint8_t  load = audio_engine_load();
    MidiUiState m;
    midi_controller_ui_state(&m);

    char buf[24];

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

    if (first || m.last_note != last_midi.last_note || m.last_velocity != last_midi.last_velocity
              || m.last_channel != last_midi.last_channel) {
        if (m.last_note == 0xFF) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            int oct = m.last_note / 12 - 1;
            snprintf(buf, sizeof(buf), "%s%d v%d c%d", NOTE_NAMES[m.last_note % 12], oct,
                     m.last_velocity, m.last_channel + 1);
        }
        char note_padded[NOTE_CH + 1];
        snprintf(note_padded, sizeof(note_padded), "%-*.*s", NOTE_CH, NOTE_CH, buf);
        gfx_text(NOTE_VAL_X, NOTE_VAL_Y, note_padded, COL_VALUE, COL_BG, 1);
    }

    // Patch + algorithm: whichever channel's Program Change/CC30 (or note-on)
    // landed most recently. A held voice keeps the patch pointer it started
    // with even if its channel's program changes mid-note, so this row is
    // "what's selected now" for the last-touched channel, not necessarily
    // what every sounding voice plays -- the per-voice grid below covers that.
    const FmPatch *patch = fm_channel_patch(m.last_channel);
    if (first || patch != last_patch) {
        char raw[24];
        snprintf(raw, sizeof(raw), "#%-3d %.10s", m.program, patch->name);
        char padded[PATCH_CH + 1];
        snprintf(padded, sizeof(padded), "%-*.*s", PATCH_CH, PATCH_CH, raw);
        gfx_text(PATCH_X, ROW_PATCH, padded, COL_VALUE, COL_BG, 1);

        for (uint8_t i = 0; i < FM_NUM_OPS; i++) {
            const FmOpParams &op = patch->op[i];
            bool carrier = (op.mod_target == FM_TARGET_OUT);
            uint16_t fg = carrier ? COL_CARRIER : COL_MODULATOR;
            uint16_t fill = op.feedback_level > 0 ? COL_FEEDBACK : fg;
            int x = i * ALGO_CELL_PITCH + 2;
            gfx_fill_rect(x, ALGO_Y, ALGO_CELL_W, ALGO_CELL_H, fill);
            char op_label[3];
            snprintf(op_label, sizeof(op_label), "%u", i + 1);
            gfx_text(x + 6, ALGO_Y + 3, op_label, COL_BG, fill, 1);
        }
        last_patch = patch;
    }
    last_midi = m;

    // Per-voice multitimbral grid: voices 0..GRID_VOICES-1, channel (1-based)
    // + patch index, only for voices actually sounding right now.
    for (uint32_t v = 0; v < GRID_VOICES; v++) {
        bool active = snd & (1u << v);
        FmVoiceUiState vs;
        fm_voice_ui_state(v, &vs);
        bool changed_cell = first || active != last_grid_active[v]
                          || (active && (vs.channel != last_grid_channel[v] || vs.program != last_grid_program[v]));
        if (changed_cell) {
            if (active) {
                snprintf(buf, sizeof(buf), "%u:%u/%u", v, vs.channel + 1, vs.program);
                draw_grid_cell(v, buf, COL_VALUE);
            } else {
                draw_grid_cell(v, "", COL_LABEL);
            }
            last_grid_active[v] = active;
            last_grid_channel[v] = vs.channel;
            last_grid_program[v] = vs.program;
        }
    }

    first = false;
}

void display_bringup_test() {}
