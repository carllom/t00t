#include "uart_midi.h"
#include "midi_controller.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"

// --- UART MIDI configuration ---
// Defaults: UART1 RX on GPIO5 (pin 7). Override via board header if needed.
#ifndef MIDI_UART_NUM
#define MIDI_UART_NUM 1
#endif
#ifndef MIDI_UART_RX_PIN
#define MIDI_UART_RX_PIN 5   // GPIO5 / pin 7 — UART1 RX
#endif

#define MIDI_UART_INST (MIDI_UART_NUM ? uart1 : uart0)
#define MIDI_UART_IRQ  (MIDI_UART_NUM ? UART1_IRQ : UART0_IRQ)

static constexpr uint MIDI_BAUD = 31250;

// --- RX ring buffer (filled in IRQ, drained in poll) ---
static constexpr uint32_t RING_SIZE = 256;  // power of two
static uint8_t  ring[RING_SIZE];
static volatile uint32_t ring_head = 0;  // written by IRQ
static volatile uint32_t ring_tail = 0;  // written by poll

static void uart_midi_irq() {
    while (uart_is_readable(MIDI_UART_INST)) {
        uint8_t byte = uart_getc(MIDI_UART_INST);
        uint32_t next = (ring_head + 1) & (RING_SIZE - 1);
        if (next != ring_tail) {  // drop on overflow rather than clobber
            ring[ring_head] = byte;
            ring_head = next;
        }
    }
}

void uart_midi_init() {
    uart_init(MIDI_UART_INST, MIDI_BAUD);
    gpio_set_function(MIDI_UART_RX_PIN, GPIO_FUNC_UART);

    // 8 data bits, no parity, 1 stop bit (MIDI standard)
    uart_set_format(MIDI_UART_INST, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(MIDI_UART_INST, true);

    // RX interrupt wakes the core and buffers bytes
    irq_set_exclusive_handler(MIDI_UART_IRQ, uart_midi_irq);
    irq_set_enabled(MIDI_UART_IRQ, true);
    uart_set_irq_enables(MIDI_UART_INST, true, false);  // rx on, tx off
}

void uart_midi_poll(ParamExchange *params) {
    uint8_t buf[64];
    uint32_t n = 0;

    uint32_t tail = ring_tail;
    while (tail != ring_head && n < sizeof(buf)) {
        buf[n++] = ring[tail];
        tail = (tail + 1) & (RING_SIZE - 1);
    }
    ring_tail = tail;

    if (n > 0) {
        midi_controller_process(buf, n, params);
    }
}
