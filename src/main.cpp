#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "output.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"

// MIDI transport selection — default both on; override in the board header.
#ifndef MIDI_USB
#define MIDI_USB 1
#endif
#ifndef MIDI_UART
#define MIDI_UART 1
#endif

#if MIDI_USB
#include "midi/usb_midi.h"
#endif
#if MIDI_UART
#include "midi/uart_midi.h"
#endif

#if HAS_BUTTONS
#include "controller.h"
#endif

#ifndef HAS_LCD
#define HAS_LCD 0
#endif
#if HAS_LCD
#include "wslcd/display.h"
#endif

static AudioBuffers audio_buffers;
static ParamExchange param_exchange;

// Core 1 entry: runs audio_engine_run(), never returns
static void core1_entry() {
    audio_engine_run(&audio_buffers, &param_exchange);
}

int main() {
#if MIDI_USB
    usb_midi_init();
#endif
#if MIDI_UART
    uart_midi_init();  // DIN MIDI on UART1 RX (GPIO5 / pin 7)
#endif

    param_exchange.init();
    voice_alloc_init();
#if HAS_BUTTONS
    controller_init();
#endif
    midi_controller_init();

    // Start Core 1 (audio synthesis)
    multicore_launch_core1(core1_entry);

    // Start I2S DMA output — must be after Core 1 is ready to receive FIFO messages
    i2s_output_init(&audio_buffers);

#if HAS_LCD
    // Core 0 owns the LCD at low priority (audio + MIDI take precedence).
    display_init();
    display_bringup_test();
#endif

    // Core 0 main loop: poll USB + MIDI + buttons
    absolute_time_t next_tick = get_absolute_time();

    while (true) {
#if MIDI_USB
        usb_midi_task();
        usb_midi_poll(&param_exchange);
#endif
#if MIDI_UART
        uart_midi_poll(&param_exchange);
#endif

#if HAS_BUTTONS
        // 1ms button poll
        if (time_reached(next_tick)) {
            next_tick = delayed_by_ms(next_tick, 1);
            controller_tick(&param_exchange);
        }
#endif

        __wfi();  // sleep until next IRQ (USB, timer, etc.)
    }

    return 0;
}
