#pragma once

#include "engine.h"
#include "phrases.h"
#include "phonemes.h"
#include <cmath>

// Preset table (module_speech.md "Preset table"): a SpeechPreset is what a
// voice sounds like, applied at note-on. Several of the fields a preset
// sets here (formant_shift, bandwidth_scale, jitter, shimmer, mode,
// rate, lfo_rate/lfo_depth) also have their own live per-channel CC
// (midi_controller.cpp CC21/22/24-27/1/76), so selecting a preset writes the
// *channel*-level state those CCs track (midi_controller.cpp's
// speech_load_preset(), via this header's voice_apply_preset()), not just
// the one voice about to sound. That is what makes a preset switch a
// starting point a player can still tweak live afterward, rather than a
// value that would just get immediately overwritten by whatever the
// channel's CC state already was.

// Robot chorus (module_speech.md "Scope"): MAX_VOICES is 8, so a
// preset with `chorus = true` spreads each simultaneously-held voice across
// the stereo field with a small per-voice pitch offset, keyed by the
// allocated voice slot (0..MAX_VOICES-1). Deterministic function of the
// slot index -- no extra per-voice state, no sequencer/voice-alloc changes.
inline constexpr float CHORUS_PAN_RANGE = 0.7f;          // fraction of full L/R width
inline constexpr float CHORUS_DETUNE_SEMITONES = 0.18f;  // +/- half-range across the voice pool

struct SpeechPreset {
    const char *name;
    uint8_t    utterance;        // SPEECH_NO_UTTERANCE = phoneme keyboard (HOLD); else a SPEECH_PHRASES index
    uint8_t    phoneme;          // used only when utterance == SPEECH_NO_UTTERANCE
    SpeechMode mode;             // read only when utterance != SPEECH_NO_UTTERANCE
    uint8_t    rate;             // Q4.4, 16 = 1.0x
    int16_t    formant_shift;    // Q8.8, 256 = 1.0x
    int16_t    bandwidth_scale;  // Q8.8, 256 = 1.0x
    uint8_t    jitter;           // 0-255
    uint8_t    shimmer;          // 0-255
    float      lfo_rate;         // Hz, 0 = off
    float      lfo_depth;        // 0.0-1.0
    bool       chorus;           // per-voice pan spread + detune, see above
    SpeechTract tract;            // formant vs. LPC lattice (tract.h)
    uint8_t    lattice_page;      // KEY_PER_WORD starting page, LPC tract only
    int16_t    lattice_pitch_shift; // Q8.8, 256 = 1.0x, LPC tract only
    bool       lattice_chirp_exciter; // voiced-excitation source, LPC tract only (lattice.h)
};

