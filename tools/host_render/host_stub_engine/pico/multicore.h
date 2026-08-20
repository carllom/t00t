#pragma once

// Minimal stand-in for pico-sdk's pico/multicore.h, providing only the one
// symbol src/voice_alloc.cpp references (inside VoiceAllocator::update(),
// which this test suite never calls -- see test_voice_alloc.cpp). Lets the
// real, unmodified voice_alloc.cpp compile and link on the host so the test
// exercises production code directly instead of a duplicated copy of its
// logic. Always reports "nothing pending"; behavior doesn't matter since
// update() is never invoked here.

#include <cstdint>

static inline bool multicore_fifo_pop_timeout_us(uint32_t /*timeout_us*/, uint32_t * /*out*/) {
    return false;
}
