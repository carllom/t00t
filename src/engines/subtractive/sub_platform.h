#pragma once

#include "patch.h"
#include "patch/multi.h"
#include "patch/platform.h"

// The subtractive module's patch platform and Multi instances, owned by
// the Input subsystem; Core 0 only (the display reads and edits them too).
PatchPlatformT<SubtractiveModule> &sub_platform();
MultiT<SubtractiveModule> &sub_multi();