// Q8.8 fixed-point from a float multiplier, matching tract_cc_to_q8_8()'s
// encoding (tract.h) so preset literals below can be written as musical
// multipliers instead of raw fixed-point integers.
inline constexpr int16_t speech_q8_8(float v) {
    return (int16_t)(v * 256.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

// Perturbs an already-computed phase_inc/pan by `voice_index`'s position in
// the MAX_VOICES pool: pan sweeps -CHORUS_PAN_RANGE..+CHORUS_PAN_RANGE of
// full width, detune sweeps +/-CHORUS_DETUNE_SEMITONES, both deterministic
// functions of the slot index so no extra per-voice state is needed. Shared
// by voice_apply_preset() below (called with voice_index == 0 by callers
// that don't yet know the real allocated slot) and by midi_controller.cpp,
// which calls it again once voice_alloc_allocate() has returned the real
// slot for this note-on.
inline void speech_chorus_apply(VoiceParams &vp, uint32_t voice_index) {
    uint32_t span = MAX_VOICES > 1 ? MAX_VOICES - 1 : 1;
    float t = (float)(voice_index % MAX_VOICES) / (float)span * 2.0f - 1.0f;  // -1..1
    vp.pan = (int16_t)(t * 32767.0f * CHORUS_PAN_RANGE);
    float semis = t * CHORUS_DETUNE_SEMITONES;
    vp.phase_inc = (uint32_t)((float)vp.phase_inc * exp2f(semis * (1.0f / 12.0f)));
}

// Apply a preset to one voice about to sound. `voice_index` is the slot
// voice_alloc_allocate() returned for this note-on -- only used when
// pr.chorus is set. Does not touch trigger/gate/amplitude (caller sets
// those from the note/velocity); phase_inc must already be set from the
// note's pitch before calling this, since a chorus preset perturbs it
// further.
inline void voice_apply_preset(VoiceParams &vp, const SpeechPreset &pr, uint32_t voice_index = 0) {
    vp.utterance = pr.utterance;
    vp.phoneme = pr.phoneme;
    vp.mode = pr.mode;
    vp.rate = pr.rate;
    vp.formant_shift = pr.formant_shift;
    vp.bandwidth_scale = pr.bandwidth_scale;
    vp.jitter = pr.jitter;
    vp.shimmer = pr.shimmer;
    vp.lfo_rate = pr.lfo_rate;
    vp.lfo_depth = pr.lfo_depth;
    vp.tract = pr.tract;
    vp.lattice_pitch_shift = pr.lattice_pitch_shift;
    vp.lattice_chirp_exciter = pr.lattice_chirp_exciter;

    if (pr.chorus) speech_chorus_apply(vp, voice_index);
    else            vp.pan = 0;
}

// Master preset list -- single source of truth. Covers every SpeechMode
// (module_speech.md "Data Structures"): SPEECH_HOLD is represented
// structurally (utterance == SPEECH_NO_UTTERANCE, see engine.h), the other
// three explicitly -- plus the robotic/breathy/tract-shift-both-directions
// range this table covers, the robot chorus preset, and one LPC lattice
// preset (module_speech.md "Scope"). Every preset row sets `tract`
// explicitly, even the formant ones (SPEECH_TRACT_FORMANT) -- there's no
// implicit default a reader could miss.
enum SpeechPresetId : uint8_t {
    PRESET_PHONEME_KEYBOARD,  // 0: HOLD -- one note, one sustained phoneme
    PRESET_ONESHOT_ANNOUNCE,  // 1: SPEECH_MODE_ONESHOT -- ignores note-off
    PRESET_GATED_PHRASE,      // 2: SPEECH_MODE_GATED -- the default mode, note-off-aware
    PRESET_LOOP_CHANT,        // 3: SPEECH_MODE_LOOP -- repeats while held
    PRESET_ROBOTIC,           // 4: zero jitter/shimmer, low bandwidth -- robotic, mechanical timbre
    PRESET_BREATHY,           // 5: high bandwidth, jitter/shimmer + light vibrato -- closer to human
    PRESET_TRACT_SHIFT_UP,    // 6: short tract, formant_shift near max
    PRESET_TRACT_SHIFT_DOWN,  // 7: long tract, formant_shift near min
    PRESET_ROBOT_CHORUS,      // 8: robotic + per-voice pan/detune spread across MAX_VOICES
    PRESET_LPC_WORDS,         // 9: SPEECH_TRACT_LATTICE -- KEY_PER_WORD corpus playback, page 0
    SPEECH_PRESET_COUNT
};

static constexpr SpeechPreset presets[SPEECH_PRESET_COUNT] = {
    // name                utterance             phoneme  mode                rate  fmt_shift          bw_scale                    jit  shim lfo_rate lfo_depth chorus  tract                  page  pitch_shift        chirp_exciter
    // PH_I / formant_shift 1.0x / bandwidth_scale 1.0x matches
    // midi_controller.cpp's original power-on default exactly, so this
    // preset being channel 0's initial load is not a behaviour change.
    { "PHON KEYS",   SPEECH_NO_UTTERANCE, PH_I,   SPEECH_MODE_GATED,   16, speech_q8_8(1.0f), speech_q8_8(1.0f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "ANNOUNCE",    PHRASE_VOICE_TEST,   PH_SIL, SPEECH_MODE_ONESHOT, 16, speech_q8_8(1.0f), speech_q8_8(1.0f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "GATED SAY",   PHRASE_NOW_PLAYING,  PH_SIL, SPEECH_MODE_GATED,   16, speech_q8_8(1.0f), speech_q8_8(1.0f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "LOOP CHANT",  PHRASE_HELLO_WORLD,  PH_SIL, SPEECH_MODE_LOOP,    20, speech_q8_8(1.0f), speech_q8_8(1.0f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "ROBOTIC",     PHRASE_GOOD_MORNING, PH_SIL, SPEECH_MODE_GATED,   16, speech_q8_8(1.0f), speech_q8_8(0.5f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "BREATHY",     PHRASE_TRACK_SAVED,  PH_SIL, SPEECH_MODE_GATED,   16, speech_q8_8(1.0f), speech_q8_8(2.4f),          40,  30,  5.0f, 0.15f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "TRACT UP",    SPEECH_NO_UTTERANCE, PH_I,   SPEECH_MODE_GATED,   16, speech_q8_8(1.55f), speech_q8_8(1.0f),          0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "TRACT DOWN",  SPEECH_NO_UTTERANCE, PH_U,   SPEECH_MODE_GATED,   16, speech_q8_8(0.75f), speech_q8_8(1.0f),          0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    { "BOT CHORUS",  PHRASE_DRUM_MACHINE, PH_SIL, SPEECH_MODE_ONESHOT, 16, speech_q8_8(1.0f), speech_q8_8(0.5f),           0,   0,  0.0f, 0.0f, true,  SPEECH_TRACT_FORMANT, 0, speech_q8_8(1.0f), false },
    // LPC lattice tract: utterance/phoneme/formant_shift/bandwidth_scale/
    // jitter/shimmer/lfo are formant-only fields, unread by the lattice
    // render path -- left at the same neutral values PHON KEYS uses so a
    // channel that switches back to a formant preset afterward doesn't
    // inherit anything unusual. GATED (the engine's own default mode) and
    // page 0, the same starting page a channel already has at power-on.
    // chirp_exciter false -- CC104 (midi_controller.cpp) switches it live,
    // this preset doesn't pick a side.
    { "LPC WORDS",   SPEECH_NO_UTTERANCE, PH_I,   SPEECH_MODE_GATED,   16, speech_q8_8(1.0f), speech_q8_8(1.0f),           0,   0,  0.0f, 0.0f, false, SPEECH_TRACT_LATTICE, 0, speech_q8_8(1.0f), false },
};
