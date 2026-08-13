#include "wslcd/display.h"

// Chip module: no display yet (sid.md §1 P5 owns the LCD UI).
//
// P1 through P4 have nothing worth putting on a screen yet -- §15 open
// question 3 (how much VM state the display wants) isn't answerable until
// the frame VM exists at P3, and HAS_LCD defaults to 0 on boards without one
// anyway, so these stubs just keep main.cpp linking unchanged in the
// meantime (same reasoning the P0 rig used, extended forward).
void display_init() {}
void display_task() {}
void display_bringup_test() {}
