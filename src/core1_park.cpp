#include "core1_park.h"
#include "pico.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

// Each park request has its own number; Core 1 acknowledges by echoing the
// number it parked for, so Core 0 never mistakes an earlier park for the
// current one.
volatile bool g_core1_park_request = false;
static volatile uint32_t s_request_seq = 0;
static volatile uint32_t s_parked_seq = 0;

bool core1_park(uint32_t timeout_us) {
    const uint32_t seq = s_request_seq + 1;
    s_request_seq = seq;
    __dmb();
    g_core1_park_request = true;
    __dmb();
    const uint32_t start = time_us_32();
    while (s_parked_seq != seq) {
        if (time_us_32() - start > timeout_us) {
            core1_unpark();
            return false;
        }
    }
    __dmb();
    return true;
}

void core1_unpark() {
    __dmb();
    g_core1_park_request = false;
    __sev();
}

// Runs from RAM and calls nothing, so it never fetches from flash.
void __no_inline_not_in_flash_func(core1_park_wait)() {
    s_parked_seq = s_request_seq;
    __dmb();
    while (g_core1_park_request) __wfe();
    __dmb();
}
