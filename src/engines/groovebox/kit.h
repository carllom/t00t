#pragma once

#include "engine.h"
#include "osc/common.h"   // osc_phase_inc
#include "envelope.h"     // env_config

// Groovebox voice configuration: the drum kit table and the TB-303 preset,
// plus the apply-to-VoiceParams helpers. Analogous to the subtractive engine's
// presets.h. Values are pre-baked into EnvConfig by Core 0 at note-on so Core 1
// pays no per-render conversion cost.

// --- Fixed voice-slot assignment ------------------------------------------
// A drum machine has a fixed instrument set, so voices map statically to
// instruments instead of being dynamically allocated.
enum GrooveVoice : uint8_t {
    GV_303   = 0,   // mono acid bass
    GV_BD    = 2,
    GV_SNARE = 3,
    GV_TOM_LO = 4,
    GV_TOM_MID = 5,
    GV_TOM_HI = 6,
    GV_HAT_CLOSED = 9,   // chokes GV_HAT_OPEN (shared 808 hi-hat circuit)
    GV_HAT_OPEN   = 10,
};

// --- Drum kit instrument --------------------------------------------------
struct KitInstrument {
    uint8_t   note;          // MIDI note (on the drum channel) that triggers it
    uint8_t   voice;         // fixed voice slot (GrooveVoice)
    VoiceType type;
    float     freq;          // primary osc freq (tonal drums / snare tone 1)
    float     freq2;         // snare tone 2 (0 if unused)
    uint16_t  amp_decay_ms;  // amplitude one-shot decay
    uint16_t  pitch_decay_ms;// pitch-envelope decay (BD/tom/snare)
    int16_t   pitch_env_depth;   // Q15 fraction of base pitch swept downward (0 = none)
    FilterMode filter_mode;  // HP (hat) / BP (snare) / OFF
    uint16_t  filter_cutoff;
    uint16_t  filter_resonance;
    uint16_t  noise_level;   // Q15 noise mix (snare/hat)
    uint16_t  tone_level;    // Q15 tone mix (snare)
};

// 808-flavoured kit. GM-ish note mapping on the drum channel.
static const KitInstrument kit_808[] = {
    // note voice        type          freq  freq2  aDec pDec  pDepth   filter      cut   res    noise  tone
    {  36, GV_BD,        VT_DRUM_BD,    55.0f, 0.0f,  400,  55,  22937, FILTER_OFF,    0,     0,      0,      0 }, // Bass drum
    {  38, GV_SNARE,     VT_DRUM_SNARE,180.0f,330.0f,180,  40,   6553, FILTER_BP,  2000, 12000,  26214,  16384 }, // Snare
    {  41, GV_TOM_LO,    VT_DRUM_TOM,   90.0f, 0.0f,  320,  60,  16384, FILTER_OFF,    0,     0,      0,      0 }, // Low tom
    {  45, GV_TOM_MID,   VT_DRUM_TOM,  130.0f, 0.0f,  300,  60,  16384, FILTER_OFF,    0,     0,      0,      0 }, // Mid tom
    {  48, GV_TOM_HI,    VT_DRUM_TOM,  180.0f, 0.0f,  280,  55,  16384, FILTER_OFF,    0,     0,      0,      0 }, // Hi tom
    {  42, GV_HAT_CLOSED,VT_DRUM_HAT,    0.0f, 0.0f,   45,   0,      0, FILTER_HP,  8000,  6000,  32767,      0 }, // Closed hat
    {  46, GV_HAT_OPEN,  VT_DRUM_HAT,    0.0f, 0.0f,  350,   0,      0, FILTER_HP,  8000,  6000,  32767,      0 }, // Open hat
};
static constexpr uint32_t KIT_808_COUNT = sizeof(kit_808) / sizeof(kit_808[0]);

// Look up a kit instrument by MIDI note. Returns nullptr if the note is unmapped.
inline const KitInstrument *kit_find(const KitInstrument *kit, uint32_t count, uint8_t note) {
    for (uint32_t i = 0; i < count; i++) {
        if (kit[i].note == note) return &kit[i];
    }
    return nullptr;
}

// Apply a kit instrument to a voice (does not touch trigger/gate — the
// controller bumps those).
inline void apply_kit(VoiceParams &vp, const KitInstrument &k, uint8_t velocity) {
    vp.type = k.type;
    vp.amplitude = (int16_t)(velocity * 258);      // 0..127 -> ~0..32766
    vp.phase_inc  = k.freq  > 0.0f ? osc_phase_inc(k.freq)  : 0;
    vp.phase_inc2 = k.freq2 > 0.0f ? osc_phase_inc(k.freq2) : 0;
    vp.waveform = WAVE_SINE;
    vp.duty_cycle = 512;
    vp.amp_env = env_config(0, 0, 0, k.amp_decay_ms);   // one-shot: use release_coeff
    vp.aux_env = env_config(0, 0, 0, k.pitch_decay_ms);
    vp.filter_mode = k.filter_mode;
    vp.filter_cutoff = k.filter_cutoff;
    vp.filter_resonance = k.filter_resonance;
    vp.filter_env_amount = 0;
    vp.pitch_env_depth = k.pitch_env_depth;
    vp.noise_level = k.noise_level;
    vp.tone_level = k.tone_level;
}

// --- TB-303 preset --------------------------------------------------------
struct Tb303Preset {
    Waveform waveform;      // WAVE_SAW or WAVE_SQUARE
    uint16_t cutoff;        // base cutoff Hz
    uint16_t resonance;     // 0..32767 (high = squelchy)
    int16_t  env_mod;       // filter env amount Hz (positive = attack sweep up, decays down)
    uint16_t env_decay_ms;  // filter env decay
    uint16_t amp_attack_ms;
    uint16_t amp_decay_ms;
    uint8_t  amp_sustain_pct;
    uint16_t amp_release_ms;
};

// Default acid bass. Uses the existing 2-pole SVF LP for now (P0); the 4-pole
// ladder filter lands in a later phase for the authentic squelch.
static const Tb303Preset tb303_default = {
    WAVE_SAW, 500, 28000, 5000, 300,
    3, 200, 80, 120
};

inline void apply_303(VoiceParams &vp, const Tb303Preset &p, uint32_t phase_inc, uint8_t velocity) {
    vp.type = VT_TB303;
    vp.amplitude = (int16_t)(velocity * 258);
    vp.phase_inc = phase_inc;
    vp.phase_inc2 = 0;
    vp.waveform = p.waveform;
    vp.duty_cycle = 512;
    vp.amp_env = env_config(p.amp_attack_ms, p.amp_decay_ms, p.amp_sustain_pct, p.amp_release_ms);
    vp.aux_env = env_config(0, 0, 0, p.env_decay_ms);   // one-shot filter env
    vp.filter_mode = FILTER_LP;
    vp.filter_cutoff = p.cutoff;
    vp.filter_resonance = p.resonance;
    vp.filter_env_amount = p.env_mod;
    vp.pitch_env_depth = 0;
    vp.noise_level = 0;
    vp.tone_level = 0;
}
