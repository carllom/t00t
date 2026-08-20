#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "note_freq.h"
#include "instruments.h"
#include "ay_instruments.h"
#include "speaker_sim.h"
#include <cmath>

// chip's Input subsystem: the module-specific tail of the Input pipeline
// -- mapping table, Handlers, and the Voice Allocation Interface calls
// they make. MIDI bytes reach it via midi_controller_process(), built
// from midi_dispatch.h's shared, module-agnostic generic dispatch helpers
// (src/midi/midi_controller_generic.h) -- the mechanism is shared, the
// mapping table and Handlers are fully this module's own.
//
// One combined instrument-selection space spans both chip types
// (module_chip.md §12.2; SID instruments first, then AY's) so Program
// Change stays a single "pick a patch" list rather than needing a
// separate chip-type selector -- a player thinks in patches, not in which
// silicon a patch happens to come from. Index < INSTRUMENT_COUNT is VT_SID
// (sub-index unchanged); >= INSTRUMENT_COUNT is VT_AY (sub-index =
// combined - INSTRUMENT_COUNT).
static constexpr uint8_t TOTAL_INSTRUMENT_COUNT = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;

// midi_note_voice[128] maps note number -> allocated voice across the
// whole MAX_VOICES pool (not tied to MIDI channel), same shape as the
// speech engine's controller.
//
// Filter-bus binding stays per-channel (chan_bus[]), not per-voice: §5.2's
// "bus already owned by this instrument -> share it" rule means multiple
// simultaneous notes on one channel (a chord) share that channel's one
// bound bus. It's a second allocator-like concern alongside voice_alloc,
// so it lives in the same place: set_note(), resolved once per note-on,
// never touched by Configuration (instrument select) itself -- an
// instrument change only affects the *next* note-on, matching how
// voice_alloc's own allocation is never retroactive either.
//
// Pitch bend is genuinely per-channel (channel_bend[]), not a single
// global ratio. Live-pushed to every currently-held voice on that channel,
// same shape as speech's live CCs.

enum ChipCC : uint8_t {
    CC_SPEAKER  = 17,     // module_chip.md §1: speaker sim preset, banded 0..SPEAKER_PRESET_COUNT-1.
                           // Global (not per-channel/next-note) -- applies immediately, same as
                           // CC_FX_TYPE below.
    CC_FX_TYPE  = 74,     // effect select (engine_base.h EffectParams convention)
    CC_FX_MIX   = 73,
    CC_FX_P1    = 72,
    CC_FX_P2    = 75,
};

static constexpr uint8_t NUM_CHANNELS = 16;

static uint8_t s_speaker_preset_ui = SPEAKER_1702;   // module_chip.md §1: display-only mirror of
                                                       // shadow.voices[*].speaker_preset --
                                                       // not in MidiUiState, which is shared
                                                       // across every engine and has no
                                                       // speaker-sim concept

static int8_t  midi_note_voice[128];         // note number -> allocated voice, -1 = none
static bool    voice_held[MAX_VOICES];       // physically held (for live-CC channel scoping)
static uint8_t voice_channel[MAX_VOICES];    // which channel triggered this voice
static uint8_t voice_note[MAX_VOICES];       // which note number this voice is playing
                                               // (reverse of midi_note_voice[], for pitch bend)

static uint8_t chan_instrument[NUM_CHANNELS];  // next-note instrument select (Program Change)
static float   channel_bend[NUM_CHANNELS];     // per-channel pitch-bend ratio, live

// --- Filter bus binding (module_chip.md §5.2), per channel -------------------------
// Tonal params (cutoff/resonance/mode) live in the selected instrument
// itself and are read directly by Core 1 from the feeding voice's own
// instrument -- see audio_engine.cpp's comment for why FilterBusParams
// goes unused by chip. This is routing only.
static int8_t chan_bus[NUM_CHANNELS];        // bus this channel owns, -1 = none
static int8_t bus_owner[FILTER_BUS_COUNT];   // channel owning this bus, -1 = free

static void release_bus(uint8_t ch) {
    if (chan_bus[ch] >= 0) {
        bus_owner[chan_bus[ch]] = -1;
        chan_bus[ch] = -1;
    }
}

// bind_filter (module_chip.md §5.2): 1. already owns a bus -> share/reuse
// it. 2. any free bus -> bind it. 3. none free -> BUS_NONE, render
// unfiltered (graceful degradation, and period-correct -- most voices in
// real tunes ran unfiltered, since the filter was scarce). Driven by the
// selected instrument's uses_filter flag, not a manual toggle.
static void bind_filter(VoiceParamBlock &shadow, uint32_t voice, uint8_t ch, const Instrument &ins) {
    if (!ins.uses_filter) {
        release_bus(ch);
        shadow.voices[voice].filter_bus = BUS_NONE;
        return;
    }
    if (chan_bus[ch] < 0) {
        for (uint8_t b = 0; b < FILTER_BUS_COUNT; b++) {
            if (bus_owner[b] < 0) { bus_owner[b] = (int8_t)ch; chan_bus[ch] = (int8_t)b; break; }
        }
    }
    shadow.voices[voice].filter_bus = (chan_bus[ch] >= 0) ? (uint8_t)chan_bus[ch] : BUS_NONE;
}

