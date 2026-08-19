#include "midi_controller.h"
#include "midi_parser.h"
#include "../input_layer.h"
#include "../voice_alloc.h"
#include "presets.h"
#include "../osc/common.h"
#include "../osc/sample.h"
#include <cmath>

// --- Parser state ---
static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Per-channel state (16 MIDI channels) ---
// Default MIDI channel → preset mapping (channels beyond the list fall back
// to the last entry). Program change overrides channel_program[] at runtime.
static const uint8_t channel_preset[] = {
    PRESET_FAIRLIGHT,   // Ch 1
    PRESET_SQUARE_PWM,  // Ch 2
    PRESET_SAW_FILTER,  // Ch 3
};
static constexpr uint8_t NUM_CHANNEL_PRESETS = sizeof(channel_preset) / sizeof(channel_preset[0]);

static constexpr uint8_t NUM_CHANNELS = 16;
static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float PITCH_BEND_RANGE_SEMITONES = 2.0f;

static uint8_t channel_program[NUM_CHANNELS];   // current preset index per channel
static float   channel_bend_ratio[NUM_CHANNELS]; // phase_inc multiplier (1.0 = centered)
static int16_t channel_mod[NUM_CHANNELS];        // mod-wheel vibrato depth, Q15
static int16_t channel_pan[NUM_CHANNELS];        // CC10 pan, Q15 (-32768=L .. 0=center .. 32767=R)
static uint8_t channel_bank_msb[NUM_CHANNELS];   // CC0  — bank select MSB
static uint8_t channel_bank_lsb[NUM_CHANNELS];   // CC32 — bank select LSB

static uint8_t default_preset_for_channel(uint8_t ch) {
    return ch < NUM_CHANNEL_PRESETS ? channel_preset[ch] : channel_preset[NUM_CHANNEL_PRESETS - 1];
}

// microKORG program numbering: the program-change value's tens digit is the row
// (0-7) and the ones digit is the column (0-7), so a bank holds programs
// 0-7, 10-17, ... 70-77 (64 total; columns 8/9 and rows >7 are unused). Bank
// select adds another 64. Returns a linear slot 0-127, or -1 if the value isn't
// a valid microKORG program.
static int microkorg_slot(uint8_t bank, uint8_t pc) {
    uint8_t row = pc / 10, col = pc % 10;
    if (row > 7 || col > 7) return -1;
    return (int)(bank & 1) * 64 + row * 8 + col;
}

// --- UI snapshot (updated on each event, read by the display) ---
static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) {
    *out = ui_state;
}

// Map a 14-bit pitch bend value to a phase_inc multiplier.
static float bend_to_ratio(uint16_t bend14) {
    float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER) / (float)PITCH_BEND_CENTER
                      * PITCH_BEND_RANGE_SEMITONES;
    return powf(2.0f, semitones / 12.0f);
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

// Push mod-wheel depth to every held voice on a channel.
static void apply_channel_mod(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].mod_depth = channel_mod[channel];
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

// --- Input-layer mapping table (#84/#85) ---
// Pitch bend has no MIDI CC number of its own; it's given an id outside the
// 0-127 CC range so it can share the Modifier table with real CC-driven
// entries without colliding.
static constexpr uint8_t MOD_ID_PITCH_BEND = 128;

// Note setter: applies a resolved note on/off edge to the shadow block.
// Voice allocation and voice_held/midi_note_voice bookkeeping stay in
// midi_controller_process (allocator concerns, not shadow-parameter
// content) -- this only mutates VoiceParams, same split as #83 draws
// between edge classification and each engine's own action code.
//
// note/velocity arrive un-normalized (input_layer.h's fixed_velocity
// substitution mutator only knows how to substitute raw 0-127 velocity, so
// it has to run before any native-unit scaling); note is also an identity
// (Strike needs it to pick a sound, not just Note to compute a pitch), so
// it's not something to "normalize" away. The freq/amplitude math below is
// this setter's own normalization step, same as voice_apply_preset() already
// being setter-owned.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.note_on) {
        const VoicePreset &pr = presets[channel_program[value.channel]];
        float freq = 440.0f * powf(2.0f, (float)(value.note - 69) / 12.0f);
        uint32_t base = (pr.waveform == WAVE_SAMPLE && pr.sample)
            ? osc_sample_phase_inc(pr.sample, freq)
            : osc_phase_inc(freq);

        voice_base_inc[value.voice] = base;
        voice_channel[value.voice] = value.channel;

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
        ui_state.program = channel_program[value.channel];

        VoiceParams &vp = shadow.voices[value.voice];
        voice_apply_preset(vp, pr);
        vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[value.channel]);
        vp.amplitude = (int16_t)(value.velocity * 258);  // override with velocity
        vp.mod_depth = channel_mod[value.channel];
        vp.pan = channel_pan[value.channel];
        vp.trigger++;
        vp.gate = true;
    } else {
        shadow.voices[value.voice].gate = false;
    }
}

static void set_mod_wheel(VoiceParamBlock &shadow, const InputValue &value) {
    channel_mod[value.channel] = (int16_t)value.scalar;
    apply_channel_mod(shadow, value.channel);
}

static void set_pan(VoiceParamBlock &shadow, const InputValue &value) {
    channel_pan[value.channel] = (int16_t)value.scalar;
    apply_channel_pan(shadow, value.channel);
}

static void set_pitch_bend(VoiceParamBlock &shadow, const InputValue &value) {
    channel_bend_ratio[value.channel] = value.scalar;
    apply_channel_bend(shadow, value.channel);
}

