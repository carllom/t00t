#pragma once

// Minimal stand-in for pico-sdk's hardware/sync.h, providing only the two
// symbols engine_base.h's ParamExchangeT uses, so an engine's engine.h (and
// the DSP headers that include engine_base.h) can be host-built. Single-
// threaded host code needs neither a barrier nor an event signal.

static inline void __compiler_memory_barrier() { __asm__ volatile("" ::: "memory"); }
static inline void __sev() {}
