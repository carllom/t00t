# T00T Audio Engine Architecture

## Overview

Dual-core architecture on RP2040:
- **Core 0**: Control plane — input polling, event processing, parameter management
- **Core 1**: Audio plane — synthesis, mixing, buffer filling

## Pin Allocation

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | Button A | Shared with VGA color, active-high, pull-down |
| 1 | *free* | VGA color (unused) |
| 2 | Profile pin | Synthesis workload measurement (directly probe D-sub pin 1 / Red LSB) |
| 3-4 | *free* | VGA color (unused) |
| 5 | SD CLK | |
| 6 | Button B | Shared with VGA color |
| 7-10 | *free* | VGA color (unused) |
| 11 | Button C | Shared with VGA color |
| 12-15 | *free* | VGA color (unused) |
| 16-17 | VGA sync | Unused |
| 18-22 | SD / UART | |
| 26 | I2S DATA | PCM5100A DIN |
| 27 | I2S BCK | PCM5100A bit clock |
| 28 | I2S LRCK | PCM5100A word select |

## Audio Buffer Flow

Two raw stereo `int16_t` buffers, managed directly (no pico-extras producer/consumer pools).

```
Buffer A ◄──── DMA playing ────► PIO I2S ──► DAC
Buffer B ◄──── Core 1 filling

DMA IRQ fires (buffer A done):
  → DMA starts on buffer B (already filled)
  → Signal Core 1 via multicore FIFO: "fill buffer A"

Core 1 wakes:
  → Reads committed voice params
  → Renders all voices into buffer A
  → Marks buffer A ready
  → Sleeps (WFE / FIFO wait)
```

Buffer size: 256 stereo samples = 512 × int16_t = 1024 bytes each.
Latency: 256 / 44100 ≈ 5.8ms per buffer, ~11.6ms total pipeline.

## I2S Output (Direct DMA)

Bypasses pico-extras `audio_i2s_connect()` and its internal consumer pool.
We configure DMA + PIO ourselves:

- PIO SM loaded with `audio_i2s` program (from pico-extras .pio file)
- DMA channel transfers from buffer → PIO TX FIFO, DREQ-paced
- DMA IRQ on completion: swap buffers, restart DMA, signal Core 1
- Still uses pico-extras PIO program for I2S bit-banging

## Core 1 — Audio Engine

Runs on Core 1 exclusively. Entry point: `audio_engine_run()` (never returns).

```
audio_engine_run():
  loop:
    wait for FIFO message (buffer index to fill)
    set profiling GPIO high
    read committed voice params (atomic snapshot)
    clear mix scratch buffer
    for each active voice:
      render into scratch (wavetable + phase accumulator)
    clip scratch → int16_t stereo interleaved into target buffer
    clear profiling GPIO
    signal buffer ready
```

Voice state (phase accumulators) lives on Core 1 only.
Voice parameters (freq, amplitude, active) come from Core 0 via the double-buffered param block.

## Core 0 — Control Plane

Wakes on a 1ms hardware timer alarm (repeating). Main loop:

```
core0_main():
  init hardware, start Core 1
  loop:
    WFE (sleep until timer alarm)
    poll buttons → debounce → edge detect
    (future: drain MIDI UART ring buffer)
    process events → update voice parameter shadow
    commit shadow → flip param double-buffer (atomic flag)
    housekeeping / debug (low priority)
```

## Voice Parameter Double Buffer

Core 0 writes to a shadow copy. On commit, it flips an atomic index.
Core 1 reads from the committed copy at the start of each render pass.

```c
struct VoiceParams {
    uint32_t phase_inc;   // pre-computed by Core 0
    int16_t  amplitude;
    bool     active;
};

struct VoiceParamBlock {
    VoiceParams voices[MAX_VOICES];
};

// Two copies, atomic flip
VoiceParamBlock param_blocks[2];
volatile uint8_t committed_index;  // 0 or 1, written by Core 0

// Core 0 writes to param_blocks[1 - committed_index] (shadow)
// Core 0 commits: committed_index = 1 - committed_index (with __sev())
// Core 1 reads: param_blocks[committed_index] at start of render
```

No locks needed: Core 0 only writes the shadow (non-committed) block.
Core 1 only reads the committed block. The flip is a single byte write (atomic on Cortex-M0+).

## Profiling GPIO

GPIO 2 directly on the VGA header. High during synthesis, low while idle.
Duty cycle visible on oscilloscope = CPU utilization of audio engine.

## Debounce

