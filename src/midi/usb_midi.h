#pragma once

#include "../engine.h"

// USB MIDI transport — thin wrapper around TinyUSB.
// Reads raw MIDI bytes and feeds them to midi_controller.

void usb_midi_init();
void usb_midi_task();  // call from main loop (runs tud_task)
void usb_midi_poll(ParamExchange *params);
