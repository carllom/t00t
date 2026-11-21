#pragma once

#include "chip_module.h"
#include "patch/multi.h"
#include "patch/platform.h"

// The chip module's patch platform and Multi instances, owned by the Input
// subsystem; Core 0 only (the display reads them too).
PatchPlatformT<ChipModule> &chip_platform();
MultiT<ChipModule> &chip_multi();
