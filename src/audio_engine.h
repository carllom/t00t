#pragma once

#include "output.h"
#include "engine.h"

// Core 1 entry point. Initializes synthesis state, then loops forever
// waiting for DMA buffer-fill requests via multicore FIFO.
// Never returns.
void audio_engine_run(AudioBuffers *buffers, ParamExchange *params);

// --- Telemetry, published by Core 1 for a Core 0 display/UI (read-only) ---
// Single-word reads, atomic on the M33 — no locking needed for a diagnostic.

// Bitmap of voices whose envelope is currently active (bit v = voice v).
uint16_t audio_engine_active_mask();

// Smoothed Core 1 render load as a percentage of the audio buffer period
// (0–100). Mirrors the PROFILE_PIN duty cycle.
uint8_t audio_engine_load();
