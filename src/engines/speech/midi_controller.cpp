#include "midi_controller.h"
#include "midi_parser.h"
#include "voice_alloc.h"
#include "excitation.h"
#include "phonemes.h"
#include "utterance.h"
#include <cmath>

// Phoneme keyboard (#28, speech.md MIDI Mapping Phase 1 "SPEECH_HOLD"): one
// MIDI note is one sustained phoneme. Note number sets glottal pitch, gate
// sustains the phoneme; the phoneme new notes on a channel use is selectable
// two ways -- Program Change, or CC20 (banded into PHONEME_COUNT zones, same
// shape as CC74's effect-type select). CC20 exists because not every
// controller can actually send Program Change from a live control -- an
// Arturia BeatStep Pro encoder assigned to "PC" in its editor was found to
// emit something else entirely (its encoders are absolute CC by default,
// CC16-31 -- see groovebox's midi_controller.cpp), so a real synth with a
// genuine PC message worked while the BSP didn't. CC20 sits in that same
// BSP encoder range so the same physical knob can drive phoneme selection
// without needing to be reconfigured. Own controller (not the shared
// src/midi/midi_controller.cpp) for the same reason the #27 skeleton stub
// gave: this engine's VoiceParams has no VoicePreset shape for the shared
// controller's presets.h to apply.
//
// formant_shift (CC21) and bandwidth_scale (CC22, #29) sit right after
// CC20 in that same BSP bank. Unlike pan/phoneme -- which only take effect
// on the *next* note-on, matching every other engine's "next note" CCs --
// these two are genuinely live (speech.md: "the field that makes this an
// instrument"): the CC handler pushes the new value into every voice
// currently held on that channel, not just the per-channel default used by
// future notes, so sweeping the knob is audible mid-note.
//
// CC23 (#34) selects one of utterance.h's hardcoded SPEECH_UTTERANCES
// instead of the CC20 phoneme keyboard: 0 = off (SPEECH_NO_UTTERANCE, the
// existing #28 phoneme-keyboard path, CC20 keeps working exactly as
// before); 1..SPEECH_UTTERANCE_COUNT band the rest of the CC range, same
// shape as CC20's phoneme bands. Exists so #34's sequencer is reachable
// from a real controller for on-device verification -- the full CC map
// (rate, jitter, shimmer, program-change utterance/phrase selection) is
// speech.md P4 "MIDI integration" work, not this slice's. Mode is fixed at
// SPEECH_MODE_GATED (#30's default) and rate at 1.0x (VoiceParams default)
// for the same reason: no CC owns them yet.

static MidiParser midi_parser;
static int8_t midi_note_voice[128];
static bool   voice_held[MAX_VOICES];
static uint8_t voice_channel[MAX_VOICES];

static constexpr uint8_t NUM_CHANNELS = 16;
static uint8_t channel_phoneme[NUM_CHANNELS];         // Phoneme (phonemes.h), set by program change
static int16_t channel_pan[NUM_CHANNELS];             // CC10, Q15
static int16_t channel_formant_shift[NUM_CHANNELS];   // CC21, Q8.8 (live)
static int16_t channel_bandwidth_scale[NUM_CHANNELS]; // CC22, Q8.8 (live)
static uint8_t channel_utterance[NUM_CHANNELS];       // CC23 (#34): SPEECH_NO_UTTERANCE or an index into SPEECH_UTTERANCES

static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

