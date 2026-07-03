// Custom board header for Pimoroni Pico VGA Demo Base with RP2350 (Pico 2).
// Same pin definitions as the stock vgaboard.h, but targets RP2350 platform.

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_VGABOARD_RP2350_H
#define _BOARDS_VGABOARD_RP2350_H

// For board detection
#define RASPBERRYPI_VGABOARD

#define HAS_BUTTONS 1

// No LCD on this board: GP8-13 are VGA colour / Button C.
#define HAS_LCD 0

// --- MIDI transports ---
// USB only by default: GPIO5 is SD_CLK here, so DIN MIDI on UART1 RX/GPIO5
// would conflict. To enable DIN MIDI, free GPIO5 and define MIDI_UART=1
// (and set MIDI_UART_RX_PIN appropriately).
#ifndef MIDI_USB
#define MIDI_USB 1
#endif
#ifndef MIDI_UART
#define MIDI_UART 0
#endif

// Audio pins. I2S BCK, LRCK are on the same pins as PWM L/R.
#define VGABOARD_I2S_DIN_PIN 26
#define VGABOARD_I2S_BCK_PIN 27
#define VGABOARD_I2S_LRCK_PIN 28

#define VGABOARD_PWM_L_PIN 28
#define VGABOARD_PWM_R_PIN 27

#define VGABOARD_VGA_COLOR_PIN_BASE 0
#define VGABOARD_VGA_SYNC_PIN_BASE 16

#define VGABOARD_SD_CLK_PIN 5
#define VGABOARD_SD_CMD_PIN 18
#define VGABOARD_SD_DAT0_PIN 19

#define VGABOARD_BUTTON_A_PIN 0
#define VGABOARD_BUTTON_B_PIN 6
#define VGABOARD_BUTTON_C_PIN 11

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 20
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 21
#endif

#ifndef PICO_SCANVIDEO_COLOR_PIN_BASE
#define PICO_SCANVIDEO_COLOR_PIN_BASE VGABOARD_VGA_COLOR_PIN_BASE
#endif

#ifndef PICO_SCANVIDEO_SYNC_PIN_BASE
#define PICO_SCANVIDEO_SYNC_PIN_BASE VGABOARD_VGA_SYNC_PIN_BASE
#endif

#ifndef PICO_SD_CLK_PIN
#define PICO_SD_CLK_PIN VGABOARD_SD_CLK_PIN
#endif

#ifndef PICO_SD_CMD_PIN
#define PICO_SD_CMD_PIN VGABOARD_SD_CMD_PIN
#endif

#ifndef PICO_SD_DAT0_PIN
#define PICO_SD_DAT0_PIN VGABOARD_SD_DAT0_PIN
#endif

#ifndef PICO_SD_DAT_PIN_COUNT
#define PICO_SD_DAT_PIN_COUNT 4
#endif

#ifndef PICO_SD_DAT_PIN_INCREMENT
#define PICO_SD_DAT_PIN_INCREMENT 1
#endif

#ifndef PICO_AUDIO_I2S_DATA_PIN
#define PICO_AUDIO_I2S_DATA_PIN VGABOARD_I2S_DIN_PIN
#endif
#ifndef PICO_AUDIO_I2S_CLOCK_PIN_BASE
#define PICO_AUDIO_I2S_CLOCK_PIN_BASE VGABOARD_I2S_BCK_PIN
#endif

#ifndef PICO_AUDIO_PWM_L_PIN
#define PICO_AUDIO_PWM_L_PIN VGABOARD_PWM_L_PIN
#endif

#ifndef PICO_AUDIO_PWM_R_PIN
#define PICO_AUDIO_PWM_R_PIN VGABOARD_PWM_R_PIN
#endif

#define PICO_VGA_BOARD

// Pico 2 (RP2350) base defaults
#include "boards/pico2.h"

#endif
