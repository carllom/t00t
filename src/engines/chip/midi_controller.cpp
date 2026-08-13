#include "midi/midi_controller.h"
#include "midi_parser.h"
#include "chip/sid_osc.h"   // SID_CLOCK_PAL, sid_freq_to_inc's freq_reg convention
#include <cmath>

// Chip module MIDI routing (Core 0), sid.md §1 P1 / §8: "static assignment.
// MIDI channel -> voice, no allocator." MIDI channel N plays SID voice N,
// monophonic per channel (matching main.cpp's own convention comment: "the
// tracker engine assigns channel N to voice N directly"). Channels 16..31 of
// the MAX_VOICES=32 pool sit unreachable until P4's dynamic allocator; P1's
// job is proving a voice sounds right, not full polyphony management.
//
// No instrument system yet (P4: .ins import) -- a fixed default patch plus
// two live CCs (waveform, pulse width) are enough to hear and compare the
// primitives against reSID, which is what this phase is actually for.

// --- Live-patch CCs (BeatStep-Pro-friendly low CC numbers, matching
// groovebox's convention -- see the BSP quirk note in project memory: its
// encoders are absolute CC, not Program Change) --------------------------
enum ChipCC : uint8_t {
    CC_WAVEFORM = 16,   // 0-31 saw, 32-63 pulse, 64-95 triangle, 96-127 noise
    CC_PULSE_WIDTH = 17,   // 0..127 -> 0..4095 (12-bit SID PW)
    CC_FX_TYPE  = 74,   // effect select (engine_base.h EffectParams convention)
    CC_FX_MIX   = 73,
    CC_FX_P1    = 72,
    CC_FX_P2    = 75,
};

static constexpr uint16_t PITCH_BEND_CENTER = 8192;
static constexpr float    PITCH_BEND_RANGE_SEMITONES = 2.0f;

// Fixed default patch (P4 replaces this with real instrument data). Attack 2
// (~16 ms), decay 8 (~100 ms), sustain 10 (level 0xAA), release 6 (~68 ms) --
// a generic pluck, chosen only to have *something* with all four ADSR
// segments audible for a by-ear check against reSID.
static constexpr uint8_t DEFAULT_AD = (2 << 4) | 8;
static constexpr uint8_t DEFAULT_SR = (10 << 4) | 6;

static MidiParser midi_parser;
static MidiUiState ui_state;

// Per-channel state so pitch bend and waveform/PW CCs can recompute the
// currently-held note (mirrors groovebox's g_303_note pattern).
static int16_t chan_note[16];      // -1 = nothing held on this channel
static uint8_t chan_waveform = 0x2;   // SID_WAVE_SAW (sid_osc.h's SidWave bits)
static uint16_t chan_pw = 0x800;      // 50% duty
static float    bend_ratio = 1.0f;    // multiplies the frequency register

static float note_to_hz(uint8_t note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

static uint16_t hz_to_sid_freq_reg(float hz) {
    // inc = freq_reg * (clock/rate*256); solving sid_freq_to_inc's contract
    // for freq_reg directly (sid.md §4.1's "16-bit frequency register"):
    // freq_reg = hz * 2^24 / clock_hz.
    float reg = hz * 16777216.0f / (float)SID_CLOCK_PAL;
    if (reg < 0.0f) reg = 0.0f;
    if (reg > 65535.0f) reg = 65535.0f;
    return (uint16_t)(reg + 0.5f);
}

static void set_voice_freq(VoiceParamBlock &shadow, uint8_t voice, uint8_t note) {
    shadow.voices[voice].freq = hz_to_sid_freq_reg(note_to_hz(note) * bend_ratio);
}

void midi_controller_init() {
    midi_parser.init();
    for (uint8_t c = 0; c < 16; c++) chan_note[c] = -1;
    chan_waveform = 0x2;
    chan_pw = 0x800;
    bend_ratio = 1.0f;

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
                if (ev.channel >= MAX_VOICES) break;   // no voice slot for this channel
                VoiceParams &vp = shadow.voices[ev.channel];
                vp.type = VT_SID;
                set_voice_freq(shadow, ev.channel, ev.data1);
                vp.pw = chan_pw;
                vp.waveform = chan_waveform;
                vp.ad = DEFAULT_AD;
                vp.sr = DEFAULT_SR;
                vp.velocity = ev.data2 ? ev.data2 : 127;
                vp.gate = true;
                vp.trigger++;   // Core 1 hard-restarts the envelope on this edge
                chan_note[ev.channel] = ev.data1;

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
                    case CC_WAVEFORM: {
                        // sid_osc.h's SidWave bits: TRI=0x1 SAW=0x2 PULSE=0x4 NOISE=0x8.
                        uint8_t band = (uint8_t)(ev.data2 >> 5);   // 0..3 from 0..127
                        chan_waveform = band == 0 ? 0x2 : band == 1 ? 0x4 : band == 2 ? 0x1 : 0x8;
                        for (uint8_t c = 0; c < 16 && c < MAX_VOICES; c++)
                            if (chan_note[c] >= 0) shadow.voices[c].waveform = chan_waveform;
                        changed = true;
                        break;
                    }
                    case CC_PULSE_WIDTH: {
                        chan_pw = (uint16_t)((uint32_t)ev.data2 * 4095u / 127u);
                        for (uint8_t c = 0; c < 16 && c < MAX_VOICES; c++)
                            if (chan_note[c] >= 0) shadow.voices[c].pw = chan_pw;
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

            default: break;   // no sequencer/transport in this engine
        }
    }

    if (changed) params->commit();
}

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }
