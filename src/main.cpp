#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "output.h"
#include "audio_engine.h"
#include "controller.h"
#include "voice_alloc.h"
#include <cstdio>

static AudioBuffers audio_buffers;
static ParamExchange param_exchange;

// Core 1 entry: runs audio_engine_run(), never returns
static void core1_entry() {
    audio_engine_run(&audio_buffers, &param_exchange);
}

// 1ms repeating timer callback — runs on Core 0 IRQ context
static bool timer_callback(repeating_timer_t *rt) {
    (void)rt;
    controller_tick(&param_exchange);
    return true;  // keep repeating
}

int main() {
    stdio_init_all();

    param_exchange.init();
    voice_alloc_init();
    controller_init();

    // Start Core 1 (audio synthesis)
    multicore_launch_core1(core1_entry);

    // Start I2S DMA output — must be after Core 1 is ready to receive FIFO messages
    i2s_output_init(&audio_buffers);

    // Start 1ms repeating timer for button polling
    repeating_timer_t tick_timer;
    add_repeating_timer_ms(-1, timer_callback, nullptr, &tick_timer);

    // Core 0 main loop — sleep, wake on IRQ (timer handles everything)
    while (true) {
        __wfi();
    }

    return 0;
}
