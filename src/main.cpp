#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "output.h"
#include "audio_engine.h"
#include "midi/midi_controller.h"

// Not every engine has a dynamic voice allocator — the tracker engine assigns
// channel N to voice N directly, so voice_alloc.cpp isn't linked into that
// build (set by CMakeLists.txt) and these calls must compile out too.
#ifndef HAS_VOICE_ALLOC
#define HAS_VOICE_ALLOC 1
#endif
#if HAS_VOICE_ALLOC
#include "voice_alloc.h"
#endif

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

// The tracker engine's Core 0 player task (#18) -- song load, TickBlock
// ring, transport. Other engines leave this at the default 0.
#ifndef HAS_TRACKER_PLAYER
#define HAS_TRACKER_PLAYER 0
#endif
#if HAS_TRACKER_PLAYER
#include "player_task.h"
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
#if HAS_VOICE_ALLOC
    voice_alloc_init();
#endif
#if HAS_BUTTONS
    controller_init();
#endif
    midi_controller_init();

#if HAS_TRACKER_PLAYER
    // Sample SRAM load + resident-table build + ring priming, all before
    // Core 1's thread exists -- stricter than tracker.md's "Core 0 primes
    // two blocks before Core 1 starts" requires, since Core 1 doesn't touch
    // the ring until the first DMA-triggered fill request anyway (that
    // starts even later, at i2s_output_init() below).
    tracker_player_task_init();
#endif

    // Start Core 1 (audio synthesis)
    multicore_launch_core1(core1_entry);

    // Start I2S DMA output — must be after Core 1 is ready to receive FIFO messages
    i2s_output_init(&audio_buffers);

#if HAS_LCD
    // Core 0 owns the LCD at low priority (audio + MIDI take precedence).
    display_init();
#endif

    // Core 0 main loop: poll USB + MIDI + buttons
    absolute_time_t next_tick = get_absolute_time();

    while (true) {
#if !HAS_BUTTONS && HAS_VOICE_ALLOC
        // Drain Core 1's active-voice feedback before this pass allocates, so the
        // allocator's silent/released/oldest priority uses fresh state. On button
        // boards controller_tick() already does this at the start of its tick.
        voice_alloc_update();
#endif

#if HAS_TRACKER_PLAYER
        // Drains Core 1's tick-consumed doorbell and keeps the ring topped
        // up. Woken reliably by output.cpp's DMA IRQ (~every
        // SAMPLES_PER_BUFFER/SAMPLE_RATE seconds) below, regardless of
        // whether any MIDI host is even attached.
        tracker_player_task();
#endif

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

#if HAS_LCD
        display_task();  // low-priority; self-limits to ~20 Hz, redraws on change
#endif

        __wfi();  // sleep until next IRQ (USB, timer, etc.)
    }

    return 0;
}
