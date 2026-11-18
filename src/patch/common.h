#pragma once

#include "descriptor.h"
#include "fx/effect_params.h"
#include <cstdint>

// The common components' descriptor tables: the parts of a sound every
// module shares, described once. They belong to the Multi, not a Patch
// (multi.h); the descriptor dump sends them after the module's own table.
//
//   kCommonDescriptors  the Multi's own settings: the FX and the master level
//   kPartDescriptors    one Part of a Multi; every Part has these

enum CommonParamId : uint16_t {
    COMMON_P_FX_TYPE,
    COMMON_P_FX_MIX,
    COMMON_P_FX_P1,
    COMMON_P_FX_P2,
    COMMON_P_MASTER,
    COMMON_P_COUNT
};

// FX type names, by EffectType value.
inline constexpr const char *kFxTypeLabels[] = {
    "OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE",
};
static constexpr uint8_t kFxTypeCount = sizeof(kFxTypeLabels) / sizeof(kFxTypeLabels[0]);
static_assert(kFxTypeCount == FX_COUNT, "an FX TYPE label per EffectType");

inline constexpr ParamDescriptor kCommonDescriptors[COMMON_P_COUNT] = {
    // id               name       group     min max              default unit             curve               class             quick  cc
    { COMMON_P_FX_TYPE, "FX TYPE", "FX",     0,  kFxTypeCount - 1, 1,     ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc,
      kFxTypeLabels, kFxTypeCount },
    { COMMON_P_FX_MIX,  "FX MIX",  "FX",     0,  127,              0,     ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
    { COMMON_P_FX_P1,   "FX P1",   "FX",     0,  127,              55,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
    { COMMON_P_FX_P2,   "FX P2",   "FX",     0,  127,              36,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
    { COMMON_P_MASTER,  "MASTER",  "MASTER", 0,  127,              127,   ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE, false, kNoDefaultCc },
};
static_assert(descriptor_table_valid(kCommonDescriptors), "invalid common descriptor table");

enum PartParamId : uint16_t {
    PART_P_CHANNEL,  // MIDI channel 0-15, or kPartChannelOff
    PART_P_BANK,     // the Part's Patch: Bank Select MSB (factory 0-63, user 64+) ...
    PART_P_PROGRAM,  // ... and program (slot) in that bank
    PART_P_VOLUME,
    PART_P_PAN,      // 0 = left, 64 = centre, 127 = right
    PART_P_SEND,     // FX send
    PART_P_COUNT
};

static constexpr int32_t kPartChannelOff = 16;

inline constexpr const char *kPartChannelLabels[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "OFF",
};

inline constexpr ParamDescriptor kPartDescriptors[PART_P_COUNT] = {
    // id              name       group   min max              default unit             curve               class                quick  cc
    { PART_P_CHANNEL,  "CHANNEL", "PART", 0,  kPartChannelOff, 0,      ParamUnit::ENUM, ParamCurve::LINEAR, ParamClass::LIVE,    false, kNoDefaultCc,
      kPartChannelLabels, kPartChannelOff + 1 },
    { PART_P_BANK,     "BANK",    "PART", 0,  127,             0,      ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc },
    { PART_P_PROGRAM,  "PROGRAM", "PART", 0,  127,             0,      ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::NOTE_ON, false, kNoDefaultCc },
    { PART_P_VOLUME,   "VOLUME",  "PART", 0,  127,             127,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, 7 },
    { PART_P_PAN,      "PAN",     "PART", 0,  127,             64,     ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, 10 },
    { PART_P_SEND,     "FX SEND", "PART", 0,  127,             127,    ParamUnit::NONE, ParamCurve::LINEAR, ParamClass::LIVE,    false, 91 },
};
static_assert(descriptor_table_valid(kPartDescriptors), "invalid part descriptor table");
