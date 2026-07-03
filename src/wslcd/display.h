// Core-0 facing display API. Milestone 1: driver bring-up only. This is where
// the synth UI will grow later (a low-priority display_task()).
#pragma once

// Bring up the panel: SPI/DMA/backlight, reset + init, clear to black.
void display_init();

// One-shot bring-up test: colour bars, gradient band, and a text banner.
void display_bringup_test();
