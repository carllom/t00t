# T00T — Subtractive Engine

The original synthesis engine, predating the module system. See `engine.md`
for the shared dual-core architecture, pin allocation, and IPC every module
(including this one) is built on; `history_subtractive.md` for development
history and full performance measurements.

## Overview

General-purpose subtractive synthesizer — one ADSR + LFO + filter voice per
note, with optional PCM sample playback in place of the synthesized
oscillator.

### Specifications

- **Voices**: 16, dynamically allocated (steal policy: silent > released >
  oldest active — see `engine.md`)
- **Oscillators**: 8 waveform types — sine, square, triangle, saw, noise,
  band-limited (PolyBLEP) square, band-limited saw, and PCM sample playback
- **Envelope**: 1 ADSR per voice (linear attack, exponential decay/release)
- **LFO**: 1 general-purpose LFO per voice (4 destinations: amplitude, pitch,
  duty cycle, filter cutoff) plus 1 dedicated mod-wheel vibrato LFO
- **Filter**: 1 state-variable filter per voice (lowpass/bandpass/highpass/
  notch/off)
- **Effects**: 1 shared post-mix insert (delay or reverb, mono send / stereo
  return) — global, not per-voice
- **Presets**: 11 factory presets (3 synthesized, 8 sample-based) — see
  `presets.h`
- No arpeggiator or step sequencer in the engine itself; the VGA board's
  buttons each play one fixed note (see MIDI mapping below)

### MIDI Mapping (Input Capabilities)

| Message | Name | Range | Function |
|---|---|---|---|
| Note On/Off | — | notes 0–127, all channels | 16 voices, dynamically allocated |
| Velocity | — | 0–127 | linear to amplitude |
| Pitch Bend | — | 14-bit | ±2 semitones |
| CC1 | Mod Wheel | 0–127 | dedicated vibrato LFO depth |
| CC10 | Pan | 0–127 | live per-voice stereo pan — subtractive is the only module that maps this CC; not stored in presets |
| CC72 | FX Param 1 | 0–127 | delay feedback / reverb room size |
| CC73 | FX Mix | 0–127 | wet/dry mix (global) |
| CC74 | FX Type | 0–127 | select, splits range into the 3 FX bands |
| CC75 | FX Param 2 | 0–127 | delay time / reverb damping |
| CC0 | Bank Select MSB | 0–127 | selects the microKORG program bank, used with Program Change |
| CC32 | Bank Select LSB | 0–127 | captured, currently unused |
| Program Change | — | 0–127 | microKORG numbering (tens digit = row, ones digit = column) mapped to one of the 11 factory presets; affects future notes only |

Non-MIDI input: VGA board buttons (A/B/C, `vgaboard_rp2350` only) — each
fixed to one note/channel/preset, routed through the same Shaping →
`input_dispatch()` → `set_note` path a MIDI note-on/off uses (see
`engine.md`'s MIDI Input section).

### Display (Presentation Capabilities)

`breadboard_rp2350`'s optional LCD shows (`display.cpp`, ~20 Hz refresh,
change-detected redraws only):

- Per-voice dot bar (16 voices: filled = sounding, bordered = key held) plus
  sounding count
- CPU load (%, colour-coded bar)
- Last note (name + octave + velocity)
- Current preset name
- Pitch bend and mod wheel values
- FX type, its two params, and mix

## Technical Overview

### Source Layout

