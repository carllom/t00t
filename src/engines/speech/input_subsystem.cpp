#include "midi_controller.h"
#include "midi_parser.h"
#include "midi_controller_generic.h"
#include "voice_alloc.h"
#include "excitation.h"
#include "phonemes.h"
#include "phrases.h"
#include "presets.h"
#include <cmath>

#ifdef T00T_SPEECH_HAS_LATTICE_WORDS
#include "lattice_words.h"
#endif

// Speech's Input subsystem: the module-specific tail of the Input pipeline
// -- mapping table, Handlers, and the Voice Allocation Interface calls they
// make. MIDI bytes reach it via midi_controller_process(), built from
// midi_dispatch.h's shared, module-agnostic generic dispatch helpers
// (src/midi/midi_controller_generic.h) -- the mechanism is shared, the
// mapping table and Handlers are fully this module's own.
//
// Program Change picks a preset (CONFIGURATION), same convention every
// migrated module follows -- but here `presets[]` (presets.h) and
// `lpc_page_presets` (its generated LPC-lattice-page extension) are one
// contiguous address space: index < SPEECH_PRESET_COUNT loads a named
// preset, anything at or past it loads an LPC page. A preset is what a
// voice sounds like *and* which tract it plays through -- the formant/SAM
// "select an utterance" and LPC lattice "select a page" behaviors used to
// be two different things Program Change meant depending on the channel's
// current tract; they're the same operation now (load a preset, whichever
// kind it is), so this Handler never branches on tract at all. CC16
// (preset select) is gone -- it was the exact same "load a preset" op
// Program Change now owns outright, not a second axis.
//
// CC28 (phrase-bank mode) changes what a note's own number means at the
// next note-on (pitch only vs. pitch + phrase-select) -- ordinary
// Handler-owned state, set_note() reads channel_phrase_bank[] the same way
// it reads every other channel_* default. No category/mechanism
// involvement.

static int8_t note_voice[128];
static bool   voice_held[MAX_VOICES];
static uint8_t voice_channel[MAX_VOICES];

static constexpr uint8_t NUM_CHANNELS = 16;
static uint8_t channel_phoneme[NUM_CHANNELS];         // Phoneme (phonemes.h), set by CC20
static int16_t channel_pan[NUM_CHANNELS];             // CC10, Q15
static int16_t channel_formant_shift[NUM_CHANNELS];   // CC21, Q8.8 (live)
static int16_t channel_bandwidth_scale[NUM_CHANNELS]; // CC22, Q8.8 (live)
static uint8_t channel_utterance[NUM_CHANNELS];        // CC23: SPEECH_NO_UTTERANCE or an index into SPEECH_PHRASES
static uint8_t channel_rate[NUM_CHANNELS];             // CC24, Q4.4 (live)
static uint8_t channel_jitter[NUM_CHANNELS];           // CC25, 0-255 (live)
static uint8_t channel_shimmer[NUM_CHANNELS];          // CC26, 0-255 (live)
static SpeechMode channel_mode[NUM_CHANNELS];          // CC27 (live)
static bool    channel_phrase_bank[NUM_CHANNELS];      // CC28: note number selects the phrase
static float   channel_lfo_rate[NUM_CHANNELS];         // CC76, Hz (live)
static float   channel_lfo_depth[NUM_CHANNELS];        // CC1, 0-1 (live)
static SpeechTract channel_tract[NUM_CHANNELS];        // CC102, next-note: formant/LPC lattice/SAM
static uint8_t channel_lattice_page[NUM_CHANNELS];      // Program Change (LPC-page presets): KEY_PER_WORD page
static int16_t channel_lattice_pitch_shift[NUM_CHANNELS]; // CC103, Q8.8 (live)
static bool    channel_lattice_chirp_exciter[NUM_CHANNELS]; // CC104, live: glottal_pulse() vs. TMS5220 chirp table
static int16_t channel_sam_throat[NUM_CHANNELS];        // CC105, Q8.8 (live)
static int16_t channel_sam_mouth[NUM_CHANNELS];         // CC106, Q8.8 (live)
static bool    channel_chorus[NUM_CHANNELS];            // Program Change: pr.chorus of the last preset loaded
static bool    channel_velocity_enabled[NUM_CHANNELS];  // CC15, next-note: use received velocity vs. fixed max

