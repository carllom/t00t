#pragma once

#include "engine.h"

// DIN (serial) MIDI transport — UART at 31250 baud.
// Default wiring: UART1 RX on GPIO5 (pin 7), via optocoupler.
// An RX interrupt buffers incoming bytes; uart_midi_poll() drains the
// buffer and feeds midi_controller from the main loop.

void uart_midi_init();
void uart_midi_poll(ParamExchange *params);