// FX setters write shadow.fx -- one instance per VoiceParamBlock, not
// per-voice -- a true module-global Modifier, unlike mod wheel/pan/pitch
// bend above which are per-channel. Each takes the CC's raw 0-127 byte via
// value.scalar and does its own byte->native conversion, same as every
// other setter in this file.
static void set_fx_type(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.type = (uint8_t)((uint32_t)value.scalar * FX_COUNT / 128u);
    ui_state.fx_type = shadow.fx.type;
}

static void set_fx_mix(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.mix = (uint8_t)value.scalar;
    ui_state.fx_mix = shadow.fx.mix;
}

static void set_fx_p1(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.p1 = (uint8_t)value.scalar;
    ui_state.fx_p1 = shadow.fx.p1;
}

static void set_fx_p2(VoiceParamBlock &shadow, const InputValue &value) {
    shadow.fx.p2 = (uint8_t)value.scalar;
    ui_state.fx_p2 = shadow.fx.p2;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::NOTE,
    InputCategory::MODIFIER,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category            id_low              id_high             channel   fixed_vel  setter
    { InputCategory::NOTE,     0,                  127,                0xFF,     0,       set_note },
    { InputCategory::MODIFIER, 1,                  1,                  0xFF,     0,       set_mod_wheel },   // CC1: mod wheel
    { InputCategory::MODIFIER, 10,                 10,                 0xFF,     0,       set_pan },         // CC10: pan
    { InputCategory::MODIFIER, MOD_ID_PITCH_BEND,  MOD_ID_PITCH_BEND,  0xFF,     0,       set_pitch_bend },
    { InputCategory::MODIFIER, 72,                 72,                 0xFF,     0,       set_fx_p1 },       // CC72: FX param 1
    { InputCategory::MODIFIER, 73,                 73,                 0xFF,     0,       set_fx_mix },      // CC73: FX wet/dry mix
    { InputCategory::MODIFIER, 74,                 74,                 0xFF,     0,       set_fx_type },     // CC74: FX type select
    { InputCategory::MODIFIER, 75,                 75,                 0xFF,     0,       set_fx_p2 },       // CC75: FX param 2
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "subtractive mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_program[ch] = default_preset_for_channel(ch);
        channel_bend_ratio[ch] = 1.0f;
        channel_mod[ch] = 0;
        channel_pan[ch] = 0;
        channel_bank_msb[ch] = 0;
        channel_bank_lsb[ch] = 0;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = default_preset_for_channel(0);
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
                    InputValue value{};
                    value.channel = ev.channel;
                    value.note = note;
                    value.velocity = ev.data2;
                    value.voice = (int8_t)v;
                    value.note_on = true;
                    input_dispatch(shadow, kMappingTable, InputCategory::NOTE, note, value);
                    voice_held[v] = true;
                    changed = true;
                }
                break;
            }
            case MIDI_NOTE_OFF: {
                int8_t v = midi_note_voice[ev.data1];
                if (v >= 0) {
                    InputValue value{};
                    value.channel = ev.channel;
                    value.note = ev.data1;
                    value.voice = v;
                    value.note_on = false;
                    input_dispatch(shadow, kMappingTable, InputCategory::NOTE, ev.data1, value);
                    voice_held[v] = false;
                    voice_alloc_release(v);
                    midi_note_voice[ev.data1] = -1;
                    changed = true;
                }
                break;
            }
            case MIDI_CC: {
                switch (ev.data1) {
                    case 1: {  // mod wheel → vibrato depth
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)(ev.data2 * 258);  // 0..127 → ~0..32766
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 1, value);
                        ui_state.mod = ev.data2;
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    }
                    case 10: {  // pan (CC10) — 0=full left, 64=center, 127=full right
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)(((int32_t)ev.data2 - 64) * 512);
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 10, value);
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    }
                    case 74: {  // effect type select — split range into FX_COUNT bands
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)ev.data2;
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 74, value);
                        changed = true;
                        break;
                    }
                    case 73: {  // effect wet/dry mix (global)
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)ev.data2;
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 73, value);
                        changed = true;
                        break;
                    }
                    case 72: {  // effect param 1: delay feedback / reverb room size
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)ev.data2;
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 72, value);
                        changed = true;
                        break;
                    }
                    case 75: {  // effect param 2: delay time / reverb damping
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)ev.data2;
                        input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, 75, value);
                        changed = true;
                        break;
                    }
                    case 0:   channel_bank_msb[ev.channel] = ev.data2; break;
                    case 32:  channel_bank_lsb[ev.channel] = ev.data2; break;
                    default:  break;  // other CCs — ignored
                }
                break;
            }
            case MIDI_PITCH_BEND: {
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                InputValue value{};
                value.channel = ev.channel;
                value.scalar = bend_to_ratio(bend14);
                input_dispatch(shadow, kMappingTable, InputCategory::MODIFIER, MOD_ID_PITCH_BEND, value);
                ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                ui_state.last_channel = ev.channel;
                changed = true;
                break;
            }
            case MIDI_PROGRAM_CHANGE: {
                // microKORG numbering (row = tens digit, col = ones digit) plus
                // bank select. Affects future notes only. Bank comes from CC0
                // (MSB) — switch to channel_bank_lsb if the microKORG uses CC32.
                int slot = microkorg_slot(channel_bank_msb[ev.channel], ev.data1);
                if (slot >= 0) {
                    channel_program[ev.channel] = (uint8_t)(slot % PRESET_COUNT);
                    ui_state.program = channel_program[ev.channel];
                    ui_state.last_channel = ev.channel;
                }
                break;
            }
        }
    }

    if (changed) {
        params->commit();
    }
}
