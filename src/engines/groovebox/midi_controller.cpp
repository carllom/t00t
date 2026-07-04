#include "midi_controller.h"
#include "midi_parser.h"
#include "engine.h"
#include "kit.h"
#include "osc/common.h"
#include <cmath>

// Groovebox MIDI routing (Core 0). Replaces the subtractive engine's
// poly-allocator routing with a fixed instrument map:
//   - Channel 10 (index 9): drum machine — note -> fixed voice via the kit table.
//   - Any other channel:     TB-303 — monophonic acid bass on voice GV_303.
//
// Shares the transport-agnostic MidiParser and the midi_controller.h interface;
// the transports (usb/uart) call midi_controller_process() unchanged.

static constexpr uint8_t DRUM_CHANNEL = 9;   // MIDI channel 10 (0-indexed)

static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float    PITCH_BEND_RANGE_SEMITONES = 2.0f;

static MidiParser midi_parser;

// --- TB-303 live state ---
static Tb303Preset g_303;          // mutated live by CCs
static int         g_303_note = -1; // currently sounding 303 note (-1 = none)
static float       g_303_bend = 1.0f;

// --- UI snapshot ---
static MidiUiState ui_state;
void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

static float note_to_freq(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

static void trigger_303(VoiceParamBlock &shadow, uint8_t note, uint8_t velocity) {
    VoiceParams &vp = shadow.voices[GV_303];
    uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq(note)) * g_303_bend);
    apply_303(vp, g_303, inc, velocity);
    vp.trigger++;
    vp.gate = true;
    g_303_note = note;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = 0;
}

static void trigger_drum(VoiceParamBlock &shadow, uint8_t note, uint8_t velocity) {
    const KitInstrument *k = kit_find(kit_808, KIT_808_COUNT, note);
    if (!k) return;
    VoiceParams &vp = shadow.voices[k->voice];
    apply_kit(vp, *k, velocity);
    vp.trigger++;
    vp.gate = true;

    // Closed hat chokes the open hat (shared 808 hi-hat circuit): cut its gate
    // so its envelope releases quickly on the next render.
    if (k->voice == GV_HAT_CLOSED) {
        shadow.voices[GV_HAT_OPEN].gate = false;
    }

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = DRUM_CHANNEL;
}

void midi_controller_init() {
    midi_parser.init();
    g_303 = tb303_default;
    g_303_note = -1;
    g_303_bend = 1.0f;

    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = 0;
    ui_state.bend = 0;
    ui_state.mod = 0;
    ui_state.fx_type = FX_DELAY;
    ui_state.fx_mix = 0;
    ui_state.fx_p1 = 55;
    ui_state.fx_p2 = 36;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    if (len == 0) return;

    bool changed = false;
    VoiceParamBlock &shadow = params->shadow();
    shadow = params->active();

    for (uint32_t i = 0; i < len; i++) {
        MidiEvent ev;
        if (!midi_parser.feed(data[i], ev)) continue;

        switch (ev.type) {
            case MIDI_NOTE_ON:
                if (ev.channel == DRUM_CHANNEL) {
                    trigger_drum(shadow, ev.data1, ev.data2);
                } else {
                    trigger_303(shadow, ev.data1, ev.data2);
                }
                changed = true;
                break;

            case MIDI_NOTE_OFF:
                if (ev.channel != DRUM_CHANNEL) {
                    // 303: release only if this is the note currently sounding.
                    if ((int)ev.data1 == g_303_note) {
                        shadow.voices[GV_303].gate = false;
                        g_303_note = -1;
                        changed = true;
                    }
                }
                // Drums are one-shot: note-off is ignored.
                break;

            case MIDI_CC:
                switch (ev.data1) {
                    case 16: {  // 303 cutoff: 0..127 -> 100..5000 Hz
                        g_303.cutoff = (uint16_t)(100 + (uint32_t)ev.data2 * 4900u / 127u);
                        if (shadow.voices[GV_303].type == VT_TB303)
                            shadow.voices[GV_303].filter_cutoff = g_303.cutoff;
                        changed = true;
                        break;
                    }
                    case 17: {  // 303 resonance: 0..127 -> 0..32766
                        g_303.resonance = (uint16_t)(ev.data2 * 258);
                        if (shadow.voices[GV_303].type == VT_TB303)
                            shadow.voices[GV_303].filter_resonance = g_303.resonance;
                        changed = true;
                        break;
                    }
                    case 74:  // effect type select
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case 73:  // effect wet/dry mix
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case 72:  // effect param 1
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case 75:  // effect param 2
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    default: break;
                }
                break;

            case MIDI_PITCH_BEND: {
                if (ev.channel != DRUM_CHANNEL) {
                    uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                    float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER)
                                      / (float)PITCH_BEND_CENTER * PITCH_BEND_RANGE_SEMITONES;
                    g_303_bend = powf(2.0f, semitones / 12.0f);
                    if (g_303_note >= 0 && shadow.voices[GV_303].type == VT_TB303) {
                        uint32_t inc = (uint32_t)((float)osc_phase_inc(note_to_freq((uint8_t)g_303_note)) * g_303_bend);
                        shadow.voices[GV_303].phase_inc = inc;
                    }
                    ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                    changed = true;
                }
                break;
            }

            case MIDI_PROGRAM_CHANGE:
                // Kit / preset switching — to be added with the 909 kit.
                break;
        }
    }

    if (changed) params->commit();
}
