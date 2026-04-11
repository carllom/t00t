#pragma once

#include "output.h"
#include "engine.h"

// Core 1 entry point. Initializes synthesis state, then loops forever
// waiting for DMA buffer-fill requests via multicore FIFO.
// Never returns.
void audio_engine_run(AudioBuffers *buffers, ParamExchange *params);
