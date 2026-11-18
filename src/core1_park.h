#pragma once

#include <cstdint>

// Parking Core 1 in RAM so Core 0 can erase and program flash: while flash
// is being written, XIP is unavailable and any instruction fetch from it
// would fail. Core 1 parks only at its park point, between buffers, so it
// never stops halfway through a render.
//
// The SDK's multicore lockout isn't used because it takes over the
// inter-core FIFO, which carries the audio buffer protocol.

// Core 0: ask Core 1 to park and wait until it has; false (request
// withdrawn) if it hasn't within `timeout_us`.
bool core1_park(uint32_t timeout_us);

// Core 0: let a parked Core 1 continue.
void core1_unpark();

extern volatile bool g_core1_park_request;

// Core 1, RAM-resident: waits while parked.
void core1_park_wait();

// Core 1, once per buffer before rendering: true if it parked, in which
// case the caller should drop the buffer it was about to render.
inline bool core1_park_point() {
    if (!g_core1_park_request) return false;
    core1_park_wait();
    return true;
}