- `engine.h` — `VoiceParams`/`ParamExchange` (via `engine_base.h`'s templates)
- `presets.h` — `VoicePreset` struct and the 11-entry factory preset table
- `audio_engine.cpp` — Core 1 render loop
- `display.cpp` — Core 0 status display
- `input_subsystem.cpp` — the Input pipeline's module-specific tail:
  mapping table, Handlers, and Voice Allocation Interface calls, built on
  the shared dispatch layer below

Also draws on shared, non-engine-specific code: `src/controller.cpp`
(VGA-board buttons — subtractive is the only engine that links it, see
`CMakeLists.txt`), `src/button_shaping.h`/`src/sensor_event.h` (button
SensorEvent → Input event Shaping), and `src/midi/midi_dispatch.h`/`.cpp`/
`midi_controller_generic.h` (shared generic per-MIDI-message-type dispatch
helpers, bank-select state, and the full parse-and-dispatch loop — also
used by `fm`).

### Build

Default engine — `make` alone builds it (equivalent to
`make ENGINE=subtractive`). No subtractive-specific build flags. See
`building.md` for board selection, MIDI transport overrides, and flashing.

### Tools

No dedicated tools for this module.

## Architecture

### ADSR Envelope

Per-voice state machine on Core 1. Envelope `level` is a float in 0.0–1.0,
converted to Q15 (`level * 32767`) inside the render loop. Attack is **linear**
(additive per sample); decay and release are **exponential** (multiplicative
coefficient per sample) for natural-sounding amplitude curves.

```
States: IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE

IDLE:     level = 0, voice silent
ATTACK:   level += attack_rate, until level >= 1.0 → DECAY
DECAY:    level = sustain + (level - sustain) * decay_coeff,
          until within epsilon of sustain_level → SUSTAIN
SUSTAIN:  level = sustain_level, held while gate is true
RELEASE:  level *= release_coeff, until level < epsilon → IDLE
```

`EnvConfig` holds `attack_rate`, `decay_coeff`, `sustain_level`,
`release_coeff`, and `gated_attack_decay` (default `true`), built from
milliseconds via `env_config(attack_ms, decay_ms, sustain_pct, release_ms)`.
`Envelope` exposes `init()`, `trigger()`, `release(cfg)`, `active()`, and
`advance(cfg)` (returns the current float level). With `gated_attack_decay`
true — the only value any preset here uses today — release from any active
state transitions to RELEASE immediately, using the current level as the
starting point. Set false, a release requested during ATTACK or DECAY is
deferred until they finish naturally instead of interrupting them.

Amplitude chain per sample (Core 1 render loop):
```
env_f = envelope.advance(cfg)          // float 0.0–1.0
level = env_f * 32767                   // Q15
if waveform == WAVE_SAMPLE: raw = osc_sample_play(sample, phase)
else:                       raw = osc_sample(waveform, phase, duty, lfsr, phase_inc)
scaled = (raw * amplitude) >> 15
scaled = (scaled * level) >> 15
if lfo_depth > 0:                       // tremolo, depth pre-converted to Q15
    mod = 32767 - lfo_depth + (lfo_val * lfo_depth) >> 15
    scaled = (scaled * mod) >> 15
if filter_mode != OFF:
    cutoff = base + (level * env_amount) >> 15 + (lfo_val * lfo_filter_depth) >> 15
    F_half = svf_compute_f_half(cutoff)
    scaled = filter.tick(scaled, F_half, Q_q15, mode)
```

The actual Core 1 loop computes `env_f`, the LFO value, and `F_half` once per
64-sample sub-block (`SUBBLOCK` in `audio_engine.cpp`) and linearly ramps each
toward its next target across the block, rather than recomputing them fresh
every sample as shown above — the oscillator phase advance, PolyBLEP
correction, and the filter's own two-pass state update stay genuinely
per-sample.

Current ADSR values:
- Attack:  10ms
- Decay:   100ms
- Sustain: 70%
- Release: 800ms

### LFO

Per-voice LFO on Core 1, driven by a float phase accumulator in [0.0, 1.0)
advanced by `lfo_rate / SAMPLE_RATE` each sample. The phase is scaled to the
fixed-point range and read from `sine_table` (`osc_sine`) for a smooth Q15 value.
Single LFO per voice with independent depth controls for four destinations:

- **Amplitude (tremolo)**: `lfo_depth` — multiplies post-envelope amplitude
- **Pitch (vibrato)**: `lfo_pitch_depth` — offsets `phase_inc` by ±fraction (0.05 ≈ ±1 semitone)
- **Duty cycle (PWM)**: `lfo_pwm_depth` — sweeps duty_cycle ± around center, clamped 1–1022
- **Filter cutoff**: `lfo_filter_depth` — offsets cutoff in Hz (signed)

LFO params in VoiceParams: `lfo_rate` (Hz, shared) plus four depth fields.
`lfo_rate`, `lfo_depth`, `lfo_pitch_depth`, and `lfo_pwm_depth` are floats; the
inner loop converts the depths to Q15 once per buffer. LFO phase state lives on
Core 1 only and is reset to 0 on trigger.

#### Mod-wheel vibrato (dedicated LFO)

Separate from the preset LFO above, each voice has a second, dedicated vibrato
LFO for the MIDI mod wheel: a fixed 5 Hz (`MOD_VIBRATO_HZ`) sine that modulates
pitch by up to ~±50 cents (`MOD_VIBRATO_MAX_Q15`) at full wheel. Its depth comes
from `VoiceParams::mod_depth` (Q15, 0 = off), a live control set by CC1 — it is
not part of a preset (`voice_apply_preset()` resets it to 0). It stacks on top of
any preset pitch LFO and runs from its own `mod_lfo_phase[v]` accumulator, also
reset to 0 on trigger.

### Waveform Types

```c
enum Waveform : uint8_t {
    WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE,
    WAVE_SQUARE_BLEP, WAVE_SAW_BLEP, WAVE_SAMPLE
};
```

The synthesized waveforms are derived from the phase accumulator (no extra
tables needed except sine); `WAVE_SAMPLE` plays back PCM data instead:
- **Sine**: wavetable lookup with linear interpolation (1024-entry table)
- **Square**: sign of phase, with variable duty cycle (0–1023)
- **Triangle**: piecewise linear, 4-quarter ramp
- **Saw**: phase directly scaled to [-32767..32767]
- **Noise**: 16-bit Galois LFSR (polynomial 0xB400), per-voice state, reseeded on trigger
- **Square BLEP**: band-limited square via PolyBLEP correction at both edges
- **Saw BLEP**: band-limited saw via PolyBLEP correction at wrap point
- **Sample**: PCM playback from a `SampleDef` (signed int8 data shifted to Q15),
  linearly interpolated, with optional looping; the phase advances at a
  resampling rate derived from the target vs. base frequency. Dispatched
  separately via `osc_sample_play()` / `osc_sample_advance_phase()`.

PolyBLEP smooths discontinuities over one sample on each side using a quadratic
polynomial residual. Fixed-point Q10 arithmetic, uses RP2350 hardware divider.
The naive (non-BLEP) variants are kept for intentionally aliased/"crusty" sound.

### State-Variable Filter (SVF)

Per-voice SID-style 2-pole (12dB/octave) multimode filter. Produces lowpass,
bandpass, highpass, and notch outputs from shared state variables.

```c
enum FilterMode : uint8_t { FILTER_OFF, FILTER_LP, FILTER_BP, FILTER_HP, FILTER_NOTCH };
```

Implementation: fixed-point SVF with 2-pass integration for stability.
The filter itself is all integer arithmetic. On the RP2350 (Cortex-M33),
`SMULL` provides 64-bit intermediates, so the multiplies use `int64_t` and
no intermediate clamping is needed. State (`lp`, `bp`) lives in `SVFilter`
with `init()` and `tick(input, F_half, Q_q15, mode)`.

#### Per-sample update (2-pass)
```
for pass in 0..1:
    hp = input - lp - (Q_q15 * bp) >> 15
    bp += (F_half * hp) >> 15
    lp += (F_half * bp) >> 15
output = lp | bp | hp | lp+hp depending on mode (input if OFF)
```

#### Coefficient computation
- **F_half** (Q15): `cutoff_hz * 76539 >> 15`, clamped [33, 15564]
  - Approximation of `π * cutoff / sample_rate`, <0.1% error
- **Q** (Q15): `65534 - (resonance << 1)`, clamped to a minimum of 2
  - resonance=0 → Q=65534 (2.0, no resonance)
  - resonance=32767 → Q≈0 (near self-oscillation)

#### Modulation
Per-sample cutoff = base_cutoff + (envelope × env_amount >> 15) + (LFO × lfo_filter_depth >> 15),
clamped 20–18000 Hz. Q is constant per buffer.

Filter state (lp, bp) reset to 0 on voice trigger for clean attacks.

## Status and Plan

### Performance

Idle ~0.6%. One voice ~5–6% (more with LFO and filter both active). Delay
insert adds ~1.5pp; reverb adds ~8pp. Measured on breadboard_rp2350 at
44.1 kHz / 150 MHz. Full measurement history: `history_subtractive.md`.

## Decision Record

1. **Attack is linear, decay/release are exponential** — matches how
   amplitude envelopes are perceived; a linear decay/release sounds
   unnaturally abrupt.
2. **Envelope, LFO, and filter coefficient are computed once per 64-sample
   sub-block and linearly ramped**, rather than recomputed every sample —
   cuts control-rate cost without an audible behavior change; oscillator
   phase, PolyBLEP correction, and the filter's own per-sample state update
   are unaffected.
3. **Naive (non-BLEP) waveforms are kept alongside their PolyBLEP-corrected
   counterparts** — the aliased versions are an intentional "crusty" sound
   option, not legacy code left behind.
4. **Mod-wheel vibrato is a separate, dedicated LFO**, not a shared instance
   with the preset LFO — a live control (CC1) has to layer independently on
   top of whatever pitch-LFO a preset already programs, and stay at zero
   depth (off) by default regardless of preset.
5. **The SVF uses two-pass integration in fixed point**, with `int64_t`
   intermediates (`SMULL` on the M33) — needed for stability at high cutoff /
   low Q; a single pass is not.
6. **CC10 pan is a live control, not part of stored presets** —
   `voice_apply_preset()` always resets it to center, matching mod-wheel
   depth's treatment.
7. **Program Change uses microKORG-specific numbering** (row = tens digit,
   column = ones digit) rather than a linear 0–127 map — matches the specific
   external controller this engine was built to pair with.
8. **Each VGA board button plays one fixed note**, not the old per-button
   4-note cycle — a bare on/off switch has no state to key a cycle position
   off other than the button itself, and a fixed note keeps the button's
   Shaping config (note, channel, velocity) a plain, self-contained value
   rather than mutable per-button state threaded through the input path.
9. **Program Change now routes through the Router** (`CONFIGURATION`,
   `src/midi/midi_dispatch.h`'s shared synthetic id) instead of bypassing
   the dispatch table directly — the microKORG row/col decoding stays
   entirely inside this module's own patch-select Handler, reading the
   bank value via `midi_channel_bank_msb()` (shared, module-agnostic
   plumbing) rather than a locally duplicated array.
10. **Voice allocation lives inside `set_note()`, not the dispatch loop**
    — `voice_alloc_allocate()`/`release()` are the Voice Allocation
    Interface (CONTEXT.md), reached from a Handler, never interleaved in
    MIDI parsing/dispatch. `set_note()` resolves its own voice via its own
    `note_voice[]` lookup (steal-on-retrigger included); the GPIO
    button path (`controller.cpp`) reaches the same Handler and no longer
    pre-allocates a voice of its own.

## Glossary

- **PolyBLEP**: Polynomial Band-Limited Step — a correction added at a
  waveform's discontinuity to suppress aliasing, without full oversampling.
- **SVF**: State-Variable Filter — a filter topology that produces
  lowpass/bandpass/highpass simultaneously from the same per-sample state.
- **Q15**: fixed-point format with 15 fractional bits (range roughly
  -1.0..1.0 as a 16-bit signed integer).
- **Sub-block**: a fixed-size chunk of samples (64 here) within one audio
  buffer, used as the unit at which slowly-changing parameters (envelope,
  LFO, filter coefficient) are recomputed and then ramped across, rather
  than recomputed every sample.