Simple integrator debounce at 1ms tick rate:
- Counter per button, incremented when pressed, decremented when released
- Threshold at ~10ms (10 ticks) for state change
- Edge detection on debounced state transitions

## Trigger/Gate Signaling

Replaces the old `active` bool in VoiceParams. Core 0 only writes, Core 1 only reads.

```c
struct VoiceParams {
    uint32_t phase_inc;
    int16_t  amplitude;   // base amplitude (velocity)
    uint8_t  trigger;     // generation counter, incremented on each note-on
    bool     gate;        // true while key held, false on release
};
```

Core 1 keeps `last_trigger[v]` per voice. Detection logic:
- `trigger != last_trigger && gate` → new note: reset phase, start ADSR attack
- `!gate && was_gated` → release: transition ADSR to release phase
- `trigger == last_trigger && gate` → sustain: no change

This handles re-triggers cleanly: rapid off→on produces a new trigger value,
which Core 1 detects even if it missed the intermediate gate=false.

## ADSR Envelope

Per-voice state machine on Core 1. Envelope level: 0–32767 (15-bit fixed-point).

```
States: IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE

IDLE:     level = 0, voice silent
ATTACK:   level += attack_rate each sample, until level >= 32767
DECAY:    level -= decay_rate each sample, until level <= sustain_level
SUSTAIN:  level = sustain_level, held while gate is true
RELEASE:  level -= release_rate each sample, until level <= 0 → IDLE
```

Release from any state (attack/decay/sustain) transitions to RELEASE
using the current level as starting point.

Amplitude chain per sample:
```
raw_osc = oscillator output          [-32767 .. 32767]
scaled  = (raw_osc * amplitude) >> 15   [-32767 .. 32767]
final   = (scaled * env_level) >> 15    [-32767 .. 32767]
```

Initial hardcoded ADSR values (Task 1):
- Attack:  10ms  → rate = 32767 / 441 ≈ 74/sample
- Decay:   100ms → rate = (32767 - sustain) / 4410 ≈ 2/sample
- Sustain: 70%   → level = 22937
- Release: 200ms → rate = 22937 / 8820 ≈ 3/sample

## LFO

Per-voice LFO on Core 1, driven by its own phase accumulator (same 22.10
fixed-point as oscillator). Reads from sine_table for smooth modulation.

Two destinations (selected per-voice):
- **Amplitude (tremolo)**: multiplies envelope output
- **Pitch (vibrato)**: offsets phase_inc per sample

LFO params in VoiceParams: `lfo_rate` (phase_inc), `lfo_depth` (modulation amount).
LFO phase state on Core 1 only.

## Waveform Types

```c
enum Waveform : uint8_t { WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE };
```

All derived from the phase accumulator (no extra tables needed except sine):
- **Sine**: wavetable lookup with linear interpolation (existing)
- **Square**: sign of phase, with variable duty cycle
- **Triangle**: fold phase into ramp-up/ramp-down
- **Saw**: phase directly scaled to [-32767..32767]
- **Noise**: LFSR (16-bit Galois), clocked at oscillator frequency

Waveform type stored in VoiceParams, selected by Core 0.

## Implementation Tasks

### Task 1: Trigger/gate + ADSR envelope
- Replace `active` with `gate`+`trigger` in VoiceParams
- Update controller.cpp for gate/trigger signaling
- Add ADSR state machine to audio_engine.cpp
- Hardcoded ADSR rates, envelope modulates amplitude
- **Test**: buttons produce notes with attack ramp and release tail

### Task 2: Square waveform
- Add `Waveform` enum and `waveform` field to VoiceParams
- Implement square wave in audio_engine.cpp (sign of phase)
- Assign different waveforms to buttons for testing
- **Test**: sine on A, square on B, hear difference

### Task 3: LFO → amplitude (tremolo)
- Add `lfo_rate`, `lfo_depth` fields to VoiceParams
- Add LFO phase accumulator per voice on Core 1
- LFO modulates post-envelope amplitude
- **Test**: one button with tremolo, others without

### Task 4: More waveforms + duty cycle
- Implement triangle, saw, noise (LFSR)
- Add `duty_cycle` field to VoiceParams for square wave
- **Test**: cycle through waveforms on different buttons

### Task 5: LFO → pitch (vibrato)
- Add LFO destination selector to VoiceParams
- LFO offsets phase_inc per sample when targeting pitch
- **Test**: vibrato on one button, tremolo on another

## Event Queue

Simple fixed-size ring buffer of `ControlMessage`, single-producer (Core 0) single-consumer (Core 0).
Events generated by button edges (and future MIDI parser).
Consumed in the same tick to update the voice parameter shadow.