static void midi_voice_on(VoiceParamBlock &shadow, int v, uint8_t note, uint8_t velocity, uint8_t channel) {
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);

    VoiceParams &vp = shadow.voices[v];
    vp.phase_inc = glottal_phase_inc(freq, (float)SPEECH_RATE);
    vp.amplitude = (int16_t)(velocity * 258);
    vp.phoneme = channel_phoneme[channel];
    vp.pan = channel_pan[channel];
    vp.formant_shift = channel_formant_shift[channel];
    vp.bandwidth_scale = channel_bandwidth_scale[channel];
    vp.utterance = channel_utterance[channel];
    vp.mode = SPEECH_MODE_GATED;  // #30 default -- no mode CC yet, see header comment
    vp.rate = 16;                 // 1.0x -- no rate CC yet, see header comment
    vp.trigger++;
    vp.gate = true;
    voice_held[v] = true;
    voice_channel[v] = channel;

    ui_state.last_note = note;
    ui_state.last_velocity = velocity;
    ui_state.last_channel = channel;
    ui_state.program = channel_phoneme[channel];
}

void midi_controller_init() {
    midi_parser.init();
    for (int i = 0; i < 128; i++) midi_note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        channel_phoneme[ch] = PH_I;
        channel_pan[ch] = 0;
        channel_formant_shift[ch] = 256;    // 1.0x, neutral
        channel_bandwidth_scale[ch] = 256;  // 1.0x, neutral
        channel_utterance[ch] = SPEECH_NO_UTTERANCE;
    }
    ui_state.last_note = 0xFF;
    ui_state.last_velocity = 0;
    ui_state.last_channel = 0;
    ui_state.program = PH_I;
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
                    midi_voice_on(shadow, v, note, ev.data2, ev.channel);
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
                    case 10:  // pan (CC10) — 0=full left, 64=center, 127=full right
                        channel_pan[ev.channel] = (int16_t)(((int32_t)ev.data2 - 64) * 512);
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 20:  // phoneme select — split range into PHONEME_COUNT bands (see
                              // header comment: alternative to Program Change for
                              // controllers that can't send it from a live encoder)
                        channel_phoneme[ev.channel] = (uint8_t)((uint32_t)ev.data2 * PHONEME_COUNT / 128u);
                        ui_state.program = channel_phoneme[ev.channel];
                        ui_state.last_channel = ev.channel;
                        break;
                    case 21:  // formant_shift (#29, live — see header comment)
                        channel_formant_shift[ev.channel] = tract_cc_to_q8_8(ev.data2, FORMANT_SHIFT_MIN, FORMANT_SHIFT_MAX);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].formant_shift = channel_formant_shift[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 22:  // bandwidth_scale (#29, live — see header comment)
                        channel_bandwidth_scale[ev.channel] = tract_cc_to_q8_8(ev.data2, BANDWIDTH_SCALE_MIN, BANDWIDTH_SCALE_MAX);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].bandwidth_scale = channel_bandwidth_scale[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 74:  // effect type select — split range into FX_COUNT bands
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case 73:  // effect wet/dry mix (global)
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case 72:  // effect param 1: delay feedback / reverb room size
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case 75:  // effect param 2: delay time / reverb damping
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    case 23: {  // utterance select (#34) — see header comment
                        // Split range into SPEECH_UTTERANCE_COUNT + 1 bands,
                        // same shape as CC20/CC74's banding -- band 0 is
                        // "off" (SPEECH_NO_UTTERANCE, back to CC20's phoneme
                        // keyboard), bands 1..SPEECH_UTTERANCE_COUNT each
                        // pick one SPEECH_UTTERANCES entry.
                        uint8_t band = (uint8_t)((uint32_t)ev.data2 * (SPEECH_UTTERANCE_COUNT + 1) / 128u);
                        channel_utterance[ev.channel] = band == 0 ? SPEECH_NO_UTTERANCE : (uint8_t)(band - 1);
                        ui_state.last_channel = ev.channel;
                        break;
                    }
                    default:  break;  // other CCs — full MIDI integration (rate/jitter/shimmer/mode/phrase select) lands in P4
                }
                break;
            }
            case MIDI_PROGRAM_CHANGE: {
                // Affects future notes only, same as the shared controller's
                // channel_program.
                channel_phoneme[ev.channel] = ev.data1 % PHONEME_COUNT;
                ui_state.program = channel_phoneme[ev.channel];
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
