#pragma once

#include "patch.h"
#include "patch/descriptor.h"
#include "patch/module_id.h"
#include "patch/multi_cooked.h"
#include "patch/part_slot.h"
#include <cstdint>

// The OPL module on the patch platform (docs/platform.md): its descriptor
// table -- an OPL3 voice in register units -- its cooked patch, and the
// cook step between them.

// One Part per MIDI channel by default: Part N plays on channel N.
static constexpr uint32_t OPL_PART_COUNT = 16;

// Operators in a Patch: a 2-op Algorithm leaves OP3 and OP4 unused.
static constexpr uint32_t OPL_NUM_OPS = 4;

// Parameter IDs (permanent; append only). Each operator has a block of
// OPL_OP_FIELD_COUNT IDs, OP1's first; the voice-wide parameters follow.
enum OplOpField : uint16_t {
    OPL_OP_MULT,
    OPL_OP_KSL,
    OPL_OP_TL,
    OPL_OP_AR,
    OPL_OP_DR,
    OPL_OP_SL,
    OPL_OP_RR,
    OPL_OP_EGT,
    OPL_OP_KSR,
    OPL_OP_WAVE,
    OPL_OP_FIELD_COUNT
};

// The ID of field `f` of operator `op` (0 = OP1).
constexpr uint16_t opl_op_param(uint32_t op, OplOpField f) { return (uint16_t)(op * OPL_OP_FIELD_COUNT + f); }

enum OplParamId : uint16_t {
    OPL_P_ALGORITHM = OPL_NUM_OPS * OPL_OP_FIELD_COUNT,
    OPL_P_FEEDBACK,
    OPL_P_COUNT
};

// Value labels for the ENUM parameters, in value order.
inline constexpr const char *kOplOffOnLabels[] = { "OFF", "ON" };
inline constexpr const char *kOplEgtLabels[] = { "PERC", "SUSTAIN" };
// The KSL register's attenuation per octave; its codes are not in slope order.
inline constexpr const char *kOplKslLabels[] = { "OFF", "3dB", "1.5dB", "6dB" };
// The multiplier register's ratio (opl_mult_table): codes 11, 13 and 15 repeat 10, 12 and 14.
inline constexpr const char *kOplMultLabels[] = {
    "0.5", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "10", "12", "12", "15", "15",
};
inline constexpr const char *kOplWaveLabels[] = {
    "SINE", "HALF SIN", "ABS SIN", "PULS SIN", "ALT SIN", "CAMEL", "SQUARE", "LOG SAW",
};
// Each Algorithm as its connection, operators by number: ">" modulates, "+" sums.
inline constexpr const char *kOplAlgorithmLabels[] = {
    "1>2", "1+2", "1>2>3>4", "1>2+3>4", "1+2>3>4", "1+2>3+4",
};
static_assert(sizeof kOplAlgorithmLabels / sizeof kOplAlgorithmLabels[0] == OPL_ALGO_COUNT,
              "an ALGORITHM label per OplAlgorithm");

// The engine-native form of one Patch: the patch data the voice glue
// reads, with its routing (and op1's feedback) already resolved. Every
// field is fixed for a note when it starts.
struct OplCookedPatch {
    OplPatch patch;     // name is null: the Patch's name lives in its Edit buffer
    FmRouting routing;
};

// Cooked-patch slots, one per Part, shared between Core 0 (publishes) and
// Core 1 (consumes once per buffer). Defined by the Input subsystem.
extern PartSlotT<OplCookedPatch> opl_part_slots[OPL_PART_COUNT];

// The cooked Multi (mix and FX) and its slot, likewise.
using OplMultiCooked = MultiCookedT<OPL_PART_COUNT>;
extern PartSlotT<OplMultiCooked> opl_multi_slot;