// Dispatches on the voice's own (already-set) type -- SID's freq_reg is
// proportional to frequency, AY's tone period is inverted (note_freq.h),
// so this cannot be one formula the way it could when only one chip type
// existed. Correct for both the note-on path (type just written this call)
// and the pitch-bend live-repush path (type from whatever the voice was
// triggered as).
static void set_voice_freq(VoiceParamBlock &shadow, uint32_t voice, uint8_t note, uint8_t ch) {
    float hz = chip_note_to_hz(note) * channel_bend[ch];
    VoiceParams &vp = shadow.voices[voice];
    vp.freq = (vp.type == VT_AY) ? ay_hz_to_period(hz) : chip_hz_to_freq_reg(hz);
}

// Note setter: the Voice Allocation Interface -- this Handler is where
// voice_alloc_allocate()/release() actually get called, never upstream in
// parsing/dispatch (CONTEXT.md's "Voice Allocation Interface" entry).
// Resolves its own voice via midi_note_voice[], keyed by note number, and
// binds this note-on's filter bus the same way -- a second, chip-specific
// resource resolved at the same point in the same Handler.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.note_on) {
        // Retrigger: release whatever voice this note number already
        // owns before reallocating -- may or may not land on the same
        // physical voice, same as any dynamic allocator revoicing a repeat.
        if (midi_note_voice[value.note] >= 0) {
            int8_t old = midi_note_voice[value.note];
            shadow.voices[old].gate = false;
            voice_held[old] = false;
            voice_alloc_release(old);
        }
        int v = voice_alloc_allocate();
        if (v < 0) return;
        midi_note_voice[value.note] = (int8_t)v;
        voice_held[v] = true;
        voice_channel[v] = value.channel;
        voice_note[v] = value.note;

        uint8_t combined = chan_instrument[value.channel];
        bool is_ay = combined >= INSTRUMENT_COUNT;
        VoiceParams &vp = shadow.voices[v];
        vp.type = is_ay ? VT_AY : VT_SID;
        set_voice_freq(shadow, (uint32_t)v, value.note, value.channel);
        vp.instrument = is_ay ? (uint8_t)(combined - INSTRUMENT_COUNT) : combined;
        vp.velocity = value.velocity ? value.velocity : 127;
        vp.gate = true;
        vp.trigger++;   // Core 1 hard-restarts the envelope + VM on this edge

        // AY has no filter model (module_chip.md §12.1) -- always BUS_NONE,
        // same as bind_filter() would resolve for any uses_filter=false
        // SID instrument, just without a bus to release on this channel.
        if (is_ay) { release_bus(value.channel); vp.filter_bus = BUS_NONE; }
        else       bind_filter(shadow, (uint32_t)v, value.channel, INSTRUMENTS[vp.instrument]);

        ui_state.last_note = value.note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = value.channel;
    } else {
        int8_t v = midi_note_voice[value.note];
        if (v < 0) return;
        shadow.voices[v].gate = false;
        voice_held[v] = false;
        voice_alloc_release(v);
        midi_note_voice[value.note] = -1;
    }
}

// Re-scale frequency for every held voice on a channel after a bend change.
static void apply_channel_bend(VoiceParamBlock &shadow, uint8_t channel) {
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == channel) {
            set_voice_freq(shadow, v, voice_note[v], channel);
        }
    }
}

static void set_pitch_bend(VoiceParamBlock &shadow, const InputValue &value) {
    channel_bend[value.channel] = value.scalar;
    apply_channel_bend(shadow, value.channel);
}

// Global speaker-sim preset -- applies immediately to every voice, the
// same module-global/immediate shape the FX setters below use, unlike
// instrument select (per-channel, next-note only).
static void set_speaker(VoiceParamBlock &shadow, const InputValue &value) {
    uint8_t idx = (uint8_t)((uint32_t)value.scalar * SPEAKER_PRESET_COUNT / 128u);
    for (uint32_t v = 0; v < MAX_VOICES; v++) shadow.voices[v].speaker_preset = idx;
    s_speaker_preset_ui = idx;
}

// FX setters write shadow.fx -- one instance per VoiceParamBlock, not
// per-voice -- a true module-global Modifier.
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

// Instrument select: Program Change is this module's only Configuration
// input -- the CC16 encoder alternative (BeatStep-Pro-safe, same reasoning
// fm's CC30 had) is dropped, per the standing Program-Change-alone
// convention every module on the Router now follows. Affects future notes
// on the channel only; a currently-held voice's filter-bus binding is
// untouched until its next note-on.
static void set_instrument(VoiceParamBlock &, const InputValue &value) {
    if (value.index >= TOTAL_INSTRUMENT_COUNT) return;
    chan_instrument[value.channel] = value.index;
    if (value.channel == ui_state.last_channel) ui_state.program = value.index;
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
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF,    0,       set_instrument },
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "chip mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        voice_held[v] = false;
        voice_channel[v] = 0;
        voice_note[v] = 0;
    }
    for (uint32_t n = 0; n < 128; n++) midi_note_voice[n] = -1;
    for (uint8_t c = 0; c < NUM_CHANNELS; c++) {
        chan_instrument[c] = INS_ARP_LEAD;
        channel_bend[c] = 1.0f;
        chan_bus[c] = -1;
    }
    for (uint8_t b = 0; b < FILTER_BUS_COUNT; b++) bus_owner[b] = -1;

    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = INS_ARP_LEAD;
    ui_state.bend = 0;
    ui_state.mod = 0;
    ui_state.fx_type = FX_DELAY;
    ui_state.fx_mix = 0;
    ui_state.fx_p1 = 55;
    ui_state.fx_p2 = 36;
}

void midi_controller_process(const uint8_t *data, uint32_t len, ParamExchange *params) {
    midi_controller_process_generic(data, len, params, kMappingTable);
}

uint8_t chip_speaker_preset_ui() { return s_speaker_preset_ui; }
