#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "patch.h"
#include "factory_bank.h"
#include "patch/platform.h"
#include "patch/platform_task.h"
#include <cmath>

// subtractive's Input subsystem: the module-specific tail of the Input
// pipeline -- mapping table, Handlers, and the Voice Allocation Interface
// calls they make. MIDI bytes reach it via midi_controller_process(),
// built from midi_dispatch.h's shared, module-agnostic generic dispatch
// helpers (src/midi/midi_controller_generic.h); GPIO buttons
// (src/controller.cpp) reach the same kMappingTable/set_note Handler via
// midi_controller_dispatch_note() -- the mechanism is shared, the mapping
// table and Handlers are fully this module's own.

// --- Parser state (the shared MidiParser instance, midi_parser.h) ---
static int8_t note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Patch platform: one Part per MIDI channel (Part N = channel N) ---
PartSlotT<SubCookedPatch> sub_part_slots[SUB_PART_COUNT];
static PatchPlatformT<SubtractiveModule> platform(sub_part_slots, kSubFactoryBank, kSubFactoryCount);

const char *sub_part_patch_name(uint8_t part) {
    return part < SUB_PART_COUNT ? platform.name(part) : "";
}

void patch_platform_task() { platform.task(); }

// --- Per-channel state (16 MIDI channels) ---
// Default MIDI channel → factory Patch (channels beyond the list fall back
// to the last entry). Program Change loads another one at runtime.
static const uint8_t channel_default_patch[] = {
    0,  // Ch 1: Fairlite
    1,  // Ch 2: Sq-PWM
    2,  // Ch 3: Saw-Flt
};
static constexpr uint8_t NUM_CHANNEL_DEFAULTS = sizeof(channel_default_patch) / sizeof(channel_default_patch[0]);

static constexpr uint8_t NUM_CHANNELS = 16;
static_assert(NUM_CHANNELS == SUB_PART_COUNT, "one Part per MIDI channel");

static uint8_t channel_patch[NUM_CHANNELS];   // factory Patch index loaded in each channel's Part
static float   channel_bend_ratio[NUM_CHANNELS]; // phase_inc multiplier (1.0 = centered)
static int16_t channel_mod[NUM_CHANNELS];        // mod-wheel vibrato depth, Q15
static int16_t channel_pan[NUM_CHANNELS];        // CC10 pan, Q15 (-32768=L .. 0=center .. 32767=R)

