#include "midi_controller.h"
#include "midi_parser.h"
#include "patch.h"
#include "sine_tab.h"
#include "voice_alloc.h"
#include <cmath>

// FM's own MIDI controller, not the shared src/midi/midi_controller.cpp:
// this engine's VoiceParams carries a patch pointer, not a VoicePreset the
// shared controller's presets.h shape expects. NOTE/MODIFIER/CONFIGURATION
// input all route through the shared Router (src/input_layer.h,
// input_dispatch()) into this file's own kMappingTable/Handlers -- the
// mechanism is shared, the mapping and Handlers are fully this module's own.
//
// Patch select: Program Change picks `FM_PATCHES[value.index % FM_PATCH_COUNT]`
// (patch.h) -- always at least one entry (FM_TEST_PATCH) even without a
// real DX7 bank generated locally, so this path never needs to branch on
// whether one exists.

static MidiParser midi_parser;
static int8_t midi_note_voice[128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static uint8_t  voice_program[MAX_VOICES];   // FM_PATCHES[] index at this voice's note-on -- a
                                              // held voice keeps the patch it started with even
                                              // if its channel's program changes mid-note (module_fm.md
                                              // "Display"), so this can't be read back from
                                              // channel_program[voice_channel[v]] after the fact
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Per-channel state (16 MIDI channels) ---
static constexpr uint8_t NUM_CHANNELS = 16;
static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float PITCH_BEND_RANGE_SEMITONES = 2.0f;

static float   channel_bend_ratio[NUM_CHANNELS];  // phase_inc multiplier (1.0 = centered)
static int16_t channel_pan[NUM_CHANNELS];         // CC10 pan, Q15
static const FmPatch *channel_patch[NUM_CHANNELS];  // defaults to FM_TEST_PATCH, Program Change overrides it
static uint8_t channel_program[NUM_CHANNELS];       // FM_PATCHES[] index, for ui_state.program
static int16_t channel_mod_wheel[NUM_CHANNELS];     // CC1, Q15 -- scales the patch's own LFO depth (live)

// --- UI snapshot (updated on each event, read by the display) ---
static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

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

// Push pan (CC10) to every held voice on a channel.
static void apply_channel_pan(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].pan = channel_pan[channel];
        }
    }
}

// Push mod wheel (CC1) to every held voice on a channel -- live, a source
// competing with the patch's own LFO PMD/AMD depth (lfo.h's max() rule).
static void apply_channel_mod_wheel(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].mod_wheel = channel_mod_wheel[channel];
        }
    }
}

// --- Router mapping table (src/input_layer.h) ---
// Pitch bend has no MIDI CC number of its own; Program Change has no CC
// number at all -- both get a synthetic id outside the 0-127 CC range so
// they can share their category's table without colliding with real CCs.
static constexpr uint8_t MOD_ID_PITCH_BEND = 128;
static constexpr uint8_t CONFIG_ID_PATCH = 0;

// Note setter: applies a resolved note on/off edge to the shadow block.
// Voice allocation and voice_held/midi_note_voice bookkeeping stay in
// midi_controller_process (allocator concerns, not shadow-parameter
// content) -- this only mutates VoiceParams and this voice's own tracking.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.note_on) {
        float freq = 440.0f * powf(2.0f, (float)(value.note - 69) / 12.0f);
        uint32_t base = fm_phase_inc(freq);

        voice_base_inc[value.voice] = base;
        voice_channel[value.voice] = value.channel;
        voice_program[value.voice] = channel_program[value.channel];

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
        ui_state.program = channel_program[value.channel];

        VoiceParams &vp = shadow.voices[value.voice];
        vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[value.channel]);
        vp.amplitude = (int16_t)(value.velocity * 258);  // 0..127 -> ~0..32766, plain amplitude scaling
        vp.pan = channel_pan[value.channel];
        vp.patch = channel_patch[value.channel];
        vp.note = value.note;  // key level/rate scaling need the raw note, not just phase_inc
        vp.mod_wheel = channel_mod_wheel[value.channel];
        vp.trigger++;
        vp.gate = true;
    } else {
        shadow.voices[value.voice].gate = false;
    }
}

static void set_mod_wheel(VoiceParamBlock &shadow, const InputValue &value) {
    channel_mod_wheel[value.channel] = (int16_t)value.scalar;
    apply_channel_mod_wheel(shadow, value.channel);
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

// Patch select: Program Change is this module's only Configuration input --
// picks a patch by index (wrapped into FM_PATCH_COUNT, always >= 1) for
// future notes on the channel. Doesn't touch the shadow block itself, so
// callers don't need to commit for it alone.
static void set_patch(VoiceParamBlock &, const InputValue &value) {
    uint8_t idx = (uint8_t)(value.index % FM_PATCH_COUNT);
    channel_patch[value.channel] = &FM_PATCHES[idx];
    channel_program[value.channel] = idx;
    ui_state.program = idx;
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
    { InputCategory::MODIFIER,      MOD_ID_PITCH_BEND,  MOD_ID_PITCH_BEND,  0xFF,     0,       set_pitch_bend },
    { InputCategory::MODIFIER,      72,                 72,                 0xFF,     0,       set_fx_p1 },       // CC72: FX param 1
    { InputCategory::MODIFIER,      73,                 73,                 0xFF,     0,       set_fx_mix },      // CC73: FX wet/dry mix
    { InputCategory::MODIFIER,      74,                 74,                 0xFF,     0,       set_fx_type },     // CC74: FX type select
    { InputCategory::MODIFIER,      75,                 75,                 0xFF,     0,       set_fx_p2 },       // CC75: FX param 2
    { InputCategory::CONFIGURATION, CONFIG_ID_PATCH,    CONFIG_ID_PATCH,    0xFF,     0,       set_patch },       // Program Change: patch select
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "fm mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bend_ratio[ch] = 1.0f;
        channel_pan[ch] = 0;
        channel_patch[ch] = &FM_TEST_PATCH;
        channel_program[ch] = 0;
        channel_mod_wheel[ch] = 0;  // resting position -- a patch's own configured LFO depth still plays at 0 (lfo.h's max() rule); the wheel only adds on top of it
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
                    case 1: {  // mod wheel — a source competing with the patch's own LFO PMD/AMD depth, live
                        InputValue value{};
                        value.channel = ev.channel;
                        value.scalar = (float)(ev.data2 * 258);
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
                    default: break;  // other CCs unmapped
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
                InputValue value{};
                value.channel = ev.channel;
                value.index = ev.data1;
                input_dispatch(shadow, kMappingTable, InputCategory::CONFIGURATION, CONFIG_ID_PATCH, value);
                ui_state.last_channel = ev.channel;
                break;
            }
            default: break;
        }
    }

    if (changed) {
        params->commit();
    }
}

const FmPatch *fm_channel_patch(uint8_t channel) {
    return channel < NUM_CHANNELS ? channel_patch[channel] : &FM_TEST_PATCH;
}

void fm_voice_ui_state(uint32_t voice, FmVoiceUiState *out) {
    if (voice < MAX_VOICES) {
        out->channel = voice_channel[voice];
        out->program = voice_program[voice];
    } else {
        out->channel = 0;
        out->program = 0;
    }
}
