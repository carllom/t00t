// Host-buildable unit test for src/voice_alloc.h's steal-priority tiering
// (ticket #101, part of the Core 0 Input pipeline redesign, spec #99).
// Same convention as test_input_layer.cpp / test_envelope.cpp (test_*()
// functions, an aggregated `bool ok`, "ALL CHECKS PASSED"/"CHECKS FAILED").
//
// Question this answers: does a self-expiring Strike's Handler pattern --
// voice_alloc_allocate() immediately followed by voice_alloc_release() --
// work correctly against the *existing* VoiceAllocator tiering, with no
// allocator code changes? Per the ticket, this is a verification test: if
// it fails, that's a real gap to report and fix minimally, not to route
// around.
//
// Seam under test: VoiceAllocator's own methods (init/allocate/release),
// exercised directly rather than through the voice_alloc_*() free-function
// wrappers -- those wrappers are one-line pass-throughs (see voice_alloc.cpp)
// plus VoiceAllocator::update()'s hardware FIFO drain (multicore_fifo_pop_*,
// pico-sdk, not host-buildable). `active_mask` is Core 1's real external
// input to this subsystem (the bitmap `update()` copies into `local_mask`),
// so tests write it directly and replicate that copy inline -- everything
// else asserted on is only ever observed through allocate()'s return value,
// never by reaching into `voice_gated[]`/`local_mask`/`alloc_age[]` directly.

#include "../../src/voice_alloc.h"

#include <cstdio>

namespace {

// Replicates VoiceAllocator::update()'s only host-relevant effect (copying
// the latest Core 1 bitmap into the working copy) without its FIFO drain.
void simulate_core1_report(VoiceAllocator &alloc, uint32_t active_mask) {
    alloc.active_mask = active_mask;
    alloc.local_mask = active_mask;
}

bool test_self_expiring_strike_beats_held_voices(const char *) {
    VoiceAllocator alloc;
    alloc.init();

    // Trigger a self-expiring Strike: allocate, then release synchronously
    // in the same call, exactly as the settled Handler pattern does.
    int v = alloc.allocate();
    alloc.release(v);

    // Fill every other voice as genuinely held (gated, never released) --
    // isolates v's own tier from the rest: it's the only non-held voice.
    for (uint32_t i = 0; i < MAX_VOICES - 1; i++) {
        alloc.allocate();
    }

    // Interim: Core 1 hasn't reported anything back yet (active_mask/
    // local_mask still reflect whatever allocate() itself set) -- v should
    // still be preferred over every held voice.
    int picked_interim = alloc.allocate();
    bool interim_ok = (picked_interim == v);

    // v got re-gated by that pick (it's being reused) -- release it again,
    // as a second self-expiry cycle, then have Core 1 report it fully silent.
    alloc.release(v);
    simulate_core1_report(alloc, alloc.active_mask & ~(1u << (uint32_t)v));

    int picked_after_silent = alloc.allocate();
    bool silent_ok = (picked_after_silent == v);

    bool ok = interim_ok && silent_ok;
    printf(ok ? "  OK: self-expiring Strike voice beats held voices both before and after active_mask reports it silent\n"
              : "  FAIL: self-expiring Strike voice lost to a held voice (interim_ok=%d silent_ok=%d)\n",
           interim_ok, silent_ok);
    return ok;
}

bool test_never_released_voice_stays_worst_tier(const char *) {
    VoiceAllocator alloc;
    alloc.init();

    // A voice that gets triggered but never released -- the failure mode
    // this pattern exists to avoid.
    int v_held = alloc.allocate();

    // Fill every other voice as released (allocate + release), so all of
    // them genuinely outrank a held voice, regardless of active_mask.
    for (uint32_t i = 0; i < MAX_VOICES - 1; i++) {
        int w = alloc.allocate();
        alloc.release(w);
    }

    // Core 1 now reports v_held has gone fully silent -- but nothing ever
    // told Core 1 to gate it off, so it must still not be preferred.
    simulate_core1_report(alloc, alloc.active_mask & ~(1u << (uint32_t)v_held));

    int picked = alloc.allocate();
    bool ok = (picked != v_held);
    printf(ok ? "  OK: a never-released (still-held) voice stays in the worst tier even once active_mask reports it silent\n"
              : "  FAIL: a never-released voice was incorrectly reused after active_mask cleared it\n");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    printf("== self-expiring Strike: released, then silent, both beat held ==\n");
    ok = test_self_expiring_strike_beats_held_voices("n/a") && ok;

    printf("\n== never-released voice stays worst tier even after active_mask clears ==\n");
    ok = test_never_released_voice_stays_worst_tier("n/a") && ok;

    printf(ok ? "\nALL CHECKS PASSED\n" : "\nCHECKS FAILED\n");
    return ok ? 0 : 1;
}
