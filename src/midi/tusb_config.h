#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE
#define CFG_TUD_ENDPOINT0_SIZE   64

// Class enable — MIDI only
#define CFG_TUD_CDC              0
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             1
#define CFG_TUD_VENDOR           0

// MIDI FIFO size
#define CFG_TUD_MIDI_RX_BUFSIZE  64
#define CFG_TUD_MIDI_TX_BUFSIZE  64

#endif