// Applies a preset (named or an LPC page alike, presets.h) to channel
// `channel`'s per-field state -- the same channel_* arrays CC21/22/24-27/
// etc. individually own, as if all of them had just been received at once.
// Routes the field mapping through voice_apply_preset() (presets.h) so
// that mapping is written once, not duplicated here field-by-field;
// `tmp`'s phase_inc/pan start at 0 since voice_index is irrelevant at load
// time (see voice_apply_preset()'s own comment) -- chorus pan/detune is
// (re-)applied for real once note-on knows the actual allocated voice
// slot, in set_note() below.
static void speech_apply_preset(uint8_t channel, const SpeechPreset &pr) {
    VoiceParams tmp{};
    voice_apply_preset(tmp, pr);

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
    channel_lattice_chirp_exciter[channel] = tmp.lattice_chirp_exciter;
    channel_sam_throat[channel] = tmp.sam_throat;
    channel_sam_mouth[channel] = tmp.sam_mouth;
}

// KEY_PER_WORD addressing (module_speech.md "LPC Lattice Tract"): note
// number selects a word within the channel's current 128-word page.
// Wraps with `%` rather than clamping, the same out-of-range convention
// every other table lookup in this file already uses -- a corpus whose
// last page is only partially populated still gives every key a defined
// word rather than silence. Without a generated corpus (lattice_words.h),
// every key and every page plays the same lattice.h LATTICE_TEST_WORD.
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

// Note Handler: the Voice Allocation Interface -- this Handler is where
// voice_alloc_allocate()/release() actually get called, never upstream in
// parsing/dispatch. Resolves its own voice via note_voice[], keyed by note
// number, steal-on-retrigger included -- same pattern subtractive/fm/chip
// use.
static void set_note(VoiceParamBlock &shadow, const InputValue &value) {
    if (value.note_on) {
        if (note_voice[value.note] >= 0) {
            int8_t old = note_voice[value.note];
            shadow.voices[old].gate = false;
            voice_held[old] = false;
            voice_alloc_release(old);
        }
        int v = voice_alloc_allocate();
        if (v < 0) return;
        note_voice[value.note] = (int8_t)v;
        voice_held[v] = true;
        voice_channel[v] = value.channel;

        uint8_t channel = value.channel;
        uint8_t note = value.note;
        float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
        // CC15: when velocity is toggled off, every note sounds at max
        // velocity regardless of what the controller actually sent --
        // ui_state.last_velocity below still shows the real received value.
        uint8_t sounding_velocity = channel_velocity_enabled[channel] ? value.velocity : 127;

        VoiceParams &vp = shadow.voices[v];
        vp.phase_inc = glottal_phase_inc(freq, (float)SPEECH_RATE);
        vp.amplitude = (int16_t)(sounding_velocity * 258);
        vp.phoneme = channel_phoneme[channel];
        vp.tract = channel_tract[channel];
        vp.lattice_word = speech_lattice_word_for_key(channel, note);
        vp.lattice_pitch_shift = channel_lattice_pitch_shift[channel];
        vp.lattice_chirp_exciter = channel_lattice_chirp_exciter[channel];
        vp.sam_throat = channel_sam_throat[channel];
        vp.sam_mouth = channel_sam_mouth[channel];
        vp.pan = channel_pan[channel];
        vp.formant_shift = channel_formant_shift[channel];
        vp.bandwidth_scale = channel_bandwidth_scale[channel];
        // Phrase-bank mode (CC28): this note's own number picks the
        // phrase, overriding whatever Program Change selected for the
        // channel -- "playing the words themselves" instead of "playing a
        // line".
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
        // and further perturbs phase_inc, keyed by this note-on's real
        // allocated voice slot `v` -- speech_apply_preset() above only saw
        // voice_index 0 when the preset was loaded, since the real slot
        // isn't known until voice_alloc_allocate() returns it.
        if (channel_chorus[channel]) speech_chorus_apply(vp, (uint32_t)v);
        vp.trigger++;
        vp.gate = true;

        ui_state.last_note = note;
        ui_state.last_velocity = value.velocity;
        ui_state.last_channel = channel;
        ui_state.program = channel_phoneme[channel];
    } else {
        int8_t v = note_voice[value.note];
        if (v < 0) return;
        shadow.voices[v].gate = false;
        voice_held[v] = false;
        voice_alloc_release(v);
        note_voice[value.note] = -1;
    }
}

