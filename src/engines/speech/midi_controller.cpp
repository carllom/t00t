#include "midi_controller.h"
#include "midi_parser.h"
#include "voice_alloc.h"
#include "excitation.h"
#include "phonemes.h"
#include "phrases.h"
#include "presets.h"
#include <cmath>

#ifdef T00T_SPEECH_HAS_LATTICE_WORDS
#include "lattice_words.h"
#endif

// Phoneme keyboard: one MIDI note is one sustained phoneme (module_speech.md
// "MIDI Mapping"). Note number sets glottal pitch, gate sustains the
// phoneme. The phoneme new notes on a channel use is selected by Program
// Change or by CC20 (banded into PHONEME_COUNT zones, same shape as CC74's
// effect-type select) -- CC20 exists because not every controller can send
// Program Change from a live control: an Arturia BeatStep Pro encoder
// assigned to "PC" sends CC instead, since its encoders are absolute CC by
// default, CC16-31 (see groovebox's midi_controller.cpp). CC20 sits in that
// same BSP range so the same physical knob still works. Own controller, not
// the shared src/midi/midi_controller.cpp: this engine's VoiceParams has no
// VoicePreset shape for the shared controller's presets.h to apply.
//
// formant_shift (CC21) and bandwidth_scale (CC22) sit right after CC20 in
// the same BSP bank. Unlike pan/phoneme, which take effect on the next
// note-on only, these two are genuinely live: the CC handler pushes the new
// value into every voice currently held on the channel, not just the
// per-channel default for future notes. rate/jitter/shimmer/mode (CC24-27)
// and vibrato depth/rate (CC1/CC76) are live the same way.
//
// Program Change and CC23 both select an utterance (phrases.h's
// SPEECH_PHRASES) under the formant tract; CC20 stays phoneme-only, since a
// single PC message can't mean both. CC28 (phrase-bank mode, next-note)
// makes each note-on's own note number pick the phrase instead of PC/CC23's
// selection -- "playing the words themselves" rather than "playing a line"
// -- while pitch still comes from the note as always.
//
// Under the LPC lattice tract (CC102 >= 64), Program Change means something
// else entirely: KEY_PER_WORD addressing (module_speech.md "LPC Lattice
// Tract") maps the note number straight to a word within a 128-word page of
// the converted corpus, and Program Change picks which page is current --
// speech_lattice_word_for_key() below resolves note+page to a
// LATTICE_WORDS[] entry. CC23 keeps its formant-only meaning; there's no
// LPC equivalent of "off" the way band 0 gives CC23 for the formant tract,
// since every note always addresses some word.
//
// CC16 selects a preset (presets.h SpeechPreset / voice_apply_preset()),
// next-note only like CC20/CC23: a preset can set utterance/mode/phoneme,
// and pushing those into an already-sequencing voice would be exactly the
// note-off-adjacent glitch the release-segment mechanism (module_speech.md
// "Voice lifetime and note-off") exists to avoid. Loading a preset
// bulk-writes the same per-channel state CC21/22/24-27 individually own
// (speech_load_preset() below), so those CCs still work afterward -- they
// tweak away from whatever the preset loaded, and selecting a different
// preset overwrites those tweaks again.
//
// See module_speech.md's "MIDI Mapping" for the full CC table and the
// reasoning behind CC1/CC10/CC76 following the GM standard while CC16/
// CC20-28 stay in the BeatStep Pro's contiguous absolute-CC block.

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
static SpeechTract channel_tract[NUM_CHANNELS];        // CC102, next-note: formant vs. LPC lattice tract
static uint8_t channel_lattice_page[NUM_CHANNELS];      // Program Change under the LPC tract: KEY_PER_WORD page
static int16_t channel_lattice_pitch_shift[NUM_CHANNELS]; // CC103, Q8.8 (live)
static uint8_t channel_preset[NUM_CHANNELS];           // CC16: last preset loaded, for UI only --
                                                         // individual field CCs above can drift it out of
                                                         // sync with presets[channel_preset[ch]], same as
                                                         // any hardware synth's knobs-vs-preset relationship
static bool    channel_chorus[NUM_CHANNELS];            // CC16: pr.chorus of the last preset loaded

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
    channel_tract[channel] = tmp.tract;
    channel_lattice_page[channel] = pr.lattice_page;
    channel_lattice_pitch_shift[channel] = tmp.lattice_pitch_shift;
}

// KEY_PER_WORD addressing (module_speech.md "LPC Lattice Tract"): note
// number selects a word within the channel's current 128-word page.
// LPC_PAGE_WORDS matches the acceptance-tested keyboard span (0-127), not
// PHONEME_COUNT/SPEECH_PHRASE_COUNT-style corpus size -- the page is what
// makes a >128-word corpus reachable a page at a time. Wraps with `%`
// rather than clamping, the same out-of-range convention every other
// table lookup in this file already uses (channel_phoneme % PHONEME_COUNT,
// preset_id % SPEECH_PRESET_COUNT, etc.) -- a corpus whose last page is
// only partially populated still gives every key a defined word rather
// than silence. Without a generated corpus (lattice_words.h), every key
// and every page plays the same lattice.h LATTICE_TEST_WORD.
static constexpr uint32_t LPC_PAGE_WORDS = 128;

