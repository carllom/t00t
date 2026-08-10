#include "wslcd/display.h"

// Chip module F0 measurement build (sid.md §1 P0): no display.
//
// The rig is a pin-only measurement (audio_engine.cpp) and a display task
// would put SPI traffic and Core 0 work alongside the thing being measured.
// The speech engine's #31 profiling build makes the same call for the same
// reason. sid.md §5 of the LCD work is P5; §15 open question 3 (what VM state
// the display wants) is not answerable until the frame VM exists at P3.
void display_init() {}
void display_task() {}
void display_bringup_test() {}
