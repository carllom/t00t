#pragma once

#include "engine.h"
#include <cstdint>

// Dynamic voice allocator — runs on Core 0.
// Uses a bitmap from Core 1 (envelope active state) to find free voices.
//
// Priority:
// 1. Silent voice (envelope done, not gated) — inaudible steal
// 2. Released voice (envelope active, not gated) — quiet steal
// 3. Oldest active note — audible but least bad

struct VoiceAllocator {
    uint16_t active_mask;              // latest bitmap from Core 1
    uint16_t local_mask;               // working copy (active | newly allocated)
    uint8_t  alloc_age[MAX_VOICES];    // monotonic allocation age
    bool     voice_gated[MAX_VOICES];  // Core 0's gate tracking
    uint8_t  age_counter;              // global allocation counter

    void init();

    // Drain Core 0 RX FIFO for latest bitmap, refresh local_mask.
    // Call once at start of each controller_tick.
    void update();

    // Allocate a voice. Returns 0–15 or -1 if impossible.
    int allocate();

    // Mark a voice as released (gate off).
    void release(int v);
};

void voice_alloc_init();
void voice_alloc_update();
int  voice_alloc_allocate();
void voice_alloc_release(int v);
