#pragma once

#include "engine.h"
#include "samples.h"

// Authoring source for the subtractive factory bank: the device never reads
// this file. tools/host_render/gen_subtractive_bank.cpp converts it into
// factory_bank.h (stored-format Patches); edit here and regenerate.
//
// Describes *what* a voice sounds like, not the per-note runtime state.
struct VoicePreset {
    int16_t amplitude;       // unused: the note's velocity sets amplitude
    Waveform waveform;
    uint16_t duty_cycle;     // square wave duty (0–1023, 512 = 50%)
    float lfo_rate;          // LFO Hz (0 = off)
    float lfo_depth;         // LFO → amplitude (0.0–1.0)
    float lfo_pitch_depth;   // LFO → pitch (0.0–1.0)
    float lfo_pwm_depth;     // LFO → duty cycle (0.0–1.0)
    FilterMode filter_mode;
    uint16_t filter_cutoff;    // Hz (20–18000)
    uint16_t filter_resonance; // 0–32767
    int16_t filter_env_amount; // envelope → cutoff Hz (±18000)
    float lfo_filter_depth;    // LFO → cutoff Hz (±18000)
    const SampleDef *sample;   // nullptr for non-sample waveforms
};

// Preset list, in factory-bank order.

enum PresetId : uint8_t {
    PRESET_FAIRLIGHT,   // 0: Fairlight CMI sample, LP filter
    PRESET_SQUARE_PWM,  // 1: Square BLEP with PWM LFO
    PRESET_SAW_FILTER,  // 2: Saw BLEP with filter LFO
    PRESET_MARIMBA,     // 3: marimba sample
    PRESET_LOWSTR5,     // 4: low strings sample
    PRESET_VOICE_AH,    // 5: voice "ah" sample
    PRESET_VOICE_ARR,   // 6: voice "arr" sample
    PRESET_VOICE_RRR,   // 7: voice "rrr" sample
    PRESET_SITAR2,      // 8: sitar sample
    PRESET_ZITHER2,     // 9: zither sample
    PRESET_ELECPIAN,    // 10: electric piano sample
    PRESET_COUNT
};

static const VoicePreset presets[PRESET_COUNT] = {
    // PRESET_FAIRLIGHT: Fairlight sample, LP 400Hz, res 16000, env 8000
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  400, 16000, 8000, 0.0f,    &sararr1_sample },
    // PRESET_SQUARE_PWM: Square BLEP, PWM LFO 3Hz, LP 800Hz, res 20000
    { 10000, WAVE_SQUARE_BLEP, 512, 3.0f, 0.0f, 0.0f, 0.5f, FILTER_LP,  800, 20000, 4000, 0.0f,    nullptr },
    // PRESET_SAW_FILTER: Saw BLEP, filter LFO 2Hz, LP 200Hz, res 24000
    { 10000, WAVE_SAW_BLEP,    512, 2.0f, 0.0f, 0.0f, 0.0f, FILTER_LP,  200, 24000, 0,    2000.0f, nullptr },
    // Sample presets — clean playback (filter off), tune later as desired.
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &marimba_sample },  // PRESET_MARIMBA
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &lowstr5_sample },  // PRESET_LOWSTR5
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &ah2_sample },      // PRESET_VOICE_AH
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &arr_sample },      // PRESET_VOICE_ARR
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &rrr_sample },      // PRESET_VOICE_RRR
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &sitar2_sample },   // PRESET_SITAR2
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &zither2_sample },  // PRESET_ZITHER2
    { 10000, WAVE_SAMPLE,      512, 0.0f, 0.0f, 0.0f, 0.0f, FILTER_OFF, 1000, 0, 0, 0.0f, &elecpian_sample }, // PRESET_ELECPIAN
};

// Patch names, stored in each factory Patch (at most kPatchNameLen chars).
static const char *const preset_names[PRESET_COUNT] = {
    "Fairlite", "Sq-PWM", "Saw-Flt", "Marimba", "LowStr5", "VoiceAh", "VoiceArr", "VoiceRrr",
    "Sitar2", "Zither2", "ElecPian",
};