static uint8_t default_patch_for_channel(uint8_t ch) {
    return ch < NUM_CHANNEL_DEFAULTS ? channel_default_patch[ch] : channel_default_patch[NUM_CHANNEL_DEFAULTS - 1];
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

// Note setter: the Voice Allocation Interface -- this Handler is where
// voice_alloc_allocate()/release() actually get called, never upstream in
// parsing/dispatch (CONTEXT.md's "Voice Allocation Interface" entry).
// Resolves its own voice via note_voice[], keyed by note number, so it
// works identically whether value.note arrived from MIDI or a GPIO button.
//
// note/velocity arrive un-normalized (input_layer.h's fixed_velocity
// substitution mutator only knows how to substitute raw 0-127 velocity, so
// it has to run before any native-unit scaling); note is also an identity
// (Strike needs it to pick a sound, not just Note to compute a pitch), so
// it's not something to "normalize" away. The freq/amplitude math below is
// this setter's own normalization step.
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

        // The note takes its note-on fields, and its pitch (sample vs
        // oscillator rate), from the Part's latest cook.
        const SubCookedPatch &patch = platform.cooked(value.channel);
        uint32_t base = sub_note_phase_inc(patch, value.note);

        voice_base_inc[v] = base;
        voice_channel[v] = value.channel;

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
        ui_state.program = channel_patch[value.channel];

        sub_note_start(shadow.voices[v], value.channel, patch, platform.load_id(value.channel),
                       base, channel_bend_ratio[value.channel],
                       value.velocity, channel_mod[value.channel], channel_pan[value.channel]);
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
// own source-native -> module-native conversion and ui_state mirroring --
// midi_dispatch_cc() itself is CC-number-agnostic.
static void set_mod_wheel(VoiceParamBlock &shadow, const InputValue &value) {
    channel_mod[value.channel] = (int16_t)(value.scalar * 258);  // 0..127 -> ~0..32766
    apply_channel_mod(shadow, value.channel);
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

// Every CC not claimed above: NRPN parameter edits and the descriptors'
// default CCs, applied to the channel's Part Edit buffer (patch.h).
static void set_param_cc(VoiceParamBlock &, const InputValue &value) {
    platform.midi_cc(value.channel, value.id, (uint8_t)value.scalar);
}

// Patch select: Program Change, translated through microKORG numbering
// (row = tens digit, col = ones digit) plus the bank value CC0 stored --
// midi_channel_bank_msb() is shared, module-agnostic plumbing (midi_dispatch.h);
// what a bank+program pair means is entirely this Handler's own business.
// Loads the factory Patch into the channel's Part; sounding notes keep the
// Patch they started with.
static void set_patch(VoiceParamBlock &, const InputValue &value) {
    int slot = microkorg_slot(midi_channel_bank_msb(value.channel), value.index);
    if (slot < 0) return;
    uint8_t index = (uint8_t)(slot % kSubFactoryCount);
    if (platform.load_factory(value.channel, index) == PatchLoadResult::OK) {
        channel_patch[value.channel] = index;
        ui_state.program = index;
        ui_state.last_channel = value.channel;
    }
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::NOTE,
    InputCategory::MODIFIER,
    InputCategory::CONFIGURATION,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low              id_high             channel   fixed_vel  setter
    { InputCategory::NOTE,          0,                  127,                0xFF,     0,       set_note },
    { InputCategory::MODIFIER,      1,                  1,                  0xFF,     0,       set_mod_wheel },   // CC1: mod wheel
    { InputCategory::MODIFIER,      10,                 10,                 0xFF,     0,       set_pan },         // CC10: pan
    { InputCategory::MODIFIER,      MIDI_MOD_ID_PITCH_BEND, MIDI_MOD_ID_PITCH_BEND, 0xFF, 0,   set_pitch_bend },
    { InputCategory::MODIFIER,      72,                 72,                 0xFF,     0,       set_fx_p1 },       // CC72: FX param 1
    { InputCategory::MODIFIER,      73,                 73,                 0xFF,     0,       set_fx_mix },      // CC73: FX wet/dry mix
    { InputCategory::MODIFIER,      75,                 75,                 0xFF,     0,       set_fx_p2 },       // CC75: FX param 2
    { InputCategory::MODIFIER,      76,                 76,                 0xFF,     0,       set_fx_type },     // CC76: FX type select
    { InputCategory::MODIFIER,      0,                  127,                0xFF,     0,       set_param_cc },    // any other CC: NRPN / descriptor default CCs
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0,   set_patch },       // Program Change: patch select
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "subtractive mapping table entry uses an InputCategory not in kCapabilities");

// A descriptor default CC that an explicit entry above also claims would
// never reach the platform.
constexpr bool default_ccs_reachable() {
    for (const auto &e : kMappingTable) {
        if (e.category != InputCategory::MODIFIER || e.setter == set_param_cc) continue;
        for (uint32_t cc = e.id_low; cc <= e.id_high; cc++) {
            if (descriptor_uses_cc(SubtractiveModule::kDescriptors, (uint8_t)cc)) return false;
        }
    }
    return true;
}
static_assert(default_ccs_reachable(), "a descriptor default CC is shadowed by an explicit mapping entry");

void midi_controller_dispatch_note(VoiceParamBlock &shadow, uint8_t note, const InputValue &value) {
    input_dispatch(shadow, kMappingTable, InputCategory::NOTE, note, value);
}

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    for (int i = 0; i < 128; i++) note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_patch[ch] = default_patch_for_channel(ch);
        platform.load_factory(ch, channel_patch[ch]);
        channel_bend_ratio[ch] = 1.0f;
        channel_mod[ch] = 0;
        channel_pan[ch] = 0;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = default_patch_for_channel(0);
    ui_state.bend = 0;
    ui_state.mod = 0;
    // Match ParamExchange::init() fx defaults (delay, dry, p1=55, p2=36≈300 ms).
    ui_state.fx_type = FX_DELAY;
    ui_state.fx_mix = 0;
    ui_state.fx_p1 = 55;
    ui_state.fx_p2 = 36;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    midi_controller_process_generic(data, len, params, kMappingTable);
}
