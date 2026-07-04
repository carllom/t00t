# T00T Audio Engine Architecture

## Overview

Dual-core architecture on RP2350 (Raspberry Pi Pico 2):
- **Core 0**: Control plane — input polling (MIDI + buttons), voice allocation, parameter management
- **Core 1**: Audio plane — synthesis, mixing, buffer filling, active-voice bitmap feedback

Two board targets are supported, selected at build time via the board header:
- **`vgaboard_rp2350`** — Pimoroni Pico VGA Demo Base, 3 buttons, I2S DAC on the board, USB MIDI.
- **`breadboard_rp2350`** — bare Pico 2 + Adafruit PCM5122 I2S breakout, no buttons/VGA, DIN (UART) + USB MIDI.

## Pin Allocation

Pins differ between the two board targets. Definitions live in
`src/boards/vgaboard_rp2350.h` and `src/boards/breadboard_rp2350.h`.

### `vgaboard_rp2350` (Pimoroni VGA Demo Base)

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | Button A | Shared with VGA color base, active-high, pull-down |
| 1 | *free* | VGA color (unused) |
| 2 | *free* | VGA color (unused) |
| 3-4 | *free* | VGA color (unused) |
| 5 | SD CLK | (blocks DIN MIDI on this board) |
| 6 | Button B | Shared with VGA color |
| 7-10 | *free* | VGA color (unused) |
| 11 | Button C | Shared with VGA color |
| 12-15 | *free* | VGA color (unused) |
| 16-17 | VGA sync | Sync base (unused) |
| 18-19 | SD CMD / DAT0 | |
| 20-21 | UART1 TX / RX | `PICO_DEFAULT_UART` = 1 |
| 22 | Profile pin | Synthesis workload measurement (probe directly) |
| 26 | I2S DATA | DAC DIN |
| 27 | I2S BCK | DAC bit clock (also PWM R) |
| 28 | I2S LRCK | DAC word select (also PWM L) |

### `breadboard_rp2350` (Pico 2 + Adafruit PCM5122)

| GPIO | Function | Notes |
|------|----------|-------|
| 0-1 | UART0 TX / RX | `PICO_DEFAULT_UART` = 0 (debug only) |
| 5 | DIN MIDI in | UART1 RX, 31250 baud (via optocoupler) |
| 16 | I2S BCK | PCM5122 BCK (clock pin base) |
| 17 | I2S LRCK | PCM5122 WSEL (BCK base + 1) |
| 18 | I2S DATA | PCM5122 DIN |
| 22 | Profile pin | Synthesis workload measurement (probe on breadboard) |
| 8 | LCD DC | Waveshare 1.83" data/command |
| 9 | LCD CS | Chip select (manual GPIO) |
| 10 | LCD CLK | SPI1 SCK |
| 11 | LCD DIN | SPI1 TX (MOSI) |
| 12 | LCD RST | Reset |
| 13 | LCD BL | Backlight (PWM) |

No buttons, VGA, or SD on the breadboard; control is MIDI-only. The optional
1.83" 240×284 IPS LCD (ST7789P) is driven by Core 0 at low priority — see the
`src/wslcd/` driver.

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
    clip scratch (__ssat) → int16_t, mono duplicated to L+R of target buffer
    clear profiling GPIO
    push active-voice bitmap to Core 0 via reverse FIFO
```

Voice state (phase accumulators, LFO phase, LFSR, envelopes) lives on Core 1 only.
Voice parameters come from Core 0 via the double-buffered param block.

## Core 0 — Control Plane

`main()` initializes MIDI transports, the param exchange, the voice allocator,
buttons (only if `HAS_BUTTONS`), then launches Core 1 and starts the I2S DMA.
The main loop sleeps on `__wfi()` and services inputs when woken:

```
main():
  init MIDI (USB and/or UART), param_exchange.init(), voice_alloc_init()
  if HAS_BUTTONS: controller_init()
  launch Core 1 (audio_engine_run), then i2s_output_init()
  loop:
    if MIDI_USB:  usb_midi_task(); usb_midi_poll(params)
    if MIDI_UART: uart_midi_poll(params)   // drains the UART RX ring buffer
    if HAS_BUTTONS and 1ms tick elapsed:   // time_reached(next_tick)
      controller_tick(params)
    __wfi()  // sleep until next IRQ (USB, UART, timer)
```

`controller_tick()` (and the MIDI controller) drive a note on/off into the
voice allocator. The flow inside `controller_tick()`:

```
controller_tick(params):
  voice_alloc_update()                  // drain reverse FIFO for active bitmap
  shadow = params->active()             // start from current committed truth
  poll buttons → integrator debounce → edge detect
    on press:  v = voice_alloc_allocate(); apply preset to shadow.voices[v];
               phase_inc, trigger++, gate = true
    on release: shadow.voices[v].gate = false; voice_alloc_release(v)
  if changed: params->commit()          // flip param double-buffer
