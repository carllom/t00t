#pragma once

#include "engine.h"
#include "filter.h"
#include "osc/common.h"
#include "osc/sample.h"
#include "samples.h"
#include "patch/descriptor.h"
#include "patch/module_id.h"
#include "patch/part_slot.h"
#include <cmath>
#include <cstdint>

// The subtractive module on the patch platform (docs/platform.md): its
// descriptor table, its cooked patch, and the cook step between them.

// One Part per MIDI channel: Part N plays on channel N.
static constexpr uint32_t SUB_PART_COUNT = 16;

// Parameter IDs (permanent; append only).
enum SubParamId : uint16_t {
    SUB_P_WAVEFORM,
    SUB_P_SAMPLE,
    SUB_P_DUTY,
    SUB_P_LFO_RATE,
    SUB_P_LFO_AMP,
    SUB_P_LFO_PITCH,
    SUB_P_LFO_PWM,
    SUB_P_LFO_CUTOFF,
    SUB_P_FILTER_MODE,
    SUB_P_CUTOFF,
    SUB_P_RESONANCE,
    SUB_P_ENV_CUTOFF,
    SUB_P_COUNT
};

// Samples selectable by SUB_P_SAMPLE, by stored index (append only).
inline const SampleDef *const kSubSamples[] = {
    &sararr1_sample, &marimba_sample, &lowstr5_sample, &ah2_sample, &arr_sample,
    &rrr_sample, &sitar2_sample, &zither2_sample, &elecpian_sample,
};
static constexpr uint32_t SUB_SAMPLE_COUNT = sizeof(kSubSamples) / sizeof(kSubSamples[0]);

// Engine-native constants for one Patch: everything the render loop derives
// from the Patch rather than from the note, computed once on Core 0.
struct SubCookedPatch {
    const SampleDef *sample;     // nullptr unless waveform == WAVE_SAMPLE
    float lfo_phase_inc;         // preset LFO phase advance per sample (0 when has_lfo is false)
    float f_half_seed;           // SVF F_half at the base cutoff, seeds a new note (0 when filter off)
    int32_t filter_q;            // SVF damping, Q15 (0 when filter off)
    Waveform waveform;
    FilterMode filter_mode;
    bool has_lfo;
    uint16_t duty_cycle;         // square duty, 0..1023
    uint16_t filter_cutoff;      // base cutoff, Hz
    int16_t filter_env_amount;   // envelope -> cutoff, Hz
    int16_t lfo_depth_q15;       // LFO -> amplitude
    int16_t lfo_pitch_q15;       // LFO -> pitch
    int16_t lfo_pwm_q15;         // LFO -> duty, in duty units
    int16_t lfo_filt_hz;         // LFO -> cutoff, Hz
};

// Cooked-patch slots, one per Part, shared between Core 0 (publishes) and
// Core 1 (consumes once per buffer). Defined by the Input subsystem.
extern PartSlotT<SubCookedPatch> sub_part_slots[SUB_PART_COUNT];

struct SubtractiveModule {
    static constexpr ModuleId kModuleId = ModuleId::SUBTRACTIVE;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = SUB_PART_COUNT;
    using Cooked = SubCookedPatch;

