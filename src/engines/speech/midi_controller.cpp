#include "midi_controller.h"
#include "midi_parser.h"
#include "voice_alloc.h"
#include "excitation.h"
#include "phonemes.h"
#include "phrases.h"
#include "presets.h"
#include <cmath>

// Phoneme keyboard (#28, module_speech.md MIDI Mapping Phase 1 "SPEECH_HOLD"): one
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
// these two are genuinely live (module_speech.md: "the field that makes this an
// instrument"): the CC handler pushes the new value into every voice
// currently held on that channel, not just the per-channel default used by
// future notes, so sweeping the knob is audible mid-note.
//
// #36 (module_speech.md P4 "MIDI Mapping"/"Utterances and MIDI integration") fills
// out the rest of the BSP block (CC24-28) plus GM's own standard vibrato
// CCs (1, 76), and changes what Program Change means:
//
//   - Program Change now selects an utterance -- one of phrases.h's
//     SPEECH_PHRASES (#35's generated phrase bank), not a phoneme.
//     "Playing a line": one word/phrase held fixed by PC, sung at whatever
//     pitch each note-on carries. Phoneme selection for the #28 keyboard
//     stays CC20-only; a single PC message can no longer mean two different
//     things depending on channel_utterance, so PC had to pick one.
//   - CC23 keeps its #34 job (utterance select, live-reachable alternative
//     to PC for the same BSP-can't-send-PC reason CC20 exists), just
//     repointed at SPEECH_PHRASES instead of utterance.h's two P3 fixtures.
//   - CC28: phrase-bank mode toggle (<64 off / >=64 on). On: "playing the
//     words themselves" -- each NOTE_ON's own note number selects which
//     phrase plays (`note % SPEECH_PHRASE_COUNT`), overriding PC/CC23's
//     selection for that note, while still using the note's pitch for
//     glottal frequency exactly as always. Off (default): PC/CC23 pick the
//     phrase, note number only sets pitch.
//   - CC24/25/26: rate/jitter/shimmer (speech_rate_cc_to_q4_4() --
//     sequencer.h -- and raw 0-255 scaling -- excitation.h). Live, same
//     push-to-held-voices treatment as CC21/22.
//   - CC27: mode select (SpeechMode, #30) -- 3 bands, ONESHOT/GATED/LOOP.
//     Live-pushed like the others: changing it mid-utterance only affects
//     the *next* note-off/end-of-utterance decision, never the waveform
//     directly, so there's no click risk in pushing it live.
//   - CC1 (mod wheel) / CC76 (GM "Vibrato Rate"): lfo_depth/lfo_rate. CC1
//     is the same "mod wheel -> vibrato depth" convention the shared
//     src/midi/midi_controller.cpp and subtractive engine already use
//     (module_speech.md: "following the existing CC1/CC10 pattern in the
//     subtractive engine"); CC76 is GM's own standard vibrato-rate
//     controller, not a project-specific choice.
//
// #38 (module_speech.md P5 "Preset table") adds CC16: preset select
// (presets.h SpeechPreset / voice_apply_preset()), banded into
// SPEECH_PRESET_COUNT zones same shape as CC20/CC23. CC16 sits in the same
// BSP absolute-encoder bank (CC16-31) as CC20-28 and was the first free slot
// in it. Next-note only, like CC20/CC23 -- a preset changes utterance/mode/
// phoneme, and pushing those into an already-sequencing voice is exactly the
// kind of note-off-adjacent glitch #30's release-segment mechanism exists to
// avoid (#38 acceptance: "presets ... switchable live without glitching an
// in-flight utterance"). Loading a preset bulk-writes the same per-channel
// state CC21/22/24-27 individually own, so those CCs still work exactly as
// before -- they now tweak away from whatever the preset loaded rather than
// from a fixed power-on default, and a later preset select overwrites those
// tweaks again, the same "preset is a starting point" behaviour every other
// hardware synth with knobs *and* presets has.

static MidiParser midi_parser;
static int8_t midi_note_voice[128];
static bool   voice_held[MAX_VOICES];
static uint8_t voice_channel[MAX_VOICES];

