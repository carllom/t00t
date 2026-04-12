#pragma once

#include "audio_common.h"
#include "hardware/sync.h"
#include <cstdint>

static constexpr uint32_t MAX_VOICES = 16;

// Profiling pin — GPIO 2, routed to VGA D-sub pin 1 (Red LSB)
static constexpr uint32_t PROFILE_PIN = 2;

enum Waveform : uint8_t { WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE, WAVE_SQUARE_BLEP, WAVE_SAW_BLEP };

enum FilterMode : uint8_t { FILTER_OFF, FILTER_LP, FILTER_BP, FILTER_HP, FILTER_NOTCH };

// Voice parameters: written by Core 0, read by Core 1.
// Only contains values needed for synthesis — no phase state.
struct VoiceParams {
    uint32_t phase_inc;  // fixed-point phase increment (pre-computed by Core 0)
    int16_t amplitude;   // base amplitude / velocity (0–32767)
    uint8_t trigger;     // generation counter, incremented on each note-on
    bool gate;           // true while key held, false on release
    Waveform waveform;   // oscillator waveform type
    uint16_t duty_cycle;  // duty cycle for square wave (0–1023, 512 = 50%)
    uint32_t lfo_rate;   // LFO phase increment (same 22.10 format, 0 = off)
    int16_t lfo_depth;   // LFO → amplitude depth (0–32767, 0 = off)
    int16_t lfo_pitch_depth; // LFO → pitch depth (0–32767, 0 = off, 1638 ≈ ±1 semitone)
    int16_t lfo_pwm_depth;   // LFO → duty cycle depth (0–512, 0 = off)
    // Filter
    FilterMode filter_mode;    // filter type (OFF = bypass)
    uint16_t filter_cutoff;    // base cutoff in Hz (20–18000)
    uint16_t filter_resonance; // resonance 0–32767 (0 = none, 32767 = self-oscillation)
    int16_t filter_env_amount; // envelope → cutoff in Hz (signed, ±18000)
    int16_t lfo_filter_depth;  // LFO → cutoff in Hz (signed, ±18000)
};

// A complete snapshot of all voice parameters for one render pass.
struct VoiceParamBlock {
    VoiceParams voices[MAX_VOICES];
};

// Double-buffered parameter exchange between Core 0 and Core 1.
//
// Core 0 writes to the shadow block: param_blocks[1 - committed]
// Core 0 commits by flipping committed (single byte, atomic on M0+)
// Core 1 reads param_blocks[committed] at the start of each render pass.
//
// No locks: Core 0 never touches the committed block, Core 1 never
// touches the shadow block. The index flip is a single-byte store.
struct ParamExchange {
    VoiceParamBlock blocks[2];
    volatile uint8_t committed;  // 0 or 1

    void init() {
        committed = 0;
        for (int b = 0; b < 2; b++) {
            for (uint32_t v = 0; v < MAX_VOICES; v++) {
                blocks[b].voices[v] = { 0, 0, 0, false, WAVE_SINE, 512, 0, 0, 0, 0,
                                        FILTER_OFF, 8000, 0, 0, 0 };
            }
        }
    }

    // Core 0: get the shadow block to write into
    VoiceParamBlock &shadow() {
        return blocks[1 - committed];
    }

    // Core 0: make the shadow visible to Core 1
    void commit() {
        __compiler_memory_barrier();
        committed = 1 - committed;
        __sev();  // wake Core 1 if it's in WFE
    }

    // Core 1: get the currently committed block to read from
    const VoiceParamBlock &active() const {
        return blocks[committed];
    }
};
