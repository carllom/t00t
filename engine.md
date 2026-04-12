# T00T Audio Engine Architecture

## Overview

Dual-core architecture on RP2040:
- **Core 0**: Control plane — input polling, event processing, voice allocation, parameter management
- **Core 1**: Audio plane — synthesis, mixing, buffer filling, active-voice bitmap feedback

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
  init: profiling pin, envelope config, sine wavetable
  loop:
    wait for FIFO message (buffer index to fill)
    set profiling GPIO high
    read committed voice params (atomic snapshot)
    clear mix scratch buffer
    for each voice (0..MAX_VOICES-1):
      detect trigger/gate changes → envelope trigger/release
      skip if envelope idle
      per-sample inner loop:
        advance envelope
        compute LFO (single sine LFO per voice)
        apply LFO → pitch (vibrato), duty cycle (PWM), amplitude (tremolo)
        oscillator sample (dispatch by waveform type)
        amplitude chain: osc × velocity × envelope × LFO tremolo
        SVF filter (if enabled): modulate cutoff from envelope + LFO, tick
        accumulate into scratch
    clip scratch → int16_t stereo interleave into target buffer
    clear profiling GPIO
    push active-voice bitmap to Core 0 via reverse FIFO
```

Voice state (phase accumulators, LFO phase, LFSR, envelopes) lives on Core 1 only.
Voice parameters come from Core 0 via the double-buffered param block.

## Core 0 — Control Plane

Wakes on a 1ms hardware timer alarm (repeating). Main loop:

```
core0_main():
  init hardware, voice allocator, start Core 1
  loop:
    WFE (sleep until timer alarm)
    voice_alloc_update() — drain reverse FIFO for active bitmap
    poll buttons → debounce → edge detect
    on press: voice_alloc_allocate() → write params → trigger
    on release: gate off → voice_alloc_release()
    (future: drain MIDI UART ring buffer)
    commit shadow → flip param double-buffer (atomic flag)
```

## Voice Parameter Double Buffer

Core 0 writes to a shadow copy. On commit, it flips an atomic index.
Core 1 reads from the committed copy at the start of each render pass.

```c
struct VoiceParams {
    uint32_t phase_inc;        // fixed-point phase increment (pre-computed by Core 0)
    int16_t  amplitude;        // base amplitude / velocity (0–32767)
    uint8_t  trigger;          // generation counter, incremented on each note-on
    bool     gate;             // true while key held, false on release
    Waveform waveform;         // oscillator waveform type
    uint16_t duty_cycle;       // duty cycle for square wave (0–1023, 512 = 50%)
    uint32_t lfo_rate;         // LFO phase increment (same 22.10 format, 0 = off)
    int16_t  lfo_depth;        // LFO → amplitude depth (0–32767, 0 = off)
    int16_t  lfo_pitch_depth;  // LFO → pitch depth (0–32767, 0 = off)
    int16_t  lfo_pwm_depth;    // LFO → duty cycle depth (0–512, 0 = off)
    FilterMode filter_mode;    // LP, BP, HP, notch, or off (bypass)
    uint16_t filter_cutoff;    // base cutoff in Hz (20–18000)
    uint16_t filter_resonance; // resonance 0–32767 (0 = none, 32767 = self-oscillation)
    int16_t  filter_env_amount;// envelope → cutoff in Hz (signed, ±18000)
    int16_t  lfo_filter_depth; // LFO → cutoff in Hz (signed, ±18000)
};

