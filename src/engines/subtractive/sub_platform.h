#pragma once

#include "patch.h"
#include "patch/platform.h"

// The subtractive module's patch platform instance, owned by the Input
// subsystem; Core 0 only (the display reads and edits it too).
PatchPlatformT<SubtractiveModule> &sub_platform();
