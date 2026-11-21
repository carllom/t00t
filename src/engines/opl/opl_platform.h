#pragma once

#include "opl_module.h"
#include "patch/multi.h"
#include "patch/platform.h"

// The OPL module's patch platform and Multi instances, owned by the Input
// subsystem; Core 0 only (the display reads them too).
PatchPlatformT<OplModule> &opl_platform();
MultiT<OplModule> &opl_multi();
