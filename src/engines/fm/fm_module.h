#pragma once

#include "dx7_tables.h"
#include "patch.h"
#include "patch/descriptor.h"
#include "patch/module_id.h"
#include "patch/multi_cooked.h"
#include "patch/part_slot.h"
#include <cstdint>

// The FM module on the patch platform (docs/platform.md): its descriptor
// table -- a DX7 voice, in DX7 units -- its cooked patch, and the cook step
// between them.

// One Part per MIDI channel by default: Part N plays on channel N.
static constexpr uint32_t FM_PART_COUNT = 16;

// Parameter IDs (permanent; append only). Each operator has a block of
// FM_OP_FIELD_COUNT IDs, OP1's first; the voice-wide parameters follow.
enum FmOpField : uint16_t {
    FM_OP_R1, FM_OP_R2, FM_OP_R3, FM_OP_R4,
    FM_OP_L1, FM_OP_L2, FM_OP_L3, FM_OP_L4,
    FM_OP_BREAK_POINT,
    FM_OP_LEFT_DEPTH,
    FM_OP_RIGHT_DEPTH,
    FM_OP_LEFT_CURVE,
    FM_OP_RIGHT_CURVE,
    FM_OP_RATE_SCALING,
    FM_OP_AMS,
    FM_OP_VELOCITY,
    FM_OP_LEVEL,
    FM_OP_MODE,
    FM_OP_COARSE,
    FM_OP_FINE,
    FM_OP_DETUNE,
    FM_OP_ENABLE,
    FM_OP_FIELD_COUNT
};

// The ID of field `f` of operator `op` (0 = OP1).
constexpr uint16_t fm_op_param(uint32_t op, FmOpField f) { return (uint16_t)(op * FM_OP_FIELD_COUNT + f); }

enum FmParamId : uint16_t {
    FM_P_PEG_R1 = FM_NUM_OPS * FM_OP_FIELD_COUNT, FM_P_PEG_R2, FM_P_PEG_R3, FM_P_PEG_R4,
    FM_P_PEG_L1, FM_P_PEG_L2, FM_P_PEG_L3, FM_P_PEG_L4,
    FM_P_ALGORITHM,
    FM_P_FEEDBACK,
    FM_P_OSC_SYNC,
    FM_P_LFO_SPEED,
    FM_P_LFO_DELAY,
    FM_P_LFO_PMD,
    FM_P_LFO_AMD,
    FM_P_LFO_SYNC,
    FM_P_LFO_WAVE,
    FM_P_LFO_PMS,
    FM_P_TRANSPOSE,
    FM_P_COUNT
};

// Value labels for the ENUM parameters, in value order.
inline constexpr const char *kFmOffOnLabels[] = { "OFF", "ON" };
inline constexpr const char *kFmCurveLabels[] = { "-LIN", "-EXP", "+EXP", "+LIN" };
inline constexpr const char *kFmModeLabels[] = { "RATIO", "FIXED" };
inline constexpr const char *kFmLfoWaveLabels[] = { "TRI", "SAW DN", "SAW UP", "SQUARE", "SINE", "S/HOLD" };
inline constexpr const char *kFmAlgorithmLabels[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32",
};
inline constexpr const char *kFmDetuneLabels[] = {
    "-7", "-6", "-5", "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4", "+5", "+6", "+7",
};

// The engine-native form of one Patch: the patch data the operator kernels
// read, with its routing already resolved. Every field but the live ones
// (FmModule::copy_live()) is fixed for a note when it starts.
struct FmCookedPatch {
    FmPatch patch;      // name is null: the Patch's name lives in its Edit buffer
    FmRouting routing;
};

// Cooked-patch slots, one per Part, shared between Core 0 (publishes) and
// Core 1 (consumes once per buffer). Defined by the Input subsystem.
extern PartSlotT<FmCookedPatch> fm_part_slots[FM_PART_COUNT];

// The cooked Multi (mix and FX) and its slot, likewise.
using FmMultiCooked = MultiCookedT<FM_PART_COUNT>;
extern PartSlotT<FmMultiCooked> fm_multi_slot;

