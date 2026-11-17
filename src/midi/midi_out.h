#pragma once

#include <cstdint>

// Outgoing MIDI (SysEx replies). Goes out over USB-MIDI only: the DIN port is
// receive-only on both boards. Queued and sent from usb_midi_task().
// Returns false, sending nothing, if the queue hasn't room for the whole
// message right now; with no USB host attached the message is dropped and
// true returned, since nobody will ever take it.
bool midi_out_send(const uint8_t *data, uint32_t len);