static constexpr uint8_t NUM_CHANNELS = 16;
static uint8_t channel_phoneme[NUM_CHANNELS];         // Phoneme (phonemes.h), set by CC20
static int16_t channel_pan[NUM_CHANNELS];             // CC10, Q15
static int16_t channel_formant_shift[NUM_CHANNELS];   // CC21, Q8.8 (live)
static int16_t channel_bandwidth_scale[NUM_CHANNELS]; // CC22, Q8.8 (live)
static uint8_t channel_utterance[NUM_CHANNELS];        // Program Change or CC23: SPEECH_NO_UTTERANCE or an index into SPEECH_PHRASES
static uint8_t channel_rate[NUM_CHANNELS];             // CC24, Q4.4 (live)
static uint8_t channel_jitter[NUM_CHANNELS];           // CC25, 0-255 (live)
static uint8_t channel_shimmer[NUM_CHANNELS];          // CC26, 0-255 (live)
static SpeechMode channel_mode[NUM_CHANNELS];          // CC27 (live)
static bool    channel_phrase_bank[NUM_CHANNELS];      // CC28: note number selects the phrase
static float   channel_lfo_rate[NUM_CHANNELS];         // CC76, Hz (live)
static float   channel_lfo_depth[NUM_CHANNELS];        // CC1, 0-1 (live)
static uint8_t channel_preset[NUM_CHANNELS];           // CC16 (#38): last preset loaded, for UI only --
                                                         // individual field CCs above can drift it out of
                                                         // sync with presets[channel_preset[ch]], same as
                                                         // any hardware synth's knobs-vs-preset relationship
static bool    channel_chorus[NUM_CHANNELS];            // CC16 (#38): pr.chorus of the last preset loaded

static MidiUiState ui_state;

void midi_controller_ui_state(MidiUiState *out) { *out = ui_state; }

// Bulk-loads presets[preset_id] into channel `channel`'s per-field state --
// the same channel_* arrays CC21/22/24-27/etc. individually own, as if all
// of them had just been received at once. Routes the field mapping through
// voice_apply_preset() (presets.h) so that mapping is written once, not
// duplicated here field-by-field; `tmp`'s phase_inc/pan start at 0 since
// voice_index is irrelevant at load time (see voice_apply_preset()'s own
// comment) -- chorus pan/detune is (re-)applied for real once note-on knows
// the actual allocated voice slot, in midi_voice_on() below.
static void speech_load_preset(uint8_t channel, uint8_t preset_id) {
    preset_id = (uint8_t)(preset_id % SPEECH_PRESET_COUNT);
    const SpeechPreset &pr = presets[preset_id];
    VoiceParams tmp{};
    voice_apply_preset(tmp, pr);

    channel_preset[channel] = preset_id;
    channel_phoneme[channel] = tmp.phoneme;
    channel_utterance[channel] = tmp.utterance;
    channel_mode[channel] = tmp.mode;
    channel_rate[channel] = tmp.rate;
    channel_formant_shift[channel] = tmp.formant_shift;
    channel_bandwidth_scale[channel] = tmp.bandwidth_scale;
    channel_jitter[channel] = tmp.jitter;
    channel_shimmer[channel] = tmp.shimmer;
    channel_lfo_rate[channel] = tmp.lfo_rate;
    channel_lfo_depth[channel] = tmp.lfo_depth;
    channel_chorus[channel] = pr.chorus;
}

