#pragma once

// Core 0 main-loop hook for a module on the patch platform: publishes any
// cooked patch still waiting for Core 1 to take the previous one. Called
// once per pass when the build sets HAS_PATCH_PLATFORM; the module's Input
// subsystem implements it.
void patch_platform_task();
