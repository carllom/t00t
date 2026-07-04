// Core-0 facing display API. Owns the LCD at low priority; audio + MIDI on the
// main loop always take precedence.
#pragma once

// Bring up the panel and paint the static UI chrome (labels, title, frames).
void display_init();

// Poll telemetry and redraw only the fields that changed. Cheap when idle
// (just value compares); a change triggers one or more small DMA blits. Call
// from the Core 0 main loop; it self-limits its refresh rate.
void display_task();

// One-shot bring-up test pattern (colour bars + banner). Not used by the live
// UI, kept for driver diagnostics.
void display_bringup_test();
