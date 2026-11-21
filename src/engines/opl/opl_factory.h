#pragma once

#include "factory_bank.h"  // kOplFactoryBank[], kOplFactoryCount (generated from patches.h)
#include "patch/platform.h"

// The OPL factory banks: one, the hand-authored patches.h set as stored
// Patches.
inline const FactoryBank kOplFactoryBanks[] = { { kOplFactoryBank, kOplFactoryCount } };
static constexpr uint32_t kOplFactoryBankCount = 1;
