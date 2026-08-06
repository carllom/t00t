#pragma once

#include <cstdint>

static constexpr uint32_t SAMPLE_RATE = 44100;

// Override with -DT00T_SAMPLES_PER_BUFFER=<n> (see CMakeLists.txt's
// DMA_BUFFER_SIZE / Makefile's DMA_BUFFER_SIZE=) -- #16's IRQ-overhead vs
// latency measurement, 256 (default) vs 512.
#ifndef T00T_SAMPLES_PER_BUFFER
#define T00T_SAMPLES_PER_BUFFER 256
#endif
static constexpr uint32_t SAMPLES_PER_BUFFER = T00T_SAMPLES_PER_BUFFER;
