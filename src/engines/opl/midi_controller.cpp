#include "midi_controller.h"
#include "midi_parser.h"
#include "../fm/sine_tab.h"  // fm_phase_inc(), reused for the mult=1 base increment
#include "patches.h"
#include "voice_alloc.h"
#include <cmath>

// OPL MIDI controller: note on/off + pitch bend + velocity + patch select.
// Own controller, not the shared src/midi/midi_controller.cpp: this engine's
// VoiceParams carries a patch pointer, not a VoicePreset the shared
// controller's presets.h shape expects.
//
// Patch select: CC16 and Program Change both pick
// `OPL_PATCHES[value % OPL_PATCH_COUNT]` -- CC16 exists for controllers whose
// encoders send absolute CC rather than a real Program Change. patches.h is
// hand-authored and checked in, so patch select is unconditional here, not
// gated behind a "has this been generated" macro.

static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];
static bool     voice_held[MAX_VOICES];

// --- Per-channel state (16 MIDI channels) ---
static constexpr uint8_t NUM_CHANNELS = 16;
static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float PITCH_BEND_RANGE_SEMITONES = 2.0f;

static float   channel_bend_ratio[NUM_CHANNELS];
static int16_t channel_pan[NUM_CHANNELS];
static const OplPatch *channel_patch[NUM_CHANNELS];
static uint8_t channel_program[NUM_CHANNELS];
static int16_t channel_mod_wheel[NUM_CHANNELS];

static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

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
    vp.amplitude = (int16_t)(velocity * 258);  // 0..127 -> ~0..32766, plain amplitude scaling
    vp.pan = channel_pan[channel];
    vp.patch = channel_patch[channel];
    vp.note = note;
    vp.mod_wheel = channel_mod_wheel[channel];
    vp.trigger++;
    vp.gate = true;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = channel;
    ui_state.program = channel_program[channel];
}

static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].phase_inc =
                (uint32_t)((float)voice_base_inc[v] * channel_bend_ratio[channel]);
        }
    }
}

static void apply_channel_pan(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].pan = channel_pan[channel];
        }
    }
}

static void apply_channel_mod_wheel(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].mod_wheel = channel_mod_wheel[channel];
        }
    }
}

static void select_patch(VoiceParamBlock &shadow, uint8_t channel, uint8_t value) {
    (void)shadow;
    uint8_t idx = (uint8_t)(value % OPL_PATCH_COUNT);
    channel_patch[channel] = OPL_PATCHES[idx];
    channel_program[channel] = idx;
    ui_state.program = idx;
    ui_state.last_channel = channel;
}

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bend_ratio[ch] = 1.0f;
        channel_pan[ch] = 0;
        channel_patch[ch] = &OPL_PATCH_LEAD;
        channel_program[ch] = 0;
        channel_mod_wheel[ch] = 0;
    }
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
                    case 1:  // mod wheel -- scales vibrato depth, live
                        channel_mod_wheel[ev.channel] = (int16_t)(ev.data2 * 258);
                        apply_channel_mod_wheel(shadow, ev.channel);
                        ui_state.mod = ev.data2;
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 10:  // pan
                        channel_pan[ev.channel] = (int16_t)(((int32_t)ev.data2 - 64) * 512);
                        apply_channel_pan(shadow, ev.channel);
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 16:  // patch select -- CC alternative to Program Change
                        select_patch(shadow, ev.channel, ev.data2);
                        changed = true;
                        break;
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
                    default: break;
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
            case MIDI_PROGRAM_CHANGE: {
                select_patch(shadow, ev.channel, ev.data1);
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

const OplPatch *opl_channel_patch(uint8_t channel) {
    return channel < NUM_CHANNELS ? channel_patch[channel] : &OPL_PATCH_LEAD;
}
