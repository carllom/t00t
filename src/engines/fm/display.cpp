#include "display.h"

// Stub (#41): the FM engine has no status UI yet -- this only exists so the
// shared wslcd/gfx.cpp path still links. fm.md §6.1's display.cpp (operator
// budget / voice-dot readout) is deferred until the operator kernel and
// voice allocator weighting (§6.4) land.
void display_init() {}
void display_task() {}
void display_bringup_test() {}