static void midi_voice_on(VoiceParamBlock &shadow, int v, uint8_t note, uint8_t velocity, uint8_t channel) {
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);

    VoiceParams &vp = shadow.voices[v];
    vp.phase_inc = glottal_phase_inc(freq, (float)SPEECH_RATE);
    vp.amplitude = (int16_t)(velocity * 258);
    vp.phoneme = channel_phoneme[channel];
    vp.pan = channel_pan[channel];
    vp.formant_shift = channel_formant_shift[channel];
    vp.bandwidth_scale = channel_bandwidth_scale[channel];
    // #36 phrase-bank mode (CC28): this note's own number picks the phrase,
    // overriding whatever PC/CC23 selected for the channel -- "playing the
    // words themselves" instead of "playing a line".
    vp.utterance = channel_phrase_bank[channel]
        ? (uint8_t)(note % SPEECH_PHRASE_COUNT)
        : channel_utterance[channel];
    vp.mode = channel_mode[channel];
    vp.rate = channel_rate[channel];
    vp.jitter = channel_jitter[channel];
    vp.shimmer = channel_shimmer[channel];
    vp.lfo_rate = channel_lfo_rate[channel];
    vp.lfo_depth = channel_lfo_depth[channel];
    // Robot chorus (#38, presets.h): overrides the CC10 pan set just above
    // and further perturbs phase_inc, keyed by this note-on's real allocated
    // voice slot `v` -- speech_load_preset() above only saw voice_index 0
    // when the preset was loaded, since the real slot isn't known until
    // voice_alloc_allocate() returns it (midi_controller_process(), below).
    if (channel_chorus[channel]) speech_chorus_apply(vp, (uint32_t)v);
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
        // PRESET_PHONEME_KEYBOARD's fields (presets.h) are exactly the
        // pre-#38 literal defaults this loop used to set field-by-field --
        // loading it here is not a behaviour change, just routed through the
        // same preset machinery CC16 (#38) uses at runtime.
        speech_load_preset(ch, PRESET_PHONEME_KEYBOARD);
        channel_pan[ch] = 0;
        channel_phrase_bank[ch] = false;
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
                    case 1:  // vibrato depth (mod wheel, #36 -- see header comment)
                        channel_lfo_depth[ev.channel] = (float)ev.data2 / 127.0f;
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].lfo_depth = channel_lfo_depth[ev.channel];
                        }
                        ui_state.mod = ev.data2;
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 10:  // pan (CC10) — 0=full left, 64=center, 127=full right
                        channel_pan[ev.channel] = (int16_t)(((int32_t)ev.data2 - 64) * 512);
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 16: {  // preset select (#38, presets.h) — split range into
                                // SPEECH_PRESET_COUNT bands, next-note only (see header comment)
                        uint8_t band = (uint8_t)((uint32_t)ev.data2 * SPEECH_PRESET_COUNT / 128u);
                        speech_load_preset(ev.channel, band);
                        ui_state.last_channel = ev.channel;
                        break;
                    }
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
                    case 23: {  // utterance/phrase select (#34, repointed at SPEECH_PHRASES by #36)
                        // Split range into SPEECH_PHRASE_COUNT + 1 bands,
                        // same shape as CC20/CC74's banding -- band 0 is
                        // "off" (SPEECH_NO_UTTERANCE, back to CC20's phoneme
                        // keyboard), bands 1..SPEECH_PHRASE_COUNT each pick
                        // one SPEECH_PHRASES entry. Next-note only, same as
                        // Program Change below.
                        uint8_t band = (uint8_t)((uint32_t)ev.data2 * (SPEECH_PHRASE_COUNT + 1) / 128u);
                        channel_utterance[ev.channel] = band == 0 ? SPEECH_NO_UTTERANCE : (uint8_t)(band - 1);
                        ui_state.last_channel = ev.channel;
                        break;
                    }
                    case 24:  // rate (#36, live — see header comment)
                        channel_rate[ev.channel] = speech_rate_cc_to_q4_4(ev.data2);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].rate = channel_rate[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 25:  // jitter (#36, live — see header comment)
                        channel_jitter[ev.channel] = (uint8_t)((uint32_t)ev.data2 * 255u / 127u);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].jitter = channel_jitter[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 26:  // shimmer (#36, live — see header comment)
                        channel_shimmer[ev.channel] = (uint8_t)((uint32_t)ev.data2 * 255u / 127u);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].shimmer = channel_shimmer[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 27: {  // mode select (#30/#36) — 3 bands: ONESHOT/GATED/LOOP, live
                        uint8_t band = (uint8_t)((uint32_t)ev.data2 * 3u / 128u);
                        SpeechMode m = band == 0 ? SPEECH_MODE_ONESHOT : (band == 1 ? SPEECH_MODE_GATED : SPEECH_MODE_LOOP);
                        channel_mode[ev.channel] = m;
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].mode = m;
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    }
                    case 28:  // phrase-bank mode toggle (#36, next-note — see header comment)
                        channel_phrase_bank[ev.channel] = ev.data2 >= 64;
                        ui_state.last_channel = ev.channel;
                        break;
                    case 72:  // effect param 1: delay feedback / reverb room size
                        shadow.fx.p1 = ev.data2;
                        ui_state.fx_p1 = ev.data2;
                        changed = true;
                        break;
                    case 73:  // effect wet/dry mix (global)
                        shadow.fx.mix = ev.data2;
                        ui_state.fx_mix = ev.data2;
                        changed = true;
                        break;
                    case 74:  // effect type select — split range into FX_COUNT bands
                        shadow.fx.type = (uint8_t)((uint32_t)ev.data2 * FX_COUNT / 128u);
                        ui_state.fx_type = shadow.fx.type;
                        changed = true;
                        break;
                    case 75:  // effect param 2: delay time / reverb damping
                        shadow.fx.p2 = ev.data2;
                        ui_state.fx_p2 = ev.data2;
                        changed = true;
                        break;
                    case 76:  // vibrato rate (GM standard "Vibrato Rate", #36 — live)
                        channel_lfo_rate[ev.channel] = (float)ev.data2 / 127.0f * SPEECH_LFO_RATE_HZ_MAX;
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].lfo_rate = channel_lfo_rate[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    default:  break;
                }
                break;
            }
            case MIDI_PROGRAM_CHANGE: {
                // #36: Program Change now selects an utterance (SPEECH_PHRASES,
                // phrases.h), not a phoneme -- see header comment. Affects
                // future notes only, same as every other "next note" CC.
                channel_utterance[ev.channel] = ev.data1 % SPEECH_PHRASE_COUNT;
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
