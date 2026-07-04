#include "voice_alloc.h"
#include "pico/multicore.h"

static VoiceAllocator alloc;

void VoiceAllocator::init() {
    active_mask = 0;
    local_mask = 0;
    age_counter = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        alloc_age[v] = 0;
        voice_gated[v] = false;
    }
}

void VoiceAllocator::update() {
    // Drain FIFO (Core 1 → Core 0), keep latest value
    uint32_t val;
    while (multicore_fifo_pop_timeout_us(0, &val)) {
        active_mask = (uint16_t)val;
    }
    // Start local copy from Core 1's view
    local_mask = active_mask;
}

int VoiceAllocator::allocate() {
    // Priority 1: silent voice (not active on Core 1, not gated by Core 0)
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (!(local_mask & (1u << v)) && !voice_gated[v]) {
            local_mask |= (1u << v);
            voice_gated[v] = true;
            alloc_age[v] = age_counter++;
            return (int)v;
        }
    }

    // Priority 2: released voice (active on Core 1 but not gated — in release phase)
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if ((local_mask & (1u << v)) && !voice_gated[v]) {
            local_mask |= (1u << v);
            voice_gated[v] = true;
            alloc_age[v] = age_counter++;
            return (int)v;
        }
    }

    // Priority 3: steal oldest active (gated) voice
    int oldest = -1;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (voice_gated[v]) {
            if (oldest < 0 || (int8_t)(alloc_age[v] - alloc_age[oldest]) < 0) {
                oldest = (int)v;
            }
        }
    }
    if (oldest >= 0) {
        alloc_age[oldest] = age_counter++;
        // voice_gated stays true — we're re-stealing it
    }
    return oldest;
}

void VoiceAllocator::release(int v) {
    if (v >= 0 && v < (int)MAX_VOICES) {
        voice_gated[v] = false;
    }
}

// Free function API
void voice_alloc_init()          { alloc.init(); }
void voice_alloc_update()        { alloc.update(); }
int  voice_alloc_allocate()      { return alloc.allocate(); }
void voice_alloc_release(int v)  { alloc.release(v); }

uint16_t voice_alloc_active_mask() { return alloc.active_mask; }

uint16_t voice_alloc_gated_mask() {
    uint16_t m = 0;
    for (uint32_t v = 0; v < MAX_VOICES; v++) {
        if (alloc.voice_gated[v]) m |= (1u << v);
    }
    return m;
}
