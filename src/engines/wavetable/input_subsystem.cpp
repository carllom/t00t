#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "tables.h"
#include "osc/wavetable.h"
#include <cmath>

// Wavetable engine's Input subsystem: the module-specific tail of the Input
// pipeline -- mapping table, Handlers, and the Voice Allocation Interface
// calls they make. Mirrors src/engines/opl/input_subsystem.cpp's shape
// closely (same NOTE/MODIFIER/CONFIGURATION capability set), with one
// module-specific substitution: CC1 (mod wheel) drives a live wave-position
// scan instead of a vibrato depth -- this skeleton has no LFO yet, and a
// continuous wave-position sweep is the module's own PPG-style analogue
// (module_wavetable.md's Overview).

static int8_t note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Per-channel state (16 MIDI channels) ---
static constexpr uint8_t NUM_CHANNELS = 16;

static float             channel_bend_ratio[NUM_CHANNELS];  // phase_inc multiplier (1.0 = centered)
static int16_t            channel_pan[NUM_CHANNELS];         // CC10 pan, Q15
static const WaveTable   *channel_table[NUM_CHANNELS];       // defaults to &WT_BANK[0], Program Change overrides it
static uint8_t             channel_program[NUM_CHANNELS];    // WT_BANK[] index, for ui_state.program
static uint8_t             channel_wave_pos[NUM_CHANNELS];   // CC1: live wave-position scan, 0..WT_INDEX_SIZE-1

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

// Push wave-position (CC1) to every held voice on a channel -- live scan.
static void apply_channel_wave_pos(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].wave_pos = channel_wave_pos[channel];
        }
    }
}

// Note setter: the Voice Allocation Interface -- this Handler is where
// voice_alloc_allocate()/release() actually get called, never upstream in
// parsing/dispatch (CONTEXT.md's "Voice Allocation Interface" entry).
// Resolves its own voice via note_voice[], keyed by note number.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.note_on) {
        // Retrigger: steal whatever voice is already sounding this note.
        if (note_voice[value.note] >= 0) {
            int8_t old = note_voice[value.note];
            shadow.voices[old].gate = false;
            voice_held[old] = false;
            voice_alloc_release(old);
        }
        int v = voice_alloc_allocate();
        if (v < 0) return;
        note_voice[value.note] = (int8_t)v;
        voice_held[v] = true;

        float freq = 440.0f * powf(2.0f, (float)(value.note - 69) / 12.0f);
        uint32_t base = wavetable_phase_inc(freq);

        voice_base_inc[v] = base;
        voice_channel[v] = value.channel;

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
        ui_state.program = channel_program[value.channel];

        VoiceParams &vp = shadow.voices[v];
        vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[value.channel]);
        vp.wave_pos = channel_wave_pos[value.channel];
        vp.amplitude = (int16_t)(value.velocity * 258);  // 0..127 -> ~0..32766, plain amplitude scaling
        vp.pan = channel_pan[value.channel];
        vp.table = channel_table[value.channel];
        vp.trigger++;
        vp.gate = true;
    } else {
        int8_t v = note_voice[value.note];
        if (v < 0) return;
        shadow.voices[v].gate = false;
        voice_held[v] = false;
        voice_alloc_release(v);
        note_voice[value.note] = -1;
    }
}

// Modifier setters take the raw 0-127 CC byte via value.scalar and do their
// own source-native -> module-native conversion and ui_state mirroring.
static void set_wave_pos(VoiceParamBlock &shadow, const InputValue &value) {
    channel_wave_pos[value.channel] =
        (uint8_t)((uint32_t)value.scalar * (WT_INDEX_SIZE - 1) / 127u);
    apply_channel_wave_pos(shadow, value.channel);
    ui_state.mod = (uint8_t)value.scalar;
    ui_state.last_channel = value.channel;
}

static void set_pan(VoiceParamBlock &shadow, const InputValue &value) {
    channel_pan[value.channel] = (int16_t)(((int32_t)value.scalar - 64) * 512);  // 0=full left, 64=center, 127=full right
    apply_channel_pan(shadow, value.channel);
    ui_state.last_channel = value.channel;
}

static void set_pitch_bend(VoiceParamBlock &shadow, const InputValue &value) {
    channel_bend_ratio[value.channel] = value.scalar;
    apply_channel_bend(shadow, value.channel);
}

// FX setters write shadow.fx -- one instance per VoiceParamBlock, not
// per-voice -- a true module-global Modifier, unlike wave-pos/pan/pitch
// bend above which are per-channel.
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

// Table select: Program Change picks WT_BANK[value.index % WT_BANK_COUNT]
// (tables.h) for future notes on the channel. Doesn't touch the shadow
// block itself, so callers don't need to commit for it alone.
static void set_table(VoiceParamBlock &, const InputValue &value) {
    uint8_t idx = (uint8_t)(value.index % WT_BANK_COUNT);
    channel_table[value.channel] = &WT_BANK[idx];
    channel_program[value.channel] = idx;
    ui_state.program = idx;
    ui_state.last_channel = value.channel;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::NOTE,
    InputCategory::MODIFIER,
    InputCategory::CONFIGURATION,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low              id_high             channel   fixed_vel  setter
    { InputCategory::NOTE,          0,                  127,                0xFF,     0,       set_note },
    { InputCategory::MODIFIER,      1,                  1,                  0xFF,     0,       set_wave_pos },    // CC1: wave-position scan
    { InputCategory::MODIFIER,      10,                 10,                 0xFF,     0,       set_pan },         // CC10: pan
    { InputCategory::MODIFIER,      MIDI_MOD_ID_PITCH_BEND, MIDI_MOD_ID_PITCH_BEND, 0xFF, 0,   set_pitch_bend },
    { InputCategory::MODIFIER,      72,                 72,                 0xFF,     0,       set_fx_p1 },       // CC72: FX param 1
    { InputCategory::MODIFIER,      73,                 73,                 0xFF,     0,       set_fx_mix },      // CC73: FX wet/dry mix
    { InputCategory::MODIFIER,      74,                 74,                 0xFF,     0,       set_fx_type },     // CC74: FX type select
    { InputCategory::MODIFIER,      75,                 75,                 0xFF,     0,       set_fx_p2 },       // CC75: FX param 2
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0,   set_table },       // Program Change: wavetable select
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "wavetable mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    for (int i = 0; i < 128; i++) note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bend_ratio[ch] = 1.0f;
        channel_pan[ch] = 0;
        channel_table[ch] = &WT_BANK[0];
        channel_program[ch] = 0;
        channel_wave_pos[ch] = WT_INDEX_SIZE / 2;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = 0;
    ui_state.bend = 0;
    ui_state.mod = 64;  // matches WT_INDEX_SIZE/2's mid-scan default above
    // Match ParamExchange::init() fx defaults (delay, dry, p1=55, p2=36≈300 ms).
    ui_state.fx_type = FX_DELAY;
    ui_state.fx_mix = 0;
    ui_state.fx_p1 = 55;
    ui_state.fx_p2 = 36;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    midi_controller_process_generic(data, len, params, kMappingTable);
}
