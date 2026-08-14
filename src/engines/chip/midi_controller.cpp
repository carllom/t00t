#include "midi/midi_controller.h"
#include "midi_parser.h"
#include "note_freq.h"
#include "instruments.h"
#include "ay_instruments.h"
#include "voice_alloc.h"
#include "speaker_sim.h"
#include <cmath>

// module_chip.md §12.2, AY-P1: one combined instrument-selection space spans both
// chip types (SID instruments first, then AY's) so CC_INSTRUMENT/PC stay a
// single "pick a patch" list rather than needing a separate chip-type
// selector -- a player thinks in patches, not in which silicon a patch
// happens to come from. Index < INSTRUMENT_COUNT is VT_SID (sub-index
// unchanged); >= INSTRUMENT_COUNT is VT_AY (sub-index = combined -
// INSTRUMENT_COUNT).
static constexpr uint8_t TOTAL_INSTRUMENT_COUNT = INSTRUMENT_COUNT + AY_INSTRUMENT_COUNT;

// Chip module MIDI routing (Core 0), module_chip.md §1 P4 / §8: dynamic voice
// allocation. voice_alloc.* (existing three-tier steal policy: silent ->
// released -> oldest active) reused unmodified, per §8's "P4: dynamic
// allocation ... applies unmodified." MIDI channel no longer maps to a fixed
// voice; midi_note_voice[128] maps note number -> allocated voice across the
// whole MAX_VOICES pool, same shape as the speech engine's controller (which
// got there first, #36). Channels 16..31 of the pool are reachable now that
// allocation isn't tied to channel number at all.
//
// Filter-bus binding stays per-channel (chan_bus[]), not per-voice: §5.2's
// "bus already owned by this instrument -> share it" rule, extended from
// P1-P3's monophonic-per-channel model to real polyphony, means multiple
// simultaneous notes on one channel (a chord) share that channel's one
// bound bus -- exactly the "chord of one filtered instrument sums into one
// bus" behaviour §5.2 describes, just no longer limited to one note at a
// time to demonstrate it.
//
// Pitch bend is now genuinely per-channel (channel_bend[]), not a single
// global ratio like P1-P3 had -- the monophonic-per-channel model never had
// two different channels sounding at once to expose that as wrong; dynamic
// allocation does. Live-pushed to every currently-held voice on that
// channel, same shape as speech's live CCs (its own header comment's
// "pushes the new value into every voice currently held on that channel").

enum ChipCC : uint8_t {
    CC_INSTRUMENT = 16,   // per-channel, next-note instrument select, banded 0..INSTRUMENT_COUNT-1
    CC_SPEAKER    = 17,   // module_chip.md §1 P5: speaker sim preset, banded 0..SPEAKER_PRESET_COUNT-1.
                           // Global (not per-channel/next-note like CC_INSTRUMENT) -- applies
                           // immediately, same as CC_FX_TYPE below.
    CC_FX_TYPE  = 74,     // effect select (engine_base.h EffectParams convention)
    CC_FX_MIX   = 73,
    CC_FX_P1    = 72,
    CC_FX_P2    = 75,
};

static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float    PITCH_BEND_RANGE_SEMITONES = 2.0f;
static constexpr uint8_t  NUM_CHANNELS = 16;

static MidiParser midi_parser;
static MidiUiState ui_state;
static uint8_t s_speaker_preset_ui = SPEAKER_1702;   // module_chip.md §1 P5: display-only mirror of
                                                       // shadow.voices[*].speaker_preset --
                                                       // not in MidiUiState, which is shared
                                                       // across every engine and has no
                                                       // speaker-sim concept

static int8_t  midi_note_voice[128];         // note number -> allocated voice, -1 = none
static bool    voice_held[MAX_VOICES];       // physically held (for live-CC channel scoping)
static uint8_t voice_channel[MAX_VOICES];    // which channel triggered this voice
static uint8_t voice_note[MAX_VOICES];       // which note number this voice is playing
                                               // (reverse of midi_note_voice[], for pitch bend)

static uint8_t chan_instrument[NUM_CHANNELS];  // next-note instrument select (PC or CC16)
static float   channel_bend[NUM_CHANNELS];     // per-channel pitch-bend ratio, live

// --- Filter bus binding (module_chip.md §5.2), per channel -------------------------
// Tonal params (cutoff/resonance/mode) live in the selected instrument
// itself and are read directly by Core 1 from the feeding voice's own
// instrument -- see audio_engine.cpp's P3 comment for why FilterBusParams
// goes unused by chip. This is routing only.
static int8_t chan_bus[NUM_CHANNELS];        // bus this channel owns, -1 = none
static int8_t bus_owner[FILTER_BUS_COUNT];   // channel owning this bus, -1 = free