    static constexpr ParamDescriptor kDescriptors[SUB_P_COUNT] = {
        // id                name       group     min      max    default unit                 curve                    class                    quick  cc
        { SUB_P_WAVEFORM,    "WAVE",    "OSC",    0,       WAVE_SAMPLE, WAVE_SAW_BLEP, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, true,  kNoDefaultCc },
        { SUB_P_SAMPLE,      "SAMPLE",  "OSC",    0,       SUB_SAMPLE_COUNT - 1, 0, ParamUnit::ENUM,  ParamCurve::LINEAR,      ParamClass::NOTE_ON, false, kNoDefaultCc },
        { SUB_P_DUTY,        "DUTY",    "OSC",    0,       1023,  512,    ParamUnit::NONE,     ParamCurve::LINEAR,      ParamClass::LIVE,    false, kNoDefaultCc },
        { SUB_P_LFO_RATE,    "LFO RATE", "LFO",   0,       2000,  0,      ParamUnit::CENTI_HZ, ParamCurve::EXPONENTIAL, ParamClass::LIVE,    true,  kNoDefaultCc },
        { SUB_P_LFO_AMP,     "LFO>AMP", "LFO",    0,       1000,  0,      ParamUnit::PERMILLE, ParamCurve::LINEAR,      ParamClass::LIVE,    false, kNoDefaultCc },
        { SUB_P_LFO_PITCH,   "LFO>PTCH", "LFO",   0,       1000,  0,      ParamUnit::PERMILLE, ParamCurve::LINEAR,      ParamClass::LIVE,    false, kNoDefaultCc },
        { SUB_P_LFO_PWM,     "LFO>PWM", "LFO",    0,       1000,  0,      ParamUnit::PERMILLE, ParamCurve::LINEAR,      ParamClass::LIVE,    false, kNoDefaultCc },
        { SUB_P_LFO_CUTOFF,  "LFO>CUT", "LFO",   -18000,   18000, 0,      ParamUnit::HZ,       ParamCurve::LINEAR,      ParamClass::LIVE,    false, kNoDefaultCc },
        { SUB_P_FILTER_MODE, "FILTER",  "FILTER", 0,       FILTER_NOTCH, FILTER_OFF, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc },
        { SUB_P_CUTOFF,      "CUTOFF",  "FILTER", 20,      18000, 8000,   ParamUnit::HZ,       ParamCurve::EXPONENTIAL, ParamClass::LIVE,    true,  kNoDefaultCc },
        { SUB_P_RESONANCE,   "RESO",    "FILTER", 0,       32767, 0,      ParamUnit::NONE,     ParamCurve::LINEAR,      ParamClass::LIVE,    true,  kNoDefaultCc },
        { SUB_P_ENV_CUTOFF,  "ENV>CUT", "FILTER", -18000,  18000, 0,      ParamUnit::HZ,       ParamCurve::LINEAR,      ParamClass::LIVE,    true,  kNoDefaultCc },
    };

    static void cook(const int32_t *v, SubCookedPatch &c) {
        c.waveform = (Waveform)v[SUB_P_WAVEFORM];
        c.sample = (c.waveform == WAVE_SAMPLE) ? kSubSamples[v[SUB_P_SAMPLE]] : nullptr;
        c.duty_cycle = (uint16_t)v[SUB_P_DUTY];

        const float lfo_rate = (float)v[SUB_P_LFO_RATE] / 100.0f;
        c.has_lfo = (lfo_rate != 0.0f);
        c.lfo_phase_inc = c.has_lfo ? lfo_rate / (float)SAMPLE_RATE : 0.0f;
        c.lfo_depth_q15 = (int16_t)(((float)v[SUB_P_LFO_AMP] / 1000.0f) * 32767.0f);
        c.lfo_pitch_q15 = (int16_t)(((float)v[SUB_P_LFO_PITCH] / 1000.0f) * 32767.0f);
        c.lfo_pwm_q15 = (int16_t)(((float)v[SUB_P_LFO_PWM] / 1000.0f) * 512.0f);
        c.lfo_filt_hz = (int16_t)v[SUB_P_LFO_CUTOFF];

        c.filter_mode = (FilterMode)v[SUB_P_FILTER_MODE];
        c.filter_cutoff = (uint16_t)v[SUB_P_CUTOFF];
        c.filter_env_amount = (int16_t)v[SUB_P_ENV_CUTOFF];
        const bool has_filter = (c.filter_mode != FILTER_OFF);
        c.filter_q = has_filter ? svf_compute_q((uint16_t)v[SUB_P_RESONANCE]) : 0;
        c.f_half_seed = has_filter ? (float)svf_compute_f_half((int32_t)c.filter_cutoff) : 0.0f;
    }
};

// Base (unbent) phase increment for `note` played with cooked patch `c`.
inline uint32_t sub_note_phase_inc(const SubCookedPatch &c, uint8_t note) {
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    return (c.waveform == WAVE_SAMPLE && c.sample)
        ? osc_sample_phase_inc(c.sample, freq)
        : osc_phase_inc(freq);
}

// Fill `vp` for a new note on `part`: the per-note inputs Core 1 renders
// alongside the Part's cooked patch. `patch_tag` and `base_inc` come from
// the same cooked patch (PatchPlatformT::cooked_generation()/cooked()), so
// Core 1 can start the note on exactly that patch.
inline void sub_note_start(VoiceParams &vp, uint8_t part, uint8_t patch_tag, uint32_t base_inc,
                           float bend_ratio, uint8_t velocity, int16_t mod_depth, int16_t pan) {
    vp.part = part;
    vp.patch_tag = patch_tag;
    vp.phase_inc = (uint32_t)((float)base_inc * bend_ratio);
    vp.amplitude = (int16_t)(velocity * 258);
    vp.mod_depth = mod_depth;
    vp.pan = pan;
    vp.trigger++;
    vp.gate = true;
}

// Name of the Patch currently in `part`'s Edit buffer (Core 0 / UI).
const char *sub_part_patch_name(uint8_t part);
