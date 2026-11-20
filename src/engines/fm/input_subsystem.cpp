#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "fm_factory.h"
#include "fm_platform.h"
#include "sine_tab.h"
#include "patch/platform_task.h"
#include "patch/sysex.h"
#include "patch/user_bank.h"
#include "patch/user_flash.h"
#include "midi/midi_out.h"
#include <cmath>

// FM's Input subsystem: the module-specific tail of the Input pipeline --
// mapping table, Handlers, and the Voice Allocation Interface calls they
// make. MIDI bytes reach it via midi_controller_process(), built from
// midi_dispatch.h's shared, module-agnostic generic dispatch helpers
// (src/midi/midi_controller_generic.h) -- the mechanism is shared, the
// mapping table and Handlers are fully this module's own.

// --- Notes: the voice each Part plays a note on (-1 for none) ---
static int8_t note_voice[FM_PART_COUNT][128];

// --- Per-voice tracking (Core 0) ---
static uint32_t voice_base_inc[MAX_VOICES];  // unbent phase_inc, for bend scaling
static uint8_t  voice_channel[MAX_VOICES];   // owning MIDI channel
static uint8_t  voice_part[MAX_VOICES];      // the Part it plays for
static uint8_t  voice_note[MAX_VOICES];      // the note it plays
static uint8_t  voice_program[MAX_VOICES];   // its Part's PROGRAM at note-on, for the display
static bool     voice_held[MAX_VOICES];      // true between note-on and note-off

// --- Patch platform: the Multi's Parts, routed by MIDI channel (patch/multi.h) ---
PartSlotT<FmCookedPatch> fm_part_slots[FM_PART_COUNT];
PartSlotT<FmMultiCooked> fm_multi_slot;
static UserBank user_bank(FmModule::kModuleId, user_flash_device());
static PatchPlatformT<FmModule> platform(fm_part_slots, kFmFactoryBanks, kFmFactoryBankCount, &user_bank);
static MultiT<FmModule> multi(platform, &user_bank, fm_multi_slot);

PatchPlatformT<FmModule> &fm_platform() { return platform; }
MultiT<FmModule> &fm_multi() { return multi; }

// SysEx: the t00t editor protocol (patch/sysex.h), replies over USB.
static PatchSysExT<FmModule> sysex(platform, multi, &user_bank, midi_out_send);

void patch_platform_task() {
    platform.task();
    multi.task();
    sysex.task();
}

// --- Per-channel state (16 MIDI channels) ---
static constexpr uint8_t NUM_CHANNELS = 16;
static float   channel_bend_ratio[NUM_CHANNELS];  // phase_inc multiplier (1.0 = centered)
static int16_t channel_mod_wheel[NUM_CHANNELS];   // CC1, Q15 -- competes with the patch's own LFO depth (live)

// Re-scale phase_inc for every held voice on a channel after a bend change.
static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            shadow.voices[v].phase_inc =
                (uint32_t)((float)voice_base_inc[v] * channel_bend_ratio[channel]);
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

static void release_voice(VoiceParamBlock &shadow, int8_t v) {
    shadow.voices[v].gate = false;
    voice_held[v] = false;
    voice_alloc_release(v);
}

// Whether `v` is still `part`'s voice for `note` on `channel` -- not since
// stolen for another note or Part.
static bool voice_plays(int8_t v, uint8_t part, uint8_t note, uint8_t channel) {
    return v >= 0 && voice_held[v] && voice_part[v] == part && voice_note[v] == note && voice_channel[v] == channel;
}

// Note setter: the Voice Allocation Interface -- this Handler is where
// voice_alloc_allocate()/release() actually get called, never upstream in
// parsing/dispatch (CONTEXT.md's "Voice Allocation Interface" entry). A
// note plays on every Part on its channel (layered), one voice per Part;
// each Part resolves its own voice via note_voice[part][note]. The note
// carries its Part's Patch load and latest cook; Core 1 starts it on that
// cook (fm_render.h).
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    const uint8_t ch = value.channel;
    if (!value.note_on) {
        // Every Part's voice for this note on this channel -- including a
        // Part moved off the channel since the note started.
        for (uint32_t p = 0; p < FM_PART_COUNT; p++) {
            const int8_t v = note_voice[p][value.note];
            if (!voice_plays(v, (uint8_t)p, value.note, ch)) continue;
            release_voice(shadow, v);
            note_voice[p][value.note] = -1;
        }
        return;
    }

    const uint32_t mask = multi.parts_on_channel(ch);
    const uint32_t base = fm_phase_inc(440.0f * powf(2.0f, (float)(value.note - 69) / 12.0f));
    for (uint8_t part = 0; part < FM_PART_COUNT; part++) {
        if (!(mask & (1u << part))) continue;
        // Retrigger: steal whatever voice this Part is already sounding the note on.
        if (voice_plays(note_voice[part][value.note], part, value.note, ch)) {
            release_voice(shadow, note_voice[part][value.note]);
        }
        int v = voice_alloc_allocate();
        if (v < 0) return;
        note_voice[part][value.note] = (int8_t)v;
        voice_held[v] = true;
        voice_base_inc[v] = base;
        voice_channel[v] = ch;
        voice_part[v] = part;
        voice_note[v] = value.note;
        voice_program[v] = (uint8_t)multi.part_param(part, PART_P_PROGRAM);

        VoiceParams &vp = shadow.voices[v];
        vp.phase_inc = (uint32_t)((float)base * channel_bend_ratio[ch]);
        vp.amplitude = (int16_t)(value.velocity * 258);  // 0..127 -> ~0..32766
        vp.part = part;
        vp.note = value.note;  // key level/rate scaling need the raw note, not just phase_inc
        vp.load_id = platform.load_id(part);
        vp.cook_seq = platform.cook_seq(part);
        vp.mod_wheel = channel_mod_wheel[ch];
        vp.trigger++;
        vp.gate = true;
    }

    ui_state.last_note = value.note;
    ui_state.last_velocity = value.velocity;
    ui_state.last_channel = ch;
    const uint8_t shown = multi.first_part_on_channel(ch);
    if (shown < FM_PART_COUNT) ui_state.program = (uint8_t)multi.part_param(shown, PART_P_PROGRAM);
}