static void release_bus(uint8_t ch) {
    if (chan_bus[ch] >= 0) {
        bus_owner[chan_bus[ch]] = -1;
        chan_bus[ch] = -1;
    }
}

// bind_filter(module_chip.md §5.2): 1. already owns a bus -> share/reuse it.
// 2. any free bus -> bind it. 3. none free -> BUS_NONE, render unfiltered
// (graceful degradation, and period-correct: "most voices in real tunes ran
// unfiltered, because the filter was scarce"). Driven by the selected
// instrument's uses_filter flag, not a manual toggle.
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

void midi_controller_init() {
    midi_parser.init();
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
                // Retrigger: release whatever voice this note number already
                // owns before reallocating -- may or may not land on the
                // same physical voice, same as any dynamic allocator
                // revoicing a repeat (matches speech's controller, #36).
                if (midi_note_voice[note] >= 0) {
                    int8_t old = midi_note_voice[note];
                    shadow.voices[old].gate = false;
                    voice_held[old] = false;
                    voice_alloc_release(old);
                }
                int v = voice_alloc_allocate();
                if (v >= 0) {
                    uint8_t combined = chan_instrument[ev.channel];
                    bool is_ay = combined >= INSTRUMENT_COUNT;
                    VoiceParams &vp = shadow.voices[v];
                    vp.type = is_ay ? VT_AY : VT_SID;
                    set_voice_freq(shadow, (uint32_t)v, note, ev.channel);
                    vp.instrument = is_ay ? (uint8_t)(combined - INSTRUMENT_COUNT) : combined;
                    vp.velocity = ev.data2 ? ev.data2 : 127;
                    vp.gate = true;
                    vp.trigger++;   // Core 1 hard-restarts the envelope + VM on this edge
                    midi_note_voice[note] = (int8_t)v;
                    voice_held[v] = true;
                    voice_channel[v] = ev.channel;
                    voice_note[v] = note;
                    // AY has no filter model (module_chip.md §12.1) -- always BUS_NONE,
                    // same as bind_filter() would resolve for any uses_filter=false
                    // SID instrument, just without a bus to release on this channel.
                    if (is_ay) { release_bus(ev.channel); vp.filter_bus = BUS_NONE; }
                    else       bind_filter(shadow, (uint32_t)v, ev.channel, INSTRUMENTS[vp.instrument]);

                    ui_state.last_note = note;
                    ui_state.last_velocity = ev.data2;
                    ui_state.last_channel = ev.channel;
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
                    case CC_INSTRUMENT: {
                        if (ev.channel >= NUM_CHANNELS) break;
                        uint8_t idx = (uint8_t)((uint32_t)ev.data2 * TOTAL_INSTRUMENT_COUNT / 128u);
                        chan_instrument[ev.channel] = idx;
                        if (ev.channel == ui_state.last_channel) ui_state.program = idx;
                        changed = true;
                        break;
                    }
                    case CC_SPEAKER: {
                        uint8_t idx = (uint8_t)((uint32_t)ev.data2 * SPEAKER_PRESET_COUNT / 128u);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) shadow.voices[v].speaker_preset = idx;
                        s_speaker_preset_ui = idx;
                        changed = true;
                        break;
                    }
                    case CC_FX_TYPE:
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case CC_FX_MIX:
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case CC_FX_P1:
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case CC_FX_P2:
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    default: break;
                }
                break;
            }

            case MIDI_PITCH_BEND: {
                if (ev.channel >= NUM_CHANNELS) break;
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER)
                                  / (float)PITCH_BEND_CENTER * PITCH_BEND_RANGE_SEMITONES;
                channel_bend[ev.channel] = powf(2.0f, semitones / 12.0f);
                for (uint32_t v = 0; v < MAX_VOICES; v++) {
                    if (voice_held[v] && voice_channel[v] == ev.channel) {
                        set_voice_freq(shadow, v, voice_note[v], ev.channel);
                        changed = true;
                    }
                }
                ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                break;
            }

            case MIDI_PROGRAM_CHANGE:
                if (ev.channel < NUM_CHANNELS && ev.data1 < TOTAL_INSTRUMENT_COUNT) {
                    chan_instrument[ev.channel] = ev.data1;
                    ui_state.program = ev.data1;
                    changed = true;
                }
                break;

            default: break;   // no sequencer/transport in this engine
        }
    }

    if (changed) params->commit();
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
uint8_t chip_speaker_preset_ui() { return s_speaker_preset_ui; }
