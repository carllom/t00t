# T00T — Subtractive Engine

The original synthesis engine, predating the module system — this doc covers
what's specific to it (envelope, LFO, waveforms, filter). See `engine.md` for
the shared dual-core architecture, pin allocation, and IPC every module
(including this one) is built on. Development history (baseline measurements,
the RP2350 performance-gain table) is in `history_subtractive.md`.

## ADSR Envelope

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

`EnvConfig` holds `attack_rate`, `decay_coeff`, `sustain_level`, `release_coeff`,
built from milliseconds via `env_config(attack_ms, decay_ms, sustain_pct, release_ms)`.
`Envelope` exposes `init()`, `trigger()`, `release()`, `active()`, and
`advance(cfg)` (returns the current float level). Release from any active state
transitions to RELEASE using the current level as the starting point.

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
every sample as shown above — an optimization, not a behavior change; the
oscillator phase advance, PolyBLEP correction, and the filter's own two-pass
state update stay genuinely per-sample.

Current ADSR values:
- Attack:  10ms
- Decay:   100ms
- Sustain: 70%
- Release: 800ms

## LFO

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

### Mod-wheel vibrato (dedicated LFO)

Separate from the preset LFO above, each voice has a second, dedicated vibrato
LFO for the MIDI mod wheel: a fixed 5 Hz (`MOD_VIBRATO_HZ`) sine that modulates
pitch by up to ~±50 cents (`MOD_VIBRATO_MAX_Q15`) at full wheel. Its depth comes
from `VoiceParams::mod_depth` (Q15, 0 = off), a live control set by CC1 — it is
not part of a preset (`voice_apply_preset()` resets it to 0). It stacks on top of
any preset pitch LFO and runs from its own `mod_lfo_phase[v]` accumulator, also
reset to 0 on trigger.

## Waveform Types

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

## State-Variable Filter (SVF)

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

### Per-sample update (2-pass)
```
for pass in 0..1:
    hp = input - lp - (Q_q15 * bp) >> 15
    bp += (F_half * hp) >> 15
    lp += (F_half * bp) >> 15
output = lp | bp | hp | lp+hp depending on mode (input if OFF)
```

### Coefficient computation
- **F_half** (Q15): `cutoff_hz * 76539 >> 15`, clamped [33, 15564]
  - Approximation of `π * cutoff / sample_rate`, <0.1% error
- **Q** (Q15): `65534 - (resonance << 1)`, clamped to a minimum of 2
  - resonance=0 → Q=65534 (2.0, no resonance)
  - resonance=32767 → Q≈0 (near self-oscillation)

### Modulation
Per-sample cutoff = base_cutoff + (envelope × env_amount >> 15) + (LFO × lfo_filter_depth >> 15),
clamped 20–18000 Hz. Q is constant per buffer.

Filter state (lp, bp) reset to 0 on voice trigger for clean attacks.

### CPU cost
~20 integer ops per sample per voice (6 multiplies, 6 shifts, 8 adds/
subtracts — 2-pass integration, 3 multiplies per pass).
Estimated ~1-2% per voice on profiling pin.
