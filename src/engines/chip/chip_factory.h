#pragma once

#include "factory_bank.h"  // kChipFactoryBank[], kChipFactoryCount (generated from instruments.h, ay_instruments.h)
#include "patch/platform.h"

// The chip factory banks: one, the hand-authored SID instruments then the
// AY ones, as stored Patches -- so a program number is the instrument's
// combined index.
inline const FactoryBank kChipFactoryBanks[] = { { kChipFactoryBank, kChipFactoryCount } };
static constexpr uint32_t kChipFactoryBankCount = 1;
