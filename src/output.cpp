#include "output.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

// We reuse the PIO program from pico-extras but drive DMA ourselves.
// The .pio file is compiled to a header by the pico-extras CMake build.
#include "audio_i2s.pio.h"

static constexpr uint I2S_DATA_PIN = PICO_AUDIO_I2S_DATA_PIN;       // GPIO 26
static constexpr uint I2S_CLOCK_BASE = PICO_AUDIO_I2S_CLOCK_PIN_BASE; // GPIO 27

static PIO pio_hw;
static uint pio_sm;
static int dma_chan;
static AudioBuffers *audio_bufs;
static volatile uint8_t playing_index;  // buffer currently being played by DMA

// DMA IRQ handler: called when a buffer finishes playing.
// Starts DMA on the next buffer and tells Core 1 which buffer to fill.
static void __isr __time_critical_func(dma_irq_handler)() {
    dma_hw->ints0 = (1u << dma_chan);  // acknowledge IRQ

    // The buffer that just finished playing needs refilling
    uint8_t finished = playing_index;

    // Swap to the other buffer and start DMA
    playing_index = 1 - playing_index;
    dma_channel_set_read_addr(dma_chan, audio_bufs->data[playing_index], true);

    // Tell Core 1: "fill buffer <finished>"
    // Non-blocking: if FIFO is full we drop (Core 1 is behind — will catch up)
    multicore_fifo_push_timeout_us(finished, 0);
}

void i2s_output_init(AudioBuffers *buffers) {
    audio_bufs = buffers;

    // Clear both buffers to silence
    for (int b = 0; b < 2; b++) {
        for (uint32_t i = 0; i < SAMPLES_PER_BUFFER * 2; i++) {
            buffers->data[b][i] = 0;
        }
    }

    // --- PIO setup ---
    pio_hw = pio0;
    pio_sm = pio_claim_unused_sm(pio_hw, true);

    // Set GPIO functions to PIO
    gpio_set_function(I2S_DATA_PIN, GPIO_FUNC_PIO0);
    gpio_set_function(I2S_CLOCK_BASE, GPIO_FUNC_PIO0);
    gpio_set_function(I2S_CLOCK_BASE + 1, GPIO_FUNC_PIO0);

    // Load PIO program (non-swapped: clock_pin_base=BCK, base+1=LRCK)
    uint offset = pio_add_program(pio_hw, &audio_i2s_program);
    audio_i2s_program_init(pio_hw, pio_sm, offset, I2S_DATA_PIN, I2S_CLOCK_BASE);

    // Set PIO clock divider for 44100 Hz sample rate.
    // Each stereo sample = 32 bits, each bit = 2 PIO clocks (out + jmp/set),
    // so one sample = 64 PIO clocks. divider = sys_clk / (sample_rate * 64).
    // The pico-extras code uses: divider = sys_clk * 4 / sample_rate,
    // then sets int_frac as divider>>8 / divider&0xff.
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / SAMPLE_RATE;
    pio_sm_set_clkdiv_int_frac(pio_hw, pio_sm, divider >> 8u, divider & 0xffu);

    // --- DMA setup ---
    dma_chan = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);  // 32-bit: L+R as one word
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_PIO0_TX0 + pio_sm);

    dma_channel_configure(
        dma_chan,
        &c,
        &pio_hw->txf[pio_sm],     // write to PIO TX FIFO
        buffers->data[0],          // initial read from buffer 0
        SAMPLES_PER_BUFFER,        // transfer count (32-bit words = stereo samples)
        false                      // don't start yet
    );

    // Enable DMA IRQ
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // --- Start ---
    playing_index = 0;
    dma_channel_set_read_addr(dma_chan, buffers->data[0], true);  // start DMA
    pio_sm_set_enabled(pio_hw, pio_sm, true);                     // start PIO
}