// One operator's descriptors. EG rates and levels and FINE span the 7-bit
// byte range rather than the DX7 panel's 0-99: factory voices carry such
// values and the engine plays them as Dexed does.
#define FM_OP_DESCRIPTORS(N, LEVEL_DEFAULT)                                                                          \
    { fm_op_param(N - 1, FM_OP_R1), "R1", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_R2), "R2", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_R3), "R3", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_R4), "R4", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_L1), "L1", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_L2), "L2", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_L3), "L3", "OP" #N, 0, 127, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_L4), "L4", "OP" #N, 0, 127, 0,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_BREAK_POINT), "BREAK PT", "OP" #N, 0, 99, 39, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_LEFT_DEPTH), "L DEPTH", "OP" #N, 0, 99, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_RIGHT_DEPTH), "R DEPTH", "OP" #N, 0, 99, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_LEFT_CURVE), "L CURVE", "OP" #N, 0, 3, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kFmCurveLabels, 4 }, \
    { fm_op_param(N - 1, FM_OP_RIGHT_CURVE), "R CURVE", "OP" #N, 0, 3, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kFmCurveLabels, 4 }, \
    { fm_op_param(N - 1, FM_OP_RATE_SCALING), "RATE SCL", "OP" #N, 0, 7, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_AMS), "AMS", "OP" #N, 0, 3, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_VELOCITY), "VELOCITY", "OP" #N, 0, 7, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_LEVEL), "LEVEL", "OP" #N, 0, 99, LEVEL_DEFAULT, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_MODE), "MODE", "OP" #N, 0, 1, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kFmModeLabels, 2 }, \
    { fm_op_param(N - 1, FM_OP_COARSE), "COARSE", "OP" #N, 0, 31, 1, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_FINE), "FINE", "OP" #N, 0, 127, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { fm_op_param(N - 1, FM_OP_DETUNE), "DETUNE", "OP" #N, 0, 14, 7, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kFmDetuneLabels, 15 }, \
    { fm_op_param(N - 1, FM_OP_ENABLE), "ENABLE", "OP" #N, 0, 1, 1, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kFmOffOnLabels, 2 }

struct FmModule {
    static constexpr ModuleId kModuleId = ModuleId::FM;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = FM_PART_COUNT;
    using Cooked = FmCookedPatch;

    // The default Multi plays factory bank 0, slot 0 on every Part.
    static constexpr uint8_t default_program(uint32_t) { return 0; }

    // Defaults are the DX7's INIT VOICE: algorithm 1, OP1 alone at full
    // level, every operator at ratio 1.
    static constexpr ParamDescriptor kDescriptors[FM_P_COUNT] = {
        FM_OP_DESCRIPTORS(1, 99),
        FM_OP_DESCRIPTORS(2, 0),
        FM_OP_DESCRIPTORS(3, 0),
        FM_OP_DESCRIPTORS(4, 0),
        FM_OP_DESCRIPTORS(5, 0),
        FM_OP_DESCRIPTORS(6, 0),
        // id               name         group       min max def unit             curve               class                quick  cc
        { FM_P_PEG_R1,      "R1",        "PITCH EG", 0, 99, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_R2,      "R2",        "PITCH EG", 0, 99, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_R3,      "R3",        "PITCH EG", 0, 99, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_R4,      "R4",        "PITCH EG", 0, 99, 99, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_L1,      "L1",        "PITCH EG", 0, 99, 50, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_L2,      "L2",        "PITCH EG", 0, 99, 50, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_L3,      "L3",        "PITCH EG", 0, 99, 50, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_PEG_L4,      "L4",        "PITCH EG", 0, 99, 50, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_ALGORITHM,   "ALGORITHM", "VOICE",    0, 31, 0,  ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, true,  kNoDefaultCc,
          kFmAlgorithmLabels, 32 },
        { FM_P_FEEDBACK,    "FEEDBACK",  "VOICE",    0, 7,  0,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, true,  kNoDefaultCc },
        { FM_P_OSC_SYNC,    "OSC SYNC",  "VOICE",    0, 1,  1,  ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc,
          kFmOffOnLabels, 2 },
        { FM_P_LFO_SPEED,   "SPEED",     "LFO",      0, 99, 35, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    true,  kNoDefaultCc },
        { FM_P_LFO_DELAY,   "DELAY",     "LFO",      0, 99, 0,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    true,  kNoDefaultCc },
        { FM_P_LFO_PMD,     "PMD",       "LFO",      0, 99, 0,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    true,  kNoDefaultCc },
        { FM_P_LFO_AMD,     "AMD",       "LFO",      0, 99, 0,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    true,  kNoDefaultCc },
        { FM_P_LFO_SYNC,    "KEY SYNC",  "LFO",      0, 1,  1,  ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc,
          kFmOffOnLabels, 2 },
        { FM_P_LFO_WAVE,    "WAVE",      "LFO",      0, 5,  0,  ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc,
          kFmLfoWaveLabels, 6 },
        { FM_P_LFO_PMS,     "PMS",       "LFO",      0, 7,  3,  ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc },
        { FM_P_TRANSPOSE,   "TRANSPOSE", "VOICE",    0, 48, 24, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc },
    };

    // DX7 voice values -> the engine's patch form, as tools/syx2patch.py
    // converts a .syx voice, plus operator enables. OSC SYNC and TRANSPOSE
    // are stored but not yet applied: the engine always restarts operator
    // phase at note-on and plays the note as received.
    static void cook(const int32_t *v, FmCookedPatch &c) {
        const Dx7Algorithm &alg = kDx7Algorithms[v[FM_P_ALGORITHM]];
        FmPatch &p = c.patch;
        p.name = nullptr;
        uint8_t enabled = 0;
        for (uint32_t i = 0; i < FM_NUM_OPS; i++) {
            const int32_t *o = v + fm_op_param(i, FM_OP_R1);
            FmOpParams &op = p.op[i];
            const bool fixed = o[FM_OP_MODE] != 0;
            const int32_t coarse = o[FM_OP_COARSE];
            const int32_t fine = o[FM_OP_FINE];
            // Double, as the converter computes it, so the float is the same.
            op.ratio = fixed ? 1.0f : (float)((coarse == 0 ? 0.5 : (double)coarse) * (1.0 + 0.01 * fine));
            op.fixed_hz = fixed ? kDx7FixedHz[(coarse & 3) * 100 + fine] : 0.0f;
            op.fixed_freq = fixed;
            op.detune_offset = (int8_t)(o[FM_OP_DETUNE] - 7);
            op.mod_target = alg.op[i].mod_target;
            op.extra_target_mask = alg.op[i].extra_target_mask;
            op.feedback_level = alg.op[i].feedback ? (uint8_t)v[FM_P_FEEDBACK] : 0;
            op.output_level = (uint8_t)o[FM_OP_LEVEL];
            op.vel_sensitivity = (uint8_t)o[FM_OP_VELOCITY];
            for (uint32_t s = 0; s < 4; s++) {
                op.eg_rate[s] = (uint8_t)o[FM_OP_R1 + s];
                op.eg_level[s] = (uint8_t)o[FM_OP_L1 + s];
            }
            op.scale_breakpoint = (uint8_t)o[FM_OP_BREAK_POINT];
            op.scale_left_depth = (uint8_t)o[FM_OP_LEFT_DEPTH];
            op.scale_right_depth = (uint8_t)o[FM_OP_RIGHT_DEPTH];
            op.scale_left_curve = (uint8_t)o[FM_OP_LEFT_CURVE];
            op.scale_right_curve = (uint8_t)o[FM_OP_RIGHT_CURVE];
            op.rate_scaling = (uint8_t)o[FM_OP_RATE_SCALING];
            op.am_sensitivity = (uint8_t)o[FM_OP_AMS];
            if (o[FM_OP_ENABLE]) enabled |= (uint8_t)(1u << i);
        }
        p.lfo = FmLfoParams{ (uint8_t)v[FM_P_LFO_SPEED], (uint8_t)v[FM_P_LFO_DELAY], (uint8_t)v[FM_P_LFO_PMD],
                             (uint8_t)v[FM_P_LFO_AMD], (uint8_t)v[FM_P_LFO_WAVE], v[FM_P_LFO_SYNC] != 0,
                             (uint8_t)v[FM_P_LFO_PMS] };
        for (uint32_t s = 0; s < 4; s++) {
            p.pitch_eg.rate[s] = (uint8_t)v[FM_P_PEG_R1 + s];
            p.pitch_eg.level[s] = (uint8_t)v[FM_P_PEG_L1 + s];
        }
        c.routing = FmRouting{};
        fm_resolve_routing(p, c.routing, enabled);
    }

    // Copy the live-class fields of `from` into `to` (a sounding note's
    // copy of its Patch): the LFO, the pitch EG and each operator's AMS.
    static void copy_live(const FmCookedPatch &from, FmCookedPatch &to) {
        to.patch.lfo = from.patch.lfo;
        to.patch.pitch_eg = from.patch.pitch_eg;
        for (uint32_t i = 0; i < FM_NUM_OPS; i++) to.patch.op[i].am_sensitivity = from.patch.op[i].am_sensitivity;
    }
};

#undef FM_OP_DESCRIPTORS