struct VoiceParamBlock {
    VoiceParams voices[MAX_VOICES];  // MAX_VOICES = 16
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

Core 0 only writes, Core 1 only reads. `trigger` is a generation counter
(uint8_t, wraps), `gate` is a bool.

Core 1 keeps `last_trigger[v]` per voice. Detection logic:
- `trigger != last_trigger` → new note: reset phase + LFO + LFSR, start ADSR attack
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
raw_osc = osc_sample(waveform, phase, duty_cycle, lfsr, phase_inc)
scaled  = (raw_osc * amplitude) >> 15
scaled  = (scaled * env_level) >> 15
if lfo_depth > 0:
    mod = 32767 - lfo_depth + (lfo_val * lfo_depth) >> 15
    scaled = (scaled * mod) >> 15
if filter_mode != OFF:
    cutoff = base + (env_level * env_amount) >> 15 + (lfo_val * lfo_depth) >> 15
    F_half = svf_compute_f_half(cutoff)
    scaled = filter.tick(scaled, F_half, Q_q13, mode)
```

Current ADSR values:
- Attack:  10ms
- Decay:   100ms
- Sustain: 70%
- Release: 800ms

## LFO

Per-voice LFO on Core 1, driven by its own phase accumulator (same 22.10
fixed-point as oscillator). Reads from sine_table for smooth modulation.
Single LFO per voice with independent depth controls for three destinations:

- **Amplitude (tremolo)**: `lfo_depth` — multiplies post-envelope amplitude
- **Pitch (vibrato)**: `lfo_pitch_depth` — offsets phase_inc by ±fraction per sample (1638 ≈ ±1 semitone)
- **Duty cycle (PWM)**: `lfo_pwm_depth` — sweeps duty_cycle ± around center, clamped 1–1022
- **Filter cutoff**: `lfo_filter_depth` — offsets cutoff in Hz (signed)

LFO params in VoiceParams: `lfo_rate` (phase_inc, shared), four depth fields.
LFO phase state on Core 1 only. Reset to 0 on trigger.

## Waveform Types

```c
enum Waveform : uint8_t {
    WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE,
    WAVE_SQUARE_BLEP, WAVE_SAW_BLEP
};
```

All derived from the phase accumulator (no extra tables needed except sine):
- **Sine**: wavetable lookup with linear interpolation (1024-entry table)
- **Square**: sign of phase, with variable duty cycle (0–1023)
- **Triangle**: piecewise linear, 4-quarter ramp
- **Saw**: phase directly scaled to [-32767..32767]
- **Noise**: 16-bit Galois LFSR (polynomial 0xB400), per-voice state, reseeded on trigger
- **Square BLEP**: band-limited square via PolyBLEP correction at both edges
- **Saw BLEP**: band-limited saw via PolyBLEP correction at wrap point

PolyBLEP smooths discontinuities over one sample on each side using a quadratic
polynomial residual. Fixed-point Q10 arithmetic, uses RP2040 hardware divider.
The naive (non-BLEP) variants are kept for intentionally aliased/"crusty" sound.

## State-Variable Filter (SVF)

Per-voice SID-style 2-pole (12dB/octave) multimode filter. Produces lowpass,
bandpass, highpass, and notch outputs from shared state variables.

```c
enum FilterMode : uint8_t { FILTER_OFF, FILTER_LP, FILTER_BP, FILTER_HP, FILTER_NOTCH };
```

Implementation: fixed-point SVF with 2-pass integration for stability.
All integer arithmetic — no floats in the render path.

### Per-sample update (2-pass)
```
for pass in 0..1:
    hp = input - lp - (Q_q13 * bp) >> 13
    bp += (F_half * hp) >> 15
    lp += (F_half * bp) >> 15
clamp bp, lp to ±32767 (soft saturation)
output = lp | bp | hp | lp+hp depending on mode
```

### Coefficient computation
- **F_half** (Q15): `cutoff_hz * 76539 >> 15`, clamped [33, 15564]
  - Approximation of `π * cutoff / sample_rate`, <0.1% error
- **Q** (Q13): `16384 - (resonance >> 1)`
  - resonance=0 → Q=16384 (2.0, no resonance)
  - resonance=32767 → Q≈1 (near self-oscillation)

### Modulation
Per-sample cutoff = base_cutoff + (envelope × env_amount >> 15) + (LFO × lfo_filter_depth >> 15),
clamped 20–18000 Hz. Q is constant per buffer.

Filter state (lp, bp) reset to 0 on voice trigger for clean attacks.

### CPU cost
~18 integer ops per sample per voice (8 multiplies, shifts, adds).
Estimated ~1-2% per voice on profiling pin.

## Dynamic Voice Allocation

16-voice polyphonic allocator on Core 0. Core 1 provides feedback via a
16-bit active-voice bitmap pushed through the reverse multicore FIFO
(non-blocking, Core 1 never stalls).

Core 0 drains the FIFO at the start of each tick, keeps latest bitmap as
`active_mask`, copies to `local_mask` (working copy — newly allocated voices
get their bit set immediately to prevent double-allocation within one tick).

### Allocation priority
1. **Silent** — `local_mask` bit clear AND not gated → envelope finished, free slot
2. **Released** — `local_mask` bit set AND not gated → in release phase, quiet steal
3. **Oldest active** — `local_mask` bit set AND gated → audible steal, least bad

Age tracking uses a uint8_t monotonic counter (incremented per allocation).
Modular-arithmetic comparison `(int8_t)(a - b) < 0` handles wrap correctly;
safe with ≤16 concurrent voices (gap never exceeds 128).

### Button behavior
Each button cycles through a table of 4 notes on successive presses.
Voice is allocated on press, released on release. Long release (800ms)
ensures multiple voices can be heard simultaneously.

## Performance

Measured duty cycles on GPIO 2 profiling pin:

- Idle: 0.85%
- Single voice, no LFO: 2-3%
- Single voice w. LFO: 5-6%
- 16 voice max usage (unreliable measurement): ~75%
- <16 voice normal usage (unreliable measurement): 50%

### Baseline before RP2350

This is the baseline measurements of the state before switching to RP2350 and upgrading the core:

- Idle: 0.81%
- Voice A: "Fairlight" sample (8-bit converted on the fly to Q15): 10.3%
- Voice B: Square wave (BLEP) with 3Hz LFO for duty cycle + Filter with Q envelope: 12.3%
- Voice C: Triangle with LFO controlling amplitude + Filter Q: 10.5%
- All 3 voices sustaining: 31.5%
- Intense work will overload the buffer. Moderate use will get it close to 100%

## Event Queue

Simple fixed-size ring buffer of `ControlMessage`, single-producer (Core 0) single-consumer (Core 0).
Events generated by button edges (and future MIDI parser).
Consumed in the same tick to update the voice parameter shadow.