static const LatticeWord *speech_lattice_word_for_key(uint8_t channel, uint8_t note) {
#ifdef T00T_SPEECH_HAS_LATTICE_WORDS
    uint32_t idx = (uint32_t)channel_lattice_page[channel] * LPC_PAGE_WORDS + note;
    return &LATTICE_WORDS[idx % LATTICE_WORD_COUNT];
#else
    (void)channel;
    (void)note;
    return &LATTICE_TEST_WORD;
#endif
}

static void midi_voice_on(VoiceParamBlock &shadow, int v, uint8_t note, uint8_t velocity, uint8_t channel) {
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);

    VoiceParams &vp = shadow.voices[v];
    vp.phase_inc = glottal_phase_inc(freq, (float)SPEECH_RATE);
    vp.amplitude = (int16_t)(velocity * 258);
    vp.phoneme = channel_phoneme[channel];
    vp.tract = channel_tract[channel];
    vp.lattice_word = speech_lattice_word_for_key(channel, note);
    vp.lattice_pitch_shift = channel_lattice_pitch_shift[channel];
    vp.pan = channel_pan[channel];
    vp.formant_shift = channel_formant_shift[channel];
    vp.bandwidth_scale = channel_bandwidth_scale[channel];
    // Phrase-bank mode (CC28): this note's own number picks the phrase,
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
    // Robot chorus (presets.h): overrides the CC10 pan set just above
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
        // PRESET_PHONEME_KEYBOARD's fields (presets.h) match this engine's
        // power-on defaults; loading it here routes those defaults through
        // the same preset machinery CC16 uses at runtime, rather than
        // duplicating the field values -- including tract/lattice_page/
        // lattice_pitch_shift, now that presets carry those too.
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
                    case 1:  // vibrato depth (mod wheel) -- see header comment
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
                    case 16: {  // preset select (presets.h) — split range into
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
                    case 21:  // formant_shift, live — see header comment
                        channel_formant_shift[ev.channel] = tract_cc_to_q8_8(ev.data2, FORMANT_SHIFT_MIN, FORMANT_SHIFT_MAX);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].formant_shift = channel_formant_shift[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 22:  // bandwidth_scale, live — see header comment
                        channel_bandwidth_scale[ev.channel] = tract_cc_to_q8_8(ev.data2, BANDWIDTH_SCALE_MIN, BANDWIDTH_SCALE_MAX);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].bandwidth_scale = channel_bandwidth_scale[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 23: {  // utterance/phrase select, indexes SPEECH_PHRASES
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
                    case 24:  // rate, live — see header comment
                        channel_rate[ev.channel] = speech_rate_cc_to_q4_4(ev.data2);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].rate = channel_rate[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 25:  // jitter, live — see header comment
                        channel_jitter[ev.channel] = (uint8_t)((uint32_t)ev.data2 * 255u / 127u);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].jitter = channel_jitter[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 26:  // shimmer, live — see header comment
                        channel_shimmer[ev.channel] = (uint8_t)((uint32_t)ev.data2 * 255u / 127u);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].shimmer = channel_shimmer[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 27: {  // mode select — 3 bands: ONESHOT/GATED/LOOP, live
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
                    case 28:  // phrase-bank mode toggle, next-note — see header comment
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
                    case 102:  // tract select, next-note only -- formant vs. LPC lattice
                        channel_tract[ev.channel] = ev.data2 >= 64 ? SPEECH_TRACT_LATTICE : SPEECH_TRACT_FORMANT;
                        ui_state.last_channel = ev.channel;
                        break;
                    case 103:  // LPC pitch-shift multiplier, live -- see header comment
                        channel_lattice_pitch_shift[ev.channel] =
                            tract_cc_to_q8_8(ev.data2, SPEECH_LATTICE_PITCH_SHIFT_MIN, SPEECH_LATTICE_PITCH_SHIFT_MAX);
                        for (uint32_t v = 0; v < MAX_VOICES; v++) {
                            if (voice_held[v] && voice_channel[v] == ev.channel)
                                shadow.voices[v].lattice_pitch_shift = channel_lattice_pitch_shift[ev.channel];
                        }
                        ui_state.last_channel = ev.channel;
                        changed = true;
                        break;
                    case 76:  // vibrato rate (GM standard "Vibrato Rate"), live
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
                // Tract-dependent (see header comment): under the LPC
                // lattice tract, picks the KEY_PER_WORD page instead of a
                // formant utterance -- no cross-talk between the two, since
                // a channel only ever reads one of channel_utterance/
                // channel_lattice_page, chosen by channel_tract at note-on.
                // Affects future notes only, same as every other "next
                // note" CC.
                if (channel_tract[ev.channel] == SPEECH_TRACT_LATTICE) {
#ifdef T00T_SPEECH_HAS_LATTICE_WORDS
                    constexpr uint32_t LPC_PAGE_COUNT = (LATTICE_WORD_COUNT + LPC_PAGE_WORDS - 1) / LPC_PAGE_WORDS;
                    channel_lattice_page[ev.channel] = (uint8_t)(ev.data1 % LPC_PAGE_COUNT);
#endif
                } else {
                    channel_utterance[ev.channel] = ev.data1 % SPEECH_PHRASE_COUNT;
                }
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
