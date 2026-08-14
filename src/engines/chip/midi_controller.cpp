#include "midi/midi_controller.h"
#include "midi_parser.h"
#include "note_freq.h"
#include "instruments.h"
#include <cmath>

// Chip module MIDI routing (Core 0), sid.md §1 P1/P2/P3 / §8: "static
// assignment. MIDI channel -> voice, no allocator." MIDI channel N plays SID
// voice N, monophonic per channel (matching main.cpp's own convention
// comment: "the tracker engine assigns channel N to voice N directly").
// Channels 16..31 of the MAX_VOICES=32 pool sit unreachable until P4's
// dynamic allocator.
//
// P3 (sid.md §6): instrument select replaces P1's fixed default patch and
// P2's manual filter CCs -- waveform/pw/ADSR/filter now come from whichever
// instrument (engines/chip/instruments.h) is selected per channel, driven
// by the frame VM on Core 1. Program Change is the natural per-channel
// instrument select (real MIDI semantics: each channel keeps its own
// program); CC16 is the BSP-friendly alternative for controllers that can't
// send PC reliably (project convention -- see the BeatStep Pro PC quirk
// note), banded the same way CC_FX_TYPE already is.

enum ChipCC : uint8_t {
    CC_INSTRUMENT = 16,   // per-channel instrument select, banded 0..INSTRUMENT_COUNT-1
    CC_FX_TYPE  = 74,     // effect select (engine_base.h EffectParams convention)
    CC_FX_MIX   = 73,
    CC_FX_P1    = 72,
    CC_FX_P2    = 75,
};

static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float    PITCH_BEND_RANGE_SEMITONES = 2.0f;

static MidiParser midi_parser;
static MidiUiState ui_state;

// Per-channel state so pitch bend can recompute the currently-held note, and
// so bus binding/instrument select survive across notes on the same channel
// (mirrors groovebox's g_303_note pattern).
static int16_t chan_note[16];        // -1 = nothing held on this channel
static uint8_t chan_instrument[16];  // index into INSTRUMENTS[]
static float   bend_ratio = 1.0f;    // multiplies the frequency register

// --- Filter bus binding (sid.md §5.2) --------------------------------------
// Routing only -- tonal params (cutoff/resonance/mode) now live in the
// selected instrument itself (instruments.h) and are read directly by Core 1
// from the feeding voice's own instrument, not written through here. See
// audio_engine.cpp's P3 comment for why FilterBusParams goes unused by chip.
static int8_t chan_bus[16];                  // bus this channel owns, -1 = none
static int8_t bus_owner[FILTER_BUS_COUNT];   // channel owning this bus, -1 = free

static void release_bus(uint8_t ch) {
    if (chan_bus[ch] >= 0) {
        bus_owner[chan_bus[ch]] = -1;
        chan_bus[ch] = -1;
    }
}

// bind_filter(sid.md §5.2): 1. already owns a bus -> share/reuse it.
// 2. any free bus -> bind it. 3. none free -> BUS_NONE, render unfiltered
// (graceful degradation, and period-correct: "most voices in real tunes ran
// unfiltered, because the filter was scarce"). Driven by the selected
// instrument's uses_filter flag now, not a manual toggle.
static void bind_filter(VoiceParamBlock &shadow, uint8_t ch, const Instrument &ins) {
    if (!ins.uses_filter) {
        release_bus(ch);
        shadow.voices[ch].filter_bus = BUS_NONE;
        return;
    }
    if (chan_bus[ch] < 0) {
        for (uint8_t b = 0; b < FILTER_BUS_COUNT; b++) {
            if (bus_owner[b] < 0) { bus_owner[b] = (int8_t)ch; chan_bus[ch] = (int8_t)b; break; }
        }
    }
    shadow.voices[ch].filter_bus = (chan_bus[ch] >= 0) ? (uint8_t)chan_bus[ch] : BUS_NONE;
}

static void set_voice_freq(VoiceParamBlock &shadow, uint8_t voice, uint8_t note) {
    shadow.voices[voice].freq = chip_hz_to_freq_reg(chip_note_to_hz(note) * bend_ratio);
}

void midi_controller_init() {
    midi_parser.init();
    for (uint8_t c = 0; c < 16; c++) {
        chan_note[c] = -1;
        chan_instrument[c] = INS_ARP_LEAD;
        chan_bus[c] = -1;
    }
    for (uint8_t b = 0; b < FILTER_BUS_COUNT; b++) bus_owner[b] = -1;
    bend_ratio = 1.0f;

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
                if (ev.channel >= MAX_VOICES) break;   // no voice slot for this channel
                const Instrument &ins = INSTRUMENTS[chan_instrument[ev.channel]];
                VoiceParams &vp = shadow.voices[ev.channel];
                vp.type = VT_SID;
                set_voice_freq(shadow, ev.channel, ev.data1);
                vp.instrument = chan_instrument[ev.channel];
                vp.velocity = ev.data2 ? ev.data2 : 127;
                vp.gate = true;
                vp.trigger++;   // Core 1 hard-restarts the envelope + VM on this edge
                chan_note[ev.channel] = ev.data1;
                bind_filter(shadow, ev.channel, ins);

                ui_state.last_note = ev.data1;
                ui_state.last_velocity = ev.data2;
                ui_state.last_channel = ev.channel;
                changed = true;
                break;
            }

            case MIDI_NOTE_OFF:
                if (ev.channel < MAX_VOICES && chan_note[ev.channel] == ev.data1) {
                    shadow.voices[ev.channel].gate = false;
                    chan_note[ev.channel] = -1;
                    changed = true;
                }
                break;

            case MIDI_CC: {
                switch (ev.data1) {
                    case CC_INSTRUMENT: {
                        if (ev.channel >= 16) break;
                        uint8_t idx = (uint8_t)((uint32_t)ev.data2 * INSTRUMENT_COUNT / 128u);
                        chan_instrument[ev.channel] = idx;
                        if (ev.channel == ui_state.last_channel) ui_state.program = idx;
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
                uint16_t bend14 = (uint16_t)(ev.data1 | (ev.data2 << 7));
                float semitones = ((float)bend14 - (float)PITCH_BEND_CENTER)
                                  / (float)PITCH_BEND_CENTER * PITCH_BEND_RANGE_SEMITONES;
                bend_ratio = powf(2.0f, semitones / 12.0f);
                if (ev.channel < MAX_VOICES && chan_note[ev.channel] >= 0) {
                    set_voice_freq(shadow, ev.channel, (uint8_t)chan_note[ev.channel]);
                    changed = true;
                }
                ui_state.bend = (int16_t)((int)bend14 - PITCH_BEND_CENTER);
                break;
            }

            case MIDI_PROGRAM_CHANGE:
                if (ev.channel < 16 && ev.data1 < INSTRUMENT_COUNT) {
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