// Modifier setters take the raw 0-127 CC byte via value.scalar and do
// their own source-native -> module-native conversion, the same pattern
// every migrated module's CC Handlers use. "Live" ones also push the new
// value into every currently-held voice on the channel, not just the
// per-channel default for future notes.
static void set_vibrato_depth(VoiceParamBlock &shadow, const InputValue &value) {  // CC1 (GM mod wheel), live
    channel_lfo_depth[value.channel] = value.scalar / 127.0f;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].lfo_depth = channel_lfo_depth[value.channel];
    }
    ui_state.mod = (uint8_t)value.scalar;
    ui_state.last_channel = value.channel;
}

static void set_pan(VoiceParamBlock &, const InputValue &value) {  // CC10 (GM pan), next note
    channel_pan[value.channel] = (int16_t)(((int32_t)value.scalar - 64) * 512);
    ui_state.last_channel = value.channel;
}

static void set_velocity_toggle(VoiceParamBlock &, const InputValue &value) {  // CC15, next note --
    // 0-63 off (every note at max velocity), 64-127 on (received velocity), default on
    channel_velocity_enabled[value.channel] = value.scalar >= 64.0f;
    ui_state.last_channel = value.channel;
}

static void set_phoneme(VoiceParamBlock &, const InputValue &value) {  // CC20, next note --
    // phoneme keyboard index, banded into PHONEME_COUNT zones
    channel_phoneme[value.channel] = (uint8_t)((uint32_t)value.scalar * PHONEME_COUNT / 128u);
    ui_state.program = channel_phoneme[value.channel];
    ui_state.last_channel = value.channel;
}