```

There is no separate event queue: edges are applied straight to the shadow
block and committed in the same tick.

## Voice Parameter Double Buffer

Core 0 writes to a shadow copy. On commit, it flips an atomic index.
Core 1 reads from the committed copy at the start of each render pass.

```c
struct VoiceParams {
    uint32_t phase_inc;        // 22.10 fixed-point phase increment (pre-computed by Core 0)
    int16_t  amplitude;        // base amplitude / velocity (0–32767)
    uint8_t  trigger;          // generation counter, incremented on each note-on
    bool     gate;             // true while key held, false on release
    Waveform waveform;         // oscillator waveform type
    uint16_t duty_cycle;       // duty cycle for square wave (0–1023, 512 = 50%)
    float    lfo_rate;         // LFO frequency in Hz (0 = off)
    float    lfo_depth;        // LFO → amplitude depth (0.0–1.0, 0 = off)
    float    lfo_pitch_depth;  // LFO → pitch depth (0.0–1.0, 0.05 ≈ ±1 semitone)
    float    lfo_pwm_depth;    // LFO → duty cycle depth (0.0–1.0, fraction of full range)
    FilterMode filter_mode;    // LP, BP, HP, notch, or off (bypass)
    uint16_t filter_cutoff;    // base cutoff in Hz (20–18000)
    uint16_t filter_resonance; // resonance 0–32767 (0 = none, 32767 = self-oscillation)
    int16_t  filter_env_amount;// envelope → cutoff in Hz (signed, ±18000)
    float    lfo_filter_depth; // LFO → cutoff in Hz (signed, ±18000)
    const SampleDef *sample;   // sample definition (nullptr for non-sample waveforms)
    int16_t  mod_depth;        // mod-wheel vibrato depth, Q15 (0 = off) — dedicated Core 1 LFO
};

struct VoiceParamBlock {
    VoiceParams voices[MAX_VOICES];  // MAX_VOICES = 16
};

// Double-buffered exchange — two copies plus an atomic index.
struct ParamExchange {
    VoiceParamBlock blocks[2];
    volatile uint8_t committed;  // 0 or 1, written by Core 0

    void init();                       // zero both blocks to a SINE/FILTER_OFF default
    VoiceParamBlock &shadow();         // Core 0: blocks[1 - committed] (write target)
    void commit();                     // Core 0: barrier, flip committed, __sev()
    const VoiceParamBlock &active() const;  // Core 1: blocks[committed] (read target)
};
```

No locks needed: Core 0 only writes the shadow (non-committed) block via `shadow()`.
Core 1 only reads the committed block via `active()`. `commit()` issues a compiler
memory barrier, flips `committed` (a single-byte store, atomic on the M33), then
`__sev()` to wake Core 1 if it is in WFE. LFO depths are stored as floats here and
converted to Q15 once per buffer inside the render loop.

## Profiling GPIO

GPIO 22 (`PROFILE_PIN`), on both boards. High during synthesis, low while idle.
(Moved from GPIO 2 to avoid coupling to Button A on GPIO 0.)
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
polynomial residual. Fixed-point Q10 arithmetic, uses RP2040 hardware divider.
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

Measured duty cycles on the profiling pin (`PROFILE_PIN`, now GPIO 22):

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

### Performance gain table

| Phase       | Idle  | Voc A | Voc B | Voc C | ABC   | Max   | Comment |
| - | - | - | - | - | - | - | - |
| RP2040      | 0.81% | 10.3% | 12.3% | 10.5% | 31.5% | >100% | |
| RP2350 port | 0.56% |  6.3% |  6.7% |  6.1% | 18.0% | ~80%  | No code changes, just retarget |
| float env.  | 0.52% |  6.5% |  6.7% |  6.2% | 18.3% | ~85%  | Calculate envelope using floats |
| "float" lfo | 0.52% |  6.3% |  7.0% |  6.4% | 18.7% | ~95%  | Float interface for LFO, but Q15 impl + sine lookup |
| SMULL filt. | 0.50% |  5.9% |  6.6% |  6.0% | 17.5% | ~90%  | |
| SSAT env.   | 0.44% |  5.9% |  6.5% |  5.9% | 17.4% | ~90%  | |

## MIDI Input

Control comes from buttons (VGA board only) and MIDI. There is no intermediate
event queue — each input source writes the param shadow and commits directly.

- **USB MIDI** (`MIDI_USB`): TinyUSB device; `usb_midi_task()` runs the stack and
  `usb_midi_poll()` feeds received bytes to the transport-agnostic MIDI controller.
- **DIN/UART MIDI** (`MIDI_UART`): UART1 RX at 31250 baud. An IRQ fills a ring
  buffer; `uart_midi_poll()` drains it each main-loop pass. Default-on for the
  breadboard (GPIO5); off for the VGA board, where GPIO5 is SD_CLK.

Both transports route through `midi_controller_process()`, which parses MIDI
bytes, maps note on/off to voices via the allocator, and commits the shadow.
Beyond notes it also handles per-channel **CC1 (mod wheel)** → `mod_depth`
(vibrato) and **pitch bend** → phase-increment ratio. CC0/CC32 (bank select) are
stored but not yet used.
