#include "usb_midi.h"
#include "midi_controller.h"
#include "tusb.h"

void usb_midi_init() {
    tusb_init();
}

void usb_midi_task() {
    tud_task();
}

void usb_midi_poll(ParamExchange *params) {
    if (!tud_midi_mounted()) return;

    uint8_t buf[64];
    uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
    if (n > 0) {
        midi_controller_process(buf, n, params);
    }
}