static void set_formant_shift(VoiceParamBlock &shadow, const InputValue &value) {  // CC21, live
    channel_formant_shift[value.channel] = tract_cc_to_q8_8((uint8_t)value.scalar, FORMANT_SHIFT_MIN, FORMANT_SHIFT_MAX);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].formant_shift = channel_formant_shift[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_bandwidth_scale(VoiceParamBlock &shadow, const InputValue &value) {  // CC22, live
    channel_bandwidth_scale[value.channel] = tract_cc_to_q8_8((uint8_t)value.scalar, BANDWIDTH_SCALE_MIN, BANDWIDTH_SCALE_MAX);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].bandwidth_scale = channel_bandwidth_scale[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_utterance(VoiceParamBlock &, const InputValue &value) {  // CC23, next note --
    // utterance/phrase select, indexes SPEECH_PHRASES. Split range into
    // SPEECH_PHRASE_COUNT + 1 bands, same shape as CC20's banding -- band
    // 0 is "off" (SPEECH_NO_UTTERANCE, back to the phoneme keyboard),
    // bands 1..SPEECH_PHRASE_COUNT each pick one SPEECH_PHRASES entry.
    uint8_t band = (uint8_t)((uint32_t)value.scalar * (SPEECH_PHRASE_COUNT + 1) / 128u);
    channel_utterance[value.channel] = band == 0 ? SPEECH_NO_UTTERANCE : (uint8_t)(band - 1);
    ui_state.last_channel = value.channel;
}

static void set_rate(VoiceParamBlock &shadow, const InputValue &value) {  // CC24, live
    channel_rate[value.channel] = speech_rate_cc_to_q4_4((uint8_t)value.scalar);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].rate = channel_rate[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_jitter(VoiceParamBlock &shadow, const InputValue &value) {  // CC25, live
    channel_jitter[value.channel] = (uint8_t)((uint32_t)value.scalar * 255u / 127u);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].jitter = channel_jitter[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_shimmer(VoiceParamBlock &shadow, const InputValue &value) {  // CC26, live
    channel_shimmer[value.channel] = (uint8_t)((uint32_t)value.scalar * 255u / 127u);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].shimmer = channel_shimmer[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_mode(VoiceParamBlock &shadow, const InputValue &value) {  // CC27, live --
    // 3 bands: ONESHOT/GATED/LOOP
    uint8_t band = (uint8_t)((uint32_t)value.scalar * 3u / 128u);
    SpeechMode m = band == 0 ? SPEECH_MODE_ONESHOT : (band == 1 ? SPEECH_MODE_GATED : SPEECH_MODE_LOOP);
    channel_mode[value.channel] = m;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].mode = m;
    }
    ui_state.last_channel = value.channel;
}

static void set_phrase_bank(VoiceParamBlock &, const InputValue &value) {  // CC28, next note --
    // phrase-bank mode toggle: set_note() reads this to decide what its
    // own note-number argument means
    channel_phrase_bank[value.channel] = value.scalar >= 64.0f;
    ui_state.last_channel = value.channel;
}

// FX setters write shadow.fx -- one instance per VoiceParamBlock, not
// per-voice -- a true module-global Modifier.
static void set_fx_p1(VoiceParamBlock &shadow, const InputValue &value) {  // CC72: delay feedback / reverb room size
    shadow.fx.p1 = (uint8_t)value.scalar;
    ui_state.fx_p1 = shadow.fx.p1;
}

static void set_fx_mix(VoiceParamBlock &shadow, const InputValue &value) {  // CC73: effect wet/dry mix
    shadow.fx.mix = (uint8_t)value.scalar;
    ui_state.fx_mix = shadow.fx.mix;
}

static void set_fx_type(VoiceParamBlock &shadow, const InputValue &value) {  // CC74: effect type select
    shadow.fx.type = (uint8_t)((uint32_t)value.scalar * FX_COUNT / 128u);
    ui_state.fx_type = shadow.fx.type;
}

static void set_fx_p2(VoiceParamBlock &shadow, const InputValue &value) {  // CC75: delay time / reverb damping
    shadow.fx.p2 = (uint8_t)value.scalar;
    ui_state.fx_p2 = shadow.fx.p2;
}

static void set_vibrato_rate(VoiceParamBlock &shadow, const InputValue &value) {  // CC76 (GM vibrato rate), live
    channel_lfo_rate[value.channel] = value.scalar / 127.0f * SPEECH_LFO_RATE_HZ_MAX;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].lfo_rate = channel_lfo_rate[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_tract(VoiceParamBlock &, const InputValue &value) {  // CC102, next note --
    // tract select: formant / LPC lattice / SAM, 3 bands. Independent of
    // preset selection (Program Change) -- lets a performer swap tract
    // without picking a different preset index.
    uint8_t band = (uint8_t)((uint32_t)value.scalar * 3u / 128u);
    channel_tract[value.channel] =
        band == 0 ? SPEECH_TRACT_FORMANT : (band == 1 ? SPEECH_TRACT_LATTICE : SPEECH_TRACT_SAM);
    ui_state.last_channel = value.channel;
}

static void set_lattice_pitch_shift(VoiceParamBlock &shadow, const InputValue &value) {  // CC103, live
    channel_lattice_pitch_shift[value.channel] =
        tract_cc_to_q8_8((uint8_t)value.scalar, SPEECH_LATTICE_PITCH_SHIFT_MIN, SPEECH_LATTICE_PITCH_SHIFT_MAX);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].lattice_pitch_shift = channel_lattice_pitch_shift[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_lattice_exciter(VoiceParamBlock &shadow, const InputValue &value) {  // CC104, live --
    // 0-63 glottal_pulse() (shared with the formant tract), 64-127 the
    // TMS5220's own chirp table (lattice.h)
    channel_lattice_chirp_exciter[value.channel] = value.scalar >= 64.0f;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].lattice_chirp_exciter = channel_lattice_chirp_exciter[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_sam_throat(VoiceParamBlock &shadow, const InputValue &value) {  // CC105, live -- tract length / "gender" (sam.h)
    channel_sam_throat[value.channel] = tract_cc_to_q8_8((uint8_t)value.scalar, SAM_THROAT_MIN, SAM_THROAT_MAX);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].sam_throat = channel_sam_throat[value.channel];
    }
    ui_state.last_channel = value.channel;
}

static void set_sam_mouth(VoiceParamBlock &shadow, const InputValue &value) {  // CC106, live -- resonance / brightness (sam.h)
    channel_sam_mouth[value.channel] = tract_cc_to_q8_8((uint8_t)value.scalar, SAM_MOUTH_MIN, SAM_MOUTH_MAX);
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_held[v] && voice_channel[v] == value.channel)
            shadow.voices[v].sam_mouth = channel_sam_mouth[value.channel];
    }
    ui_state.last_channel = value.channel;
}

// Configuration Handler: Program Change loads a preset -- see this file's
// header comment for why a formant/SAM utterance and an LPC lattice page
// are the same operation now, addressed as one contiguous space.
static void set_program(VoiceParamBlock &, const InputValue &value) {
    uint8_t idx = (uint8_t)value.index;
    if (idx < SPEECH_PRESET_COUNT) {
        speech_apply_preset(value.channel, presets[idx]);
    } else {
        uint32_t page = (idx - SPEECH_PRESET_COUNT) % LPC_PAGE_COUNT;
        speech_apply_preset(value.channel, lpc_page_presets.pages[page]);
    }
    ui_state.last_channel = value.channel;
}

static constexpr InputCategory kCapabilities[] = {
    InputCategory::NOTE,
    InputCategory::MODIFIER,
    InputCategory::CONFIGURATION,
};

static constexpr InputMapEntryT<VoiceParamBlock> kMappingTable[] = {
    // category                  id_low  id_high  channel  fixed_vel  setter
    { InputCategory::NOTE,          0,      127,     0xFF,    0,       set_note },
    { InputCategory::MODIFIER,      1,      1,       0xFF,    0,       set_vibrato_depth },       // CC1
    { InputCategory::MODIFIER,      10,     10,      0xFF,    0,       set_pan },                 // CC10
    { InputCategory::MODIFIER,      15,     15,      0xFF,    0,       set_velocity_toggle },     // CC15
    { InputCategory::MODIFIER,      20,     20,      0xFF,    0,       set_phoneme },             // CC20
    { InputCategory::MODIFIER,      21,     21,      0xFF,    0,       set_formant_shift },       // CC21
    { InputCategory::MODIFIER,      22,     22,      0xFF,    0,       set_bandwidth_scale },     // CC22
    { InputCategory::MODIFIER,      23,     23,      0xFF,    0,       set_utterance },           // CC23
    { InputCategory::MODIFIER,      24,     24,      0xFF,    0,       set_rate },                // CC24
    { InputCategory::MODIFIER,      25,     25,      0xFF,    0,       set_jitter },              // CC25
    { InputCategory::MODIFIER,      26,     26,      0xFF,    0,       set_shimmer },             // CC26
    { InputCategory::MODIFIER,      27,     27,      0xFF,    0,       set_mode },                // CC27
    { InputCategory::MODIFIER,      28,     28,      0xFF,    0,       set_phrase_bank },         // CC28
    { InputCategory::MODIFIER,      72,     72,      0xFF,    0,       set_fx_p1 },               // CC72
    { InputCategory::MODIFIER,      73,     73,      0xFF,    0,       set_fx_mix },              // CC73
    { InputCategory::MODIFIER,      74,     74,      0xFF,    0,       set_fx_type },             // CC74
    { InputCategory::MODIFIER,      75,     75,      0xFF,    0,       set_fx_p2 },               // CC75
    { InputCategory::MODIFIER,      76,     76,      0xFF,    0,       set_vibrato_rate },        // CC76
    { InputCategory::MODIFIER,      102,    102,     0xFF,    0,       set_tract },               // CC102
    { InputCategory::MODIFIER,      103,    103,     0xFF,    0,       set_lattice_pitch_shift }, // CC103
    { InputCategory::MODIFIER,      104,    104,     0xFF,    0,       set_lattice_exciter },     // CC104
    { InputCategory::MODIFIER,      105,    105,     0xFF,    0,       set_sam_throat },          // CC105
    { InputCategory::MODIFIER,      106,    106,     0xFF,    0,       set_sam_mouth },           // CC106
    { InputCategory::CONFIGURATION, MIDI_CONFIG_ID_PROGRAM, MIDI_CONFIG_ID_PROGRAM, 0xFF, 0, set_program },
};

static_assert(input_table_declares_capabilities(kMappingTable, kCapabilities),
              "speech mapping table entry uses an InputCategory not in kCapabilities");

void midi_controller_init() {
    midi_parser.init();
    midi_bank_select_init();
    for (int i = 0; i < 128; i++) note_voice[i] = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) voice_held[v] = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
        // PRESET_PHONEME_KEYBOARD's fields (presets.h) match this engine's
        // power-on defaults; loading it here routes those defaults through
        // the same preset machinery Program Change uses at runtime, rather
        // than duplicating the field values.
        speech_apply_preset(ch, presets[PRESET_PHONEME_KEYBOARD]);
        channel_pan[ch] = 0;
        channel_phrase_bank[ch] = false;
        channel_velocity_enabled[ch] = true;
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
    midi_controller_process_generic(data, len, params, kMappingTable);
}
