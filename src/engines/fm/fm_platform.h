#pragma once

#include "fm_module.h"
#include "patch/multi.h"
#include "patch/platform.h"

// The FM module's patch platform and Multi instances, owned by the Input
// subsystem; Core 0 only (the display reads them too).
PatchPlatformT<FmModule> &fm_platform();
MultiT<FmModule> &fm_multi();