// Modifier setters take the raw 0-127 CC byte via value.scalar and do their
// own source-native -> module-native conversion and ui_state mirroring --
// midi_dispatch_cc() itself is CC-number-agnostic.
static void set_mod_wheel(VoiceParamBlock &shadow, const InputValue &value) {
    channel_mod_wheel[value.channel] = (int16_t)(value.scalar * 258);
    apply_channel_mod_wheel(shadow, value.channel);
    ui_state.mod = (uint8_t)value.scalar;
    ui_state.last_channel = value.channel;
}

static void set_pitch_bend(VoiceParamBlock &shadow, const InputValue &value) {
    channel_bend_ratio[value.channel] = value.scalar;
    apply_channel_bend(shadow, value.channel);
}

// FX setters: the Multi's shared FX (patch/multi.h), whatever channel the
// CC arrives on. Each takes the CC's raw 0-127 byte via value.scalar.
static void set_fx_type(VoiceParamBlock &, const InputValue &value) {
    multi.set_common(COMMON_P_FX_TYPE, (int32_t)((uint32_t)value.scalar * FX_COUNT / 128u));
}

static void set_fx_mix(VoiceParamBlock &, const InputValue &value) {
    multi.set_common(COMMON_P_FX_MIX, (int32_t)value.scalar);
}

static void set_fx_p1(VoiceParamBlock &, const InputValue &value) {
    multi.set_common(COMMON_P_FX_P1, (int32_t)value.scalar);
}

static void set_fx_p2(VoiceParamBlock &, const InputValue &value) {
    multi.set_common(COMMON_P_FX_P2, (int32_t)value.scalar);
}

// Every CC not claimed above goes to the Parts on the channel: the Part
// parameters' default CCs (volume CC7, pan CC10, FX send CC91) and NRPN
// parameter edits.
static void set_param_cc(VoiceParamBlock &, const InputValue &value) {
    multi.control_change(value.channel, value.id, (uint8_t)value.scalar);
}

// Patch select: Program Change loads that slot of the bank the channel's
// Bank Select MSB picks (factory banks 0-63, user banks from 64; see
// patch/platform.h) into every Part on the channel. An empty slot or a
// missing bank is ignored; sounding notes keep the Patch they started with.
// It never selects a Multi.
static void set_patch(VoiceParamBlock &, const InputValue &value) {
    multi.program_change(value.channel, midi_channel_bank_msb(value.channel), (uint8_t)value.index);
    const uint8_t shown = multi.first_part_on_channel(value.channel);
    if (shown < FM_PART_COUNT) ui_state.program = (uint8_t)multi.part_param(shown, PART_P_PROGRAM);
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
    { InputCategory::MODIFIER,      1,                  1,                  0xFF,     0,       set_mod_wheel },   // CC1: mod wheel
    { InputCategory::MODIFIER,      MIDI_MOD_ID_PITCH_BEND, MIDI_MOD_ID_PITCH_BEND, 0xFF, 0,   set_pitch_bend },
    { InputCategory::MODIFIER,      72,                 72,                 0xFF,     0,       set_fx_p1 },       // CC72: FX param 1
    { InputCategory::MODIFIER,      73,                 73,                 0xFF,     0,       set_fx_mix },      // CC73: FX wet/dry mix
    { InputCategory::MODIFIER,      74,                 74,                 0xFF,     0,       set_fx_type },     // CC74: FX type select
    { InputCategory::MODIFIER,      75,                 75,                 0xFF,     0,       set_fx_p2 },       // CC75: FX param 2
    { InputCategory::MODIFIER,      0,                  127,                0xFF,     0,       set_param_cc },    // any other CC: Part mix, NRPN
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0,   set_patch },       // Program Change: patch select
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "fm mapping table entry uses an InputCategory not in kCapabilities");

// A descriptor default CC that an explicit entry above also claims would
// never reach the Multi.
constexpr bool default_ccs_reachable() {
    for (const auto &e : kMappingTable) {
        if (e.category != InputCategory::MODIFIER || e.setter == set_param_cc) continue;
        for (uint32_t cc = e.id_low; cc <= e.id_high; cc++) {
            if (descriptor_uses_cc(FmModule::kDescriptors, (uint8_t)cc)) return false;
            if (descriptor_uses_cc(kPartDescriptors, (uint8_t)cc)) return false;
        }
    }
    return true;
}
static_assert(default_ccs_reachable(), "a descriptor default CC is shadowed by an explicit mapping entry");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    for (auto &part : note_voice) {
        for (int8_t &v : part) v = -1;
    }
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_bend_ratio[ch] = 1.0f;
        channel_mod_wheel[ch] = 0;  // resting position -- a patch's own configured LFO depth still plays at 0 (lfo.h's max() rule)
    }
    multi.init();  // the default Multi: Part n on channel n
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = (uint8_t)multi.part_param(0, PART_P_PROGRAM);
    ui_state.bend = 0;
    ui_state.mod = 0;
}

static void on_sysex(const uint8_t *msg, uint32_t len) { sysex.handle(msg, len); }

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    midi_controller_process_generic(data, len, params, kMappingTable, on_sysex);
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
