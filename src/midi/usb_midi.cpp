#include "usb_midi.h"
#include "midi_out.h"
#include "midi_controller.h"
#include "midi_parser.h"
#include "tusb.h"

void usb_midi_init() {
    tusb_init();
}

// --- TX queue (midi_out.h): filled by midi_out_send(), drained here ---
static constexpr uint32_t TX_SIZE = 512;  // power of two
static uint8_t tx[TX_SIZE];
static uint32_t tx_head = 0;  // next byte to write
static uint32_t tx_tail = 0;  // next byte to send

bool midi_out_send(const uint8_t *data, uint32_t len) {
    if (!tud_midi_mounted()) return true;
    uint32_t used = (tx_head - tx_tail) & (TX_SIZE - 1);
    if (len >= TX_SIZE - used) return false;
    for (uint32_t i = 0; i < len; i++) {
        tx[tx_head] = data[i];
        tx_head = (tx_head + 1) & (TX_SIZE - 1);
    }
    return true;
}

static void usb_midi_flush() {
    while (tx_tail != tx_head && tud_midi_mounted()) {
        uint32_t run = (tx_head > tx_tail) ? tx_head - tx_tail : TX_SIZE - tx_tail;
        uint32_t sent = tud_midi_stream_write(0, &tx[tx_tail], run);
        if (sent == 0) return;
        tx_tail = (tx_tail + sent) & (TX_SIZE - 1);
    }
    if (!tud_midi_mounted()) tx_tail = tx_head;
}

void usb_midi_task() {
    tud_task();
    usb_midi_flush();
}

void usb_midi_poll(ParamExchange *params) {
    if (!tud_midi_mounted()) return;

    uint8_t buf[64];
    uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
    if (n > 0) {
        midi_parser.select(MIDI_SOURCE_USB);
        midi_controller_process(buf, n, params);
    }
}
