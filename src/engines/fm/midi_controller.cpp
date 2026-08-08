#include "midi_controller.h"
#include "midi_parser.h"
#include "patch.h"
#include "sine_tab.h"
#include "voice_alloc.h"
#include <cmath>

// FM MIDI controller (#44, fm.md P1): note on/off + pitch bend + velocity,
// mirroring the shared src/midi/midi_controller.cpp's channel-bend/pan
// pattern. Own controller (not the shared one) for the same reason the #41
// skeleton stub gave: this engine's VoiceParams carries a patch pointer, not
// a VoicePreset the shared controller's presets.h shape expects. Every
// note-on uses FM_TEST_PATCH (patch.h) -- #44's "one hardcoded 6-op patch";
// per-channel/per-program patch selection is P3+ territory, once
// tools/syx2patch.py gives this engine more than one patch to choose from.

static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Per-channel state (16 MIDI channels) ---
static constexpr uint8_t NUM_CHANNELS = 16;
static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float PITCH_BEND_RANGE_SEMITONES = 2.0f;

static float   channel_bend_ratio[NUM_CHANNELS];  // phase_inc multiplier (1.0 = centered)
static int16_t channel_pan[NUM_CHANNELS];         // CC10 pan, Q15

// --- UI snapshot (updated on each event, read by the display) ---
static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

// Map a 14-bit pitch bend value to a phase_inc multiplier.
static float bend_to_ratio(uint16_t bend14) {
    float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER) / (float)PITCH_BEND_CENTER
                      * PITCH_BEND_RANGE_SEMITONES;
    return powf(2.0f, semitones / 12.0f);
}

static void midi_voice_on(VoiceParamBlock &shadow, int v, uint8_t note, uint8_t velocity, uint8_t channel) {
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    uint32_t base = fm_phase_inc(freq);

    voice_base_inc[v] = base;
    voice_channel[v] = channel;
    voice_held[v] = true;

    VoiceParams &vp = shadow.voices[v];
    vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[channel]);
    vp.amplitude = (int16_t)(velocity * 258);  // 0..127 -> ~0..32766, "velocity as plain amplitude" (#44)
    vp.pan = channel_pan[channel];
    vp.patch = &FM_TEST_PATCH;
    vp.trigger++;
    vp.gate = true;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = channel;
    ui_state.program = 0;  // one patch until P3
}

// Re-scale phase_inc for every held voice on a channel after a bend change.
static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].phase_inc =
                (uint32_t)((float)voice_base_inc[v] * channel_bend_ratio[channel]);
        }
    }
}

// Push pan (CC10) to every held voice on a channel.
static void apply_channel_pan(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].pan = channel_pan[channel];
        }
    }
}

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bend_ratio[ch] = 1.0f;
        channel_pan[ch] = 0;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = 0;
    ui_state.bend = 0;
    ui_state.mod = 0;
    // Match ParamExchange::init() fx defaults (delay, dry, p1=55, p2=36≈300 ms).
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
            case MIDI_NOTE_ON: {
                uint8_t note = ev.data1;
                if (midi_note_voice[note] >= 0) {
                    int8_t old = midi_note_voice[note];
                    shadow.voices[old].gate = false;
                    voice_held[old] = false;
                    voice_alloc_release(old);
                }
                int v = voice_alloc_allocate();
                if (v >= 0) {
                    midi_note_voice[note] = (int8_t)v;
                    midi_voice_on(shadow, v, note, ev.data2, ev.channel);
                    changed = true;
                }
                break;
            }
            case MIDI_NOTE_OFF: {
                int8_t v = midi_note_voice[ev.data1];
                if (v >= 0) {
                    shadow.voices[v].gate = false;
                    voice_held[v] = false;
                    voice_alloc_release(v);
                    midi_note_voice[ev.data1] = -1;
                    changed = true;
                }
                break;
            }
            case MIDI_CC: {
                switch (ev.data1) {
                    case 10:  // pan (CC10) — 0=full left, 64=center, 127=full right
                        channel_pan[ev.channel] = (int16_t)(((int32_t)ev.data2 - 64) * 512);
                        apply_channel_pan(shadow, ev.channel);
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 74:  // effect type select — split range into FX_COUNT bands
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case 73:  // effect wet/dry mix (global)
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case 72:  // effect param 1: delay feedback / reverb room size
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case 75:  // effect param 2: delay time / reverb damping
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    default:  break;  // other CCs — to be mapped later (patch select, etc., P3+)
                }
                break;
            }
            case MIDI_PITCH_BEND: {
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                channel_bend_ratio[ev.channel] = bend_to_ratio(bend14);
                apply_channel_bend(shadow, ev.channel);
                ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                ui_state.last_channel = ev.channel;
                changed = true;
                break;
            }
            default: break;
        }
    }

    if (changed) {
        params->commit();
    }
}
