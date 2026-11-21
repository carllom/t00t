#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "note_freq.h"
#include "speaker_sim.h"
#include "chip_factory.h"
#include "chip_platform.h"
#include "patch/platform_task.h"
#include "patch/sysex.h"
#include "patch/user_bank.h"
#include "patch/user_flash.h"
#include "midi/midi_out.h"

// chip's Input subsystem: the module-specific tail of the Input pipeline
// -- mapping table, Handlers, and the Voice Allocation Interface calls
// they make. MIDI bytes reach it via midi_controller_process(), built
// from midi_dispatch.h's shared, module-agnostic generic dispatch helpers
// (src/midi/midi_controller_generic.h) -- the mechanism is shared, the
// mapping table and Handlers are fully this module's own.
//
// Pitch bend is per channel (channel_bend[]), live-pushed to every held
// voice on that channel.

enum ChipCC : uint8_t {
    CC_SPEAKER  = 17,     // module_chip.md §1: speaker sim preset, banded 0..SPEAKER_PRESET_COUNT-1.
                           // Global (not per-channel/next-note) -- applies immediately, and isn't
                           // stored with a Patch or a Multi.
    CC_FX_P1    = 72,     // the Multi's FX
    CC_FX_MIX   = 73,
    CC_FX_TYPE  = 74,
    CC_FX_P2    = 75,
};

static constexpr uint8_t NUM_CHANNELS = 16;

static uint8_t s_speaker_preset_ui = SPEAKER_1702;   // module_chip.md §1: display-only mirror of
                                                       // shadow.voices[*].speaker_preset

// --- Notes: the voice each Part plays a note on (-1 for none) ---
static int8_t note_voice[CHIP_PART_COUNT][128];

// --- Per-voice tracking (Core 0) ---
static bool    voice_held[MAX_VOICES];       // true between note-on and note-off
static uint8_t voice_channel[MAX_VOICES];    // owning MIDI channel
static uint8_t voice_part[MAX_VOICES];       // the Part it plays for
static uint8_t voice_note[MAX_VOICES];       // the note it plays (for pitch bend)

static float channel_bend[NUM_CHANNELS];     // per-channel pitch-bend ratio, live

// --- Patch platform: the Multi's Parts, routed by MIDI channel (patch/multi.h) ---
PartSlotT<ChipCookedPatch> chip_part_slots[CHIP_PART_COUNT];
PartSlotT<ChipMultiCooked> chip_multi_slot;
static UserBank user_bank(ChipModule::kModuleId, user_flash_device());
static PatchPlatformT<ChipModule> platform(chip_part_slots, kChipFactoryBanks, kChipFactoryBankCount, &user_bank);
static MultiT<ChipModule> multi(platform, &user_bank, chip_multi_slot);

PatchPlatformT<ChipModule> &chip_platform() { return platform; }
MultiT<ChipModule> &chip_multi() { return multi; }

// SysEx: the t00t editor protocol (patch/sysex.h), replies over USB.
static PatchSysExT<ChipModule> sysex(platform, multi, &user_bank, midi_out_send);

void patch_platform_task() {
    platform.task();
    multi.task();
    sysex.task();
}

// --- Filter bus binding (module_chip.md §5.2), per Part --------------------------
// Tonal params (cutoff/resonance/mode) live in the Patch itself and are read
// by Core 1 from the feeding voice's own copy -- this is routing only.
static int8_t part_bus[CHIP_PART_COUNT];     // bus this Part owns, -1 = none
static int8_t bus_owner[FILTER_BUS_COUNT];   // Part owning this bus, -1 = free

static void release_bus(uint8_t part) {
    if (part_bus[part] >= 0) {
        bus_owner[part_bus[part]] = -1;
        part_bus[part] = -1;
    }
}

// bind_filter (module_chip.md §5.2): 1. the Part already owns a bus ->
// share it. 2. any free bus -> bind it. 3. none free -> BUS_NONE, render
// unfiltered (graceful degradation, and period-correct -- most voices in
// real tunes ran unfiltered, since the filter was scarce). Driven by the
// Patch's FILTER bits, not a manual toggle.
static uint8_t bind_filter(uint8_t part, const ChipCookedPatch &patch) {
    if (patch.type != VT_SID || !patch.uses_filter) {
        release_bus(part);
        return BUS_NONE;
    }
    if (part_bus[part] < 0) {
        for (uint8_t b = 0; b < FILTER_BUS_COUNT; b++) {
            if (bus_owner[b] < 0) { bus_owner[b] = (int8_t)part; part_bus[part] = (int8_t)b; break; }
        }
    }
    return part_bus[part] >= 0 ? (uint8_t)part_bus[part] : BUS_NONE;
}

