#pragma once

#include "descriptor.h"
#include <cstdint>

// The common components' descriptor table: the parts of a sound every
// module shares, described once -- the FX. The descriptor dump sends it
// after the module's own table. These belong to the Multi, not a Patch;
// they are set through each module's own FX CCs.

enum CommonParamId : uint16_t {
    COMMON_P_FX_TYPE,
    COMMON_P_FX_MIX,
    COMMON_P_FX_P1,
    COMMON_P_FX_P2,
    COMMON_P_COUNT
};

// FX type names, by EffectType value (engine_base.h).
inline constexpr const char *kFxTypeLabels[] = {
    "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE",
};
static constexpr uint8_t kFxTypeCount = sizeof(kFxTypeLabels) / sizeof(kFxTypeLabels[0]);

inline constexpr ParamDescriptor kCommonDescriptors[COMMON_P_COUNT] = {
    // id               name       group min max              default unit             curve               class             quick  cc
    { COMMON_P_FX_TYPE, "FX TYPE", "FX", 0,  kFxTypeCount - 1, 1,     ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc,
      kFxTypeLabels, kFxTypeCount },
    { COMMON_P_FX_MIX,  "FX MIX",  "FX", 0,  127,              0,     ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
    { COMMON_P_FX_P1,   "FX P1",   "FX", 0,  127,              55,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
    { COMMON_P_FX_P2,   "FX P2",   "FX", 0,  127,              36,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
};
static_assert(descriptor_table_valid(kCommonDescriptors), "invalid common descriptor table");
