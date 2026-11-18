#pragma once

#include "sensor_event.h"
#include "wslcd/page.h"
#include "wslcd/ui_command_shaping.h"
#include "wslcd/ui_nav_consumer.h"

#include <cstdint>

// Rotary encoder (CLK/DT quadrature + SW push button) driving Page
// navigation on boards that wire one in place of the vgaboard-style
// discrete Buttons -- gated by HAS_ENCODER, same shape as HAS_BUTTONS/
// HAS_LCD (see the board header for pin assignment). Owns a single shared
// PageCursor (only one engine/Page list is ever linked into a build, same
// reasoning as MidiUiState's shared instance in midi_controller.h) and
// applies whichever UiCommand fires immediately, each 1ms poll, through the
// existing Sensor event -> Shaping -> ui_nav_consumer_feed() path -- a CW
// detent and a CCW detent are shaped as two independent synthetic buttons
// (PLUS/MINUS); the SW button goes through press-duration Shaping: a short
// press is ENTER, a long press EXIT (ui_command_shaping.h). A claiming
// interaction context (encoder_nav_set_claim()) sees every command before
// Page navigation does.
//
// Rotation is quarter-step quadrature-decoded and only counted at a full
// detent (four valid quarter-steps), matching common KY-040-style encoders'
// mechanical click spacing.

void encoder_nav_init(uint8_t page_count, uint8_t performance_index);

// Call once per 1ms tick: reads CLK/DT/SW, decodes/debounces, and applies
// any UiCommand that fires to the shared PageCursor.
void encoder_nav_tick();

// The shared PageCursor's current Page index, for a module's display_task()
// to read each redraw.
uint8_t encoder_nav_page_index();

// Set (or clear, with nullptr) the interaction context that may claim
// commands before Page navigation. Called from Core 0 only, like the tick.
void encoder_nav_set_claim(UiCommandClaim claim);