// The voice's pitch in both chips' registers -- SID's freq_reg is
// proportional to frequency, AY's tone period is inverted (note_freq.h).
static void set_voice_freq(VoiceParams &vp, uint8_t note, uint8_t ch) {
    float hz = chip_note_to_hz(note) * channel_bend[ch];
    vp.sid_freq = chip_hz_to_freq_reg(hz);
    vp.ay_period = ay_hz_to_period(hz);
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
// cook (chip_render.h), as the chip type and filter bus Core 0 reads from
// the same cook here.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    const uint8_t ch = value.channel;
    if (!value.note_on) {
        // Every Part's voice for this note on this channel -- including a
        // Part moved off the channel since the note started.
        for (uint32_t p = 0; p < CHIP_PART_COUNT; p++) {
            const int8_t v = note_voice[p][value.note];
            if (!voice_plays(v, (uint8_t)p, value.note, ch)) continue;
            release_voice(shadow, v);
            note_voice[p][value.note] = -1;
        }
        return;
    }

    const uint32_t mask = multi.parts_on_channel(ch);
    for (uint8_t part = 0; part < CHIP_PART_COUNT; part++) {
        if (!(mask & (1u << part))) continue;
        // Retrigger: release whatever voice this Part already sounds the
        // note on before reallocating -- may or may not land on the same
        // physical voice, same as any dynamic allocator revoicing a repeat.
        if (voice_plays(note_voice[part][value.note], part, value.note, ch)) {
            release_voice(shadow, note_voice[part][value.note]);
        }
        int v = voice_alloc_allocate();
        if (v < 0) return;
        note_voice[part][value.note] = (int8_t)v;
        voice_held[v] = true;
        voice_channel[v] = ch;
        voice_part[v] = part;
        voice_note[v] = value.note;

        const ChipCookedPatch &patch = platform.cooked(part);
        VoiceParams &vp = shadow.voices[v];
        vp.type = patch.type;
        set_voice_freq(vp, value.note, ch);
        vp.part = part;
        vp.load_id = platform.load_id(part);
        vp.cook_seq = platform.cook_seq(part);
        vp.velocity = value.velocity ? value.velocity : 127;
        vp.filter_bus = bind_filter(part, patch);
        vp.gate = true;
        vp.trigger++;   // Core 1 hard-restarts the envelope + VM on this edge
    }

    ui_state.last_note = value.note;
    ui_state.last_velocity = value.velocity;
    ui_state.last_channel = ch;
    const uint8_t shown = multi.first_part_on_channel(ch);
    if (shown < CHIP_PART_COUNT) ui_state.program = (uint8_t)multi.part_param(shown, PART_P_PROGRAM);
}

// Re-scale frequency for every held voice on a channel after a bend change.
static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            set_voice_freq(shadow.voices[v], voice_note[v], channel);
        }
    }
}

static void set_pitch_bend(VoiceParamBlock &shadow, const InputValue &value) {
    channel_bend[value.channel] = value.scalar;
    apply_channel_bend(shadow, value.channel);
}

// Global speaker-sim preset -- applies immediately to every voice, unlike a
// Patch (per-Part, next-note only).
static void set_speaker(VoiceParamBlock &shadow, const InputValue &value) {
    uint8_t idx = (uint8_t)((uint32_t)value.scalar * SPEAKER_PRESET_COUNT / 128u);
    for (uint32_t v = 0; v < MAX_VOICES; v++) shadow.voices[v].speaker_preset = idx;
    s_speaker_preset_ui = idx;
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
    if (shown < CHIP_PART_COUNT) ui_state.program = (uint8_t)multi.part_param(shown, PART_P_PROGRAM);
    ui_state.last_channel = value.channel;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::NOTE,
    InputCategory::MODIFIER,
    InputCategory::CONFIGURATION,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low                  id_high                 channel  fixed_vel  setter
    { InputCategory::NOTE,          0,                      127,                    0xFF,    0,       set_note },
    { InputCategory::MODIFIER,      MIDI_MOD_ID_PITCH_BEND, MIDI_MOD_ID_PITCH_BEND, 0xFF,    0,       set_pitch_bend },
    { InputCategory::MODIFIER,      CC_SPEAKER,             CC_SPEAKER,             0xFF,    0,       set_speaker },
    { InputCategory::MODIFIER,      CC_FX_P1,               CC_FX_P1,               0xFF,    0,       set_fx_p1 },
    { InputCategory::MODIFIER,      CC_FX_MIX,              CC_FX_MIX,              0xFF,    0,       set_fx_mix },
    { InputCategory::MODIFIER,      CC_FX_TYPE,             CC_FX_TYPE,             0xFF,    0,       set_fx_type },
    { InputCategory::MODIFIER,      CC_FX_P2,               CC_FX_P2,               0xFF,    0,       set_fx_p2 },
    { InputCategory::MODIFIER,      0,                      127,                    0xFF,    0,       set_param_cc },  // any other CC: Part mix, NRPN
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF,    0,       set_patch },     // Program Change: patch select
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "chip mapping table entry uses an InputCategory not in kCapabilities");

// A descriptor default CC that an explicit entry above also claims would
// never reach the Multi.
constexpr bool default_ccs_reachable() {
    for (const auto &e : kMappingTable) {
        if (e.category != InputCategory::MODIFIER || e.setter == set_param_cc) continue;
        for (uint32_t cc = e.id_low; cc <= e.id_high; cc++) {
            if (descriptor_uses_cc(ChipModule::kDescriptors, (uint8_t)cc)) return false;
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
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_held[v] = false;
        voice_channel[v] = 0;
        voice_part[v] = 0;
        voice_note[v] = 0;
    }
    for (uint8_t c = 0; c < NUM_CHANNELS; c++) channel_bend[c] = 1.0f;
    for (int8_t &b : part_bus) b = -1;
    for (int8_t &o : bus_owner) o = -1;

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

uint8_t chip_speaker_preset_ui() { return s_speaker_preset_ui; }