// One operator's descriptors. Every one is note-on: the voice reads its
// operators' registers only when a note starts.
#define OPL_OP_DESCRIPTORS(N, TL_DEFAULT, Q_MULT, Q_TL, Q_WAVE)                                                    \
    { opl_op_param(N - 1, OPL_OP_MULT), "MULT", "OP" #N, 0, 15, 1, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, Q_MULT, kNoDefaultCc, kOplMultLabels, 16 }, \
    { opl_op_param(N - 1, OPL_OP_KSL), "KSL", "OP" #N, 0, 3, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kOplKslLabels, 4 }, \
    { opl_op_param(N - 1, OPL_OP_TL), "TL", "OP" #N, 0, 63, TL_DEFAULT, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, Q_TL, kNoDefaultCc }, \
    { opl_op_param(N - 1, OPL_OP_AR), "AR", "OP" #N, 0, 15, 15, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { opl_op_param(N - 1, OPL_OP_DR), "DR", "OP" #N, 0, 15, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { opl_op_param(N - 1, OPL_OP_SL), "SL", "OP" #N, 0, 15, 0, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { opl_op_param(N - 1, OPL_OP_RR), "RR", "OP" #N, 0, 15, 7, ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc }, \
    { opl_op_param(N - 1, OPL_OP_EGT), "EG TYPE", "OP" #N, 0, 1, 1, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kOplEgtLabels, 2 }, \
    { opl_op_param(N - 1, OPL_OP_KSR), "KSR", "OP" #N, 0, 1, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc, kOplOffOnLabels, 2 }, \
    { opl_op_param(N - 1, OPL_OP_WAVE), "WAVE", "OP" #N, 0, 7, 0, ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::NOTE_ON, Q_WAVE, kNoDefaultCc, kOplWaveLabels, 8 }

struct OplModule {
    static constexpr ModuleId kModuleId = ModuleId::OPL;
    static constexpr uint8_t kSchemaVersion = 1;
    static constexpr uint32_t kPartCount = OPL_PART_COUNT;
    using Cooked = OplCookedPatch;

    // The default Multi plays factory bank 0, slot 0 on every Part.
    static constexpr uint8_t default_program(uint32_t) { return 0; }

    // Defaults are an INIT VOICE: the 1>2 Algorithm with OP1 silent, so OP2
    // alone plays a sustained sine.
    static constexpr ParamDescriptor kDescriptors[OPL_P_COUNT] = {
        //                 TL  quick: MULT   TL     WAVE
        OPL_OP_DESCRIPTORS(1, 63,       true,  true,  false),
        OPL_OP_DESCRIPTORS(2, 0,        true,  false, true),
        OPL_OP_DESCRIPTORS(3, 63,       false, false, false),
        OPL_OP_DESCRIPTORS(4, 63,       false, false, false),
        // id               name         group    min max def unit             curve               class                quick  cc
        { OPL_P_ALGORITHM,  "ALGORITHM", "VOICE", 0, OPL_ALGO_COUNT - 1, 0, ParamUnit::ENUM, ParamCurve::LINEAR,
          ParamClass::NOTE_ON, true, kNoDefaultCc, kOplAlgorithmLabels, OPL_ALGO_COUNT },
        { OPL_P_FEEDBACK,   "FEEDBACK",  "VOICE", 0, 7, 0, ParamUnit::NONE, ParamCurve::LINEAR,
          ParamClass::NOTE_ON, true, kNoDefaultCc },
    };

    // Register values -> the engine's patch form, its routing resolved.
    static void cook(const int32_t *v, OplCookedPatch &c) {
        OplPatch &p = c.patch;
        p.name = nullptr;
        for (uint32_t i = 0; i < OPL_NUM_OPS; i++) {
            const int32_t *o = v + opl_op_param(i, OPL_OP_MULT);
            OplOpParams &op = p.op[i];
            op.mult = (uint8_t)o[OPL_OP_MULT];
            op.ksl = (uint8_t)o[OPL_OP_KSL];
            op.tl = (uint8_t)o[OPL_OP_TL];
            op.ar = (uint8_t)o[OPL_OP_AR];
            op.dr = (uint8_t)o[OPL_OP_DR];
            op.sl = (uint8_t)o[OPL_OP_SL];
            op.rr = (uint8_t)o[OPL_OP_RR];
            op.egt = o[OPL_OP_EGT] != 0;
            op.ksr = o[OPL_OP_KSR] != 0;
            op.ws = (uint8_t)o[OPL_OP_WAVE];
        }
        p.feedback = (uint8_t)v[OPL_P_FEEDBACK];
        p.algorithm = (OplAlgorithm)v[OPL_P_ALGORITHM];
        opl_resolve_routing(p, c.routing);
    }
};

#undef OPL_OP_DESCRIPTORS
