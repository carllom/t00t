#pragma once

#include <cstdint>

// Outgoing MIDI (SysEx replies). Goes out over USB-MIDI only: the DIN port is
// receive-only on both boards. Queued and sent from usb_midi_task(); a
// message that doesn't fit the queue, or arrives while no USB host is
// attached, is dropped whole.
void midi_out_send(const uint8_t *data, uint32_t len);
