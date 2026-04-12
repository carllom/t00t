#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "output.h"
#include "audio_engine.h"
#include "voice_alloc.h"
#include "midi/midi_controller.h"
#include "midi/usb_midi.h"

#if HAS_BUTTONS
#include "controller.h"
#endif

static AudioBuffers audio_buffers;
static ParamExchange param_exchange;

// Core 1 entry: runs audio_engine_run(), never returns
static void core1_entry() {
    audio_engine_run(&audio_buffers, &param_exchange);
}

int main() {
    usb_midi_init();

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

    // Core 0 main loop: poll USB + MIDI + buttons
    absolute_time_t next_tick = get_absolute_time();

    while (true) {
        usb_midi_task();
        usb_midi_poll(&param_exchange);

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
