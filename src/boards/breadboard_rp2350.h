// Board header for Pico 2 breadboard with Adafruit PCM5122 I2S DAC.
// No VGA, no buttons — MIDI-only control via USB.
//
// Wiring (Pico 2 → PCM5122 breakout):
//
//   Pico 2 pin          PCM5122 pin     GPIO
//   ─────────────────────────────────────────
//   GPIO 16  (pin 21)   BCK             Bit clock
//   GPIO 17  (pin 22)   WSEL            Word select (LRCK)
//   GPIO 18  (pin 24)   DIN             Data in
//   3V3 OUT  (pin 36)   VIN             Power (3.3V)
//   GND      (pin 38)   GND             Ground
//
// Leave MOD1 and MOD2 unconnected (hardware control mode, default).
// Leave CLK Dir jumper open (secondary mode — Pico provides clocks).
// MCK not connected (PCM5122 auto-generates from BCK).
//
// DIN MIDI in (via optocoupler) → UART1 RX, GPIO 5 (pin 7), 31250 baud.
//
// Profiling pin: GPIO 22 (directly probe on breadboard)

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_BREADBOARD_RP2350_H
#define _BOARDS_BREADBOARD_RP2350_H

// --- I2S DAC pins ---
#define PICO_AUDIO_I2S_DATA_PIN       18
#define PICO_AUDIO_I2S_CLOCK_PIN_BASE 16   // BCK=16, LRCK=17 (consecutive)

// --- No buttons on breadboard ---
#define HAS_BUTTONS 0

// --- MIDI transports ---
// DIN MIDI on UART1 RX (GPIO5) is the primary input; USB stays on as a fallback.
#ifndef MIDI_UART
#define MIDI_UART 1
#endif
#ifndef MIDI_USB
#define MIDI_USB 1
#endif

// --- No VGA, no SD ---

// UART defaults (usable for debug if needed)
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// Pico 2 (RP2350) base defaults
#include "boards/pico2.h"

#endif
