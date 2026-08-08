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

The following measurements were measured after a couple of additions: envelopes, effects, modularization (`subtractive` and `groovebox`).
The baseline reflects the state on 2026-08-06 prior to implementing `tracker`, `speech` and `fm` modules and the subblock optimizations.
Max is measured when using all 16 voice channels.

| Phase       | Idle  | Voc A | Voc B | Voc C | ABC   | Max   | Comment |
| - | - | - | - | - | - | - | - |
| no FX       | 0.48% |  6.4% |  6.9% |  6.3% |   -   | ~90%  | |
| Delay FX    | 1.66% |  7.5% |  8.1% |  7.4% |   -   | ~90%  | |
| Reverb FX   |  8.2% | 14.1% | 14.6% | 14.0% |   -   | ~90%  | |
| LFO(vibrato)| 0.48% |  7.2% |  7.7% |  7.1% |   -   | ~90%  | Pitch LFO through modwheel. No FX |
| | | | | | | | |
| no FX       | 0.53% |  6.9% |  7.4% |  6.8% |   -   | ~90%  | After pan fix (issue #11). Slight (~0.5% for active voice, 0.05% idle) raise in CPU |
| Delay FX    |  2.1% |  8.4% |  8.9% |  8.3% |   -   | ~90%  | After pan fix (issue #11). As above |
| Reverb FX   |  8.4% | 14.8% | 15.3% | 14.7% |   -   | ~90%  | After pan fix (issue #11). As above |
| | | | | | | | |
| no FX       |  0.6% |  5.9% |  5.7% |  5.1% |   -   | ~86/80/70%  | After subchunk fix (issue #12). Max is depending on voice used (A/B/C). Major improvement for modulator-heavy voices (Voice C)! |
| Delay FX    |  2.1% |  7.3% |  7.2% |  6.6% |   -   | ~86/80/70%  | After subchunk fix (issue #12) |
| Reverb FX   |  8.5% | 13.7% | 13.6% | 13.0% |   -   | ~94/90/81%  | After subchunk fix (issue #12) |
| LFO(vibrato)|  0.6% |  5.9% |  5.7% |  5.1% |   -   | ~86/80/70%  | After subchunk fix (issue #12). Pitch LFO through modwheel. No FX. No measurable overhead for vibrato! |

## Tracker Engine (build skeleton, #13)

Third build-time engine (`make ENGINE=tracker`, `breadboard_rp2350` default),
proving the build seam and `tracker.md`'s deviations from the shared layer
model before any XM/mixer logic exists:

- `MAX_VOICES = 32`, defined in `src/engines/tracker/engine.h` ahead of its
  `#include "engine_base.h"`, per #10. Only this engine — subtractive and
  groovebox stay at 16.
- `voice_alloc` is not used: in XM, channel N is voice N (fixed assignment,
  no allocation/stealing/age tracking). `src/voice_alloc.cpp` is excluded
  from the link entirely (`CMakeLists.txt`'s `ENGINE_VOICE_ALLOC`, empty for
  `T00T_ENGINE STREQUAL "tracker"`); `main.cpp`'s calls into it compile out
  behind `HAS_VOICE_ALLOC` (defined `0` only for this engine).
- `fx/delay.h` / `fx/reverb.h` are not `#include`d by
  `src/engines/tracker/audio_engine.cpp`, so neither is linked — their
  combined ~128 KB of `.bss` must never compete with the tracker's future
  350-400 KB sample budget.
- `src/engines/tracker/display.cpp` and `midi_controller.cpp` are stubs (no
  UI, no note routing yet) so the shared `gfx.cpp` path and MIDI transports
  still link.
- Sound source: voice 0 is a hardcoded, always-on 440 Hz test tone (centre
  pan, through the shared stereo output tail) — a build/boot smoke test, not
  a mixer. Voices 1-31 are unused placeholders. `PROFILE_PIN` (GPIO 22) is
  bracketed around the render call, ready for the 32-voice measurement slice.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=tracker`, and confirmed via `nm` that no `voice_alloc`/`FxDelay`/
`FxReverb` symbols appear in the binary:

| Engine | text | bss | dec |
|---|---|---|---|
| tracker (skeleton) | 25,328 | 10,284 | 35,612 |
| subtractive (default) | 206,040 | 198,344 | 404,384 |
| groovebox | 55,516 | 199,764 | 255,280 |

The subtractive/groovebox `.bss` figures are dominated by the sample corpus
and wavetables baked into those engines, not by `voice_alloc`/delay/reverb —
this skeleton has none of that yet. `make`, `make ENGINE=groovebox`, and
`make ENGINE=tracker` all build clean from a fresh `build/`; the first two
are unchanged in size from before #13.

## Tracker Engine — 32-Voice Mixer (#15/#16)

Replaces the #13 test tone with the real stripped mixer from `tracker.md`
("Rendering Pipeline" / "Voice mixer"): `src/engines/tracker/mixer.h` is a
pure-integer, pico-sdk-free header (`TrackerSample`/`TrackerVoice`,
`mix_voice()`, `samples_to_loop_end()`, `wrap_loop()`,
`tracker_render_buffer()`) shared verbatim between the device engine and
`tools/host_render/render_tracker_mixer.cpp`, which proves ramp linearity,
loop-wrap bounds, one-shot end-of-sample, and nearest-vs-linear divergence
against exact expected values before anything touches hardware.

### #16 measurement rig

No display and no stdio on this build (USB is MIDI-only), so the profiling
pin is the only readout. `audio_engine_run()` self-cycles through 6 phases,
holding each 4 seconds, forever: idle (0 voices, isolates fixed per-buffer
overhead), 8, 16, 24, 32 voices (linear), 32 voices (`mix_voice_nearest()`).
Every voice loops forever — nothing goes inactive mid-phase — so each
phase's reading is stable for its whole hold time, unlike #15's demo (which
let one-shot voices decay away). Voice 0 is always a deliberately tight
4-sample loop (`samples_to_loop_end()`'s worst case: many short runs per
sub-block instead of one); the rest are the #15 chorus. Buffer size is a
build-time choice (`audio_common.h`'s `T00T_SAMPLES_PER_BUFFER`, overridden
via `make ENGINE=tracker DMA_BUFFER_SIZE=512`) so the idle number could be
re-measured at 512 for the IRQ-overhead comparison.

Measured on `breadboard_rp2350`, 2026-08-07:

| Buffer | Idle | 8v | 16v | 24v | 32v linear | 32v nearest |
|---|---|---|---|---|---|---|
| 256 | 0.52% | 8.20% | 15.3% | 22.8% | 30.1% | 20.1% |
| 512 | 0.52% | 8.20% | 15.3% | 22.8% | 30.1% | 20.1% |

Per-voice cost, `(duty - idle) / voice_count`, cycles/frame at 3401
cycles/frame = 100%:

| Voices | Linear | Nearest |
|---|---|---|
| 8 | 32.65 c/f (0.96%) | — |
| 16 | 31.42 c/f (0.92%) | — |
| 24 | 31.57 c/f (0.93%) | — |
| 32 | 31.44 c/f (0.92%) | 20.81 c/f (0.61%) |

Flat at ~31.5 cycles/frame/voice across 8/16/24/32 — including the tight-loop
stress voice at every point, so this is the honest worst-case number, not a
best case that degrades under load.

### Decisions (written per #16's acceptance criteria; full reasoning in `tracker.md` Settled Decisions)

- **32 channels**, not 16. 30.1% of Core 1 for the full complement, inside
  the 25-40 cycles/frame prediction and comfortably under the ≤50% goal —
  ~20 points of headroom left for a limiter or global effect send.
- **Linear interpolation, no nearest-neighbour build flag.** Nearest saves a
  real 33% (20.81 vs 31.44 c/f) and does audibly alias, as predicted, but
  linear already clears budget with room to spare — nothing forces the
  trade. `mix_voice_nearest()` stays in `mixer.h`, proven and ready if a
  later voice-count push needs it.
- **DMA buffer size stays 256.** Idle duty — the number that isolates fixed
  per-buffer/IRQ overhead — was identical at 256 and 512 (0.52% both), as
  was every other phase re-checked at 512. No measurable IRQ overhead at
  this sample rate means 512 would only add latency for zero offsetting
  benefit.
- DSP-extension / SIO-interpolator options (tracker.md's optional fallback)
  were not evaluated — only called for if the headline number missed
  target, and 30.1% is well inside it.

## Speech Engine (build skeleton, #27)

Fourth build-time engine (`make ENGINE=speech`, `breadboard_rp2350` default),
proving the build seam and `speech.md`'s divergences from the shared layer
model before any formant DSP exists:

- `MAX_VOICES = 4`, defined in `src/engines/speech/engine.h` ahead of its
  `#include "engine_base.h"`, per #10 — a placeholder pending the P2
  profiling measurement in `speech.md`. Only this engine — the other three
  are untouched.
- Unlike the tracker (#13), the standard `ParamExchange`/`voice_alloc` path
  is kept: `speech.md` settles that polyphonic speech has N independent
  per-voice segment clocks, not one global tick clock, so the tracker's
  ordered TickBlock ring is not adopted here. `src/voice_alloc.cpp` links
  normally (no `ENGINE_VOICE_ALLOC` override needed in `CMakeLists.txt`).
- `fx/delay.h`/`fx/reverb.h` **are** linked (unlike the tracker) — `speech.md`:
  "speech has no sample-RAM pressure" to protect, so there's no reason to
  exclude them, and the `.bss` table below confirms they're present (a
  128 KB delay ring buffer plus reverb, not stripped by the linker).
- Native render rate is `SPEECH_RATE = SAMPLE_RATE / 2` (22.05 kHz), zero-
  order-held ×2 to the shared 44.1 kHz output stage — `src/engines/speech/
  render.h`'s `speech_render_test_tone()` does the resample as a bare integer
  doubling (`dry_{l,r}[2*i] == dry_{l,r}[2*i+1]`), no fractional accumulator.
  That function has no pico-sdk dependency (only `osc/sine.h` + `pan.h`), so
  it is the literal shared source between the device path
  (`audio_engine.cpp`, called from the Core 1 render loop) and the new host
  target below — proving the ZOH seam identically on both before any real
  DSP depends on it.
- `src/engines/speech/display.cpp` and `midi_controller.cpp` are stubs (no
  UI, no phoneme keyboard yet) so the shared `gfx.cpp` path and MIDI
  transports still link.
- Sound source: voice 0 is a hardcoded, always-on 220 Hz test tone (centre
  pan) rendered at the native rate and ZOH'd to the stereo output tail — a
  build/boot smoke test, not a synth. Voices 1-3 are unused placeholders.
  `PROFILE_PIN` (GPIO 22) is bracketed around the render call, ready for the
  P2 measurement slice.
- Host target: `render_speech` (`tools/host_render/render_speech.cpp`, built
  via `make host`) calls the identical `speech_render_test_tone()`, renders
  2 s to `speech_test_tone.wav`, and asserts the ZOH invariant sample-by-
  sample rather than by ear. All checks pass as of 2026-08-07.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=speech`, and confirmed via a `.bss` size comparable to the groovebox's
(both link delay+reverb) that neither effect was stripped:

| Engine | text | bss | dec |
|---|---|---|---|
| speech (skeleton) | 27,544 | 194,260 | 221,804 |
| subtractive (default) | 206,096 | 198,344 | 404,440 |
| groovebox | 55,580 | 199,764 | 255,344 |
| tracker | 58,124 | 409,080 | 467,204 |

`make`, `make ENGINE=groovebox`, and `make ENGINE=tracker` all build clean
from a fresh `build/`, unchanged in behaviour by #27 (the subtractive/
groovebox/tracker figures above drift slightly from their #13-era table
entries due to unrelated work landed since — not this change).

## Speech Engine P2 Profiling (#31)

Speech's measurement gate — the direct analogue of the tracker's #16 — deciding
`MAX_SPEECH_VOICES` from a real profiling-pin reading of the full tract (#28 cascade
+ #29 fricative/nasal branches) instead of `speech.md`'s "do not trust these
numbers" budget-table estimate.

### Measurement rig

`make ENGINE=speech SPEECH_PROFILE=1` builds an alternate `audio_engine_run()`
(guarded by `T00T_SPEECH_PROFILE`, `src/engines/speech/audio_engine.cpp`) that
replaces the normal MIDI-driven loop with a self-cycling, pin-only rig — same
"hands-off" shape as #16's tracker rig: no display, no stdio, `PROFILE_PIN` (GPIO
22) is the only readout. `MAX_VOICES` is raised to 8 for this build only
(`engine.h`, gated on the same define) so the 8-voice phase has a real 8th slot;
the normal build is untouched and still `MAX_VOICES = 4`.

It self-cycles through 8 phases, holding each ~4s forever, driving `voices[]`
directly with synthetic content (bypassing `ParamExchange`/MIDI entirely, so
content is identical and repeatable on every cycle):

| Phase | Voices | Phoneme | What it isolates |
|---|---|---|---|
| idle | 0 | — | Fixed per-buffer overhead (buffer clear, output loop) |
| 1v | 1 | /a/ (vowel) | Baseline single-voice cost |
| 2v | 2 | /a/ | Linearity check |
| 4v | 4 | /a/ | Working-assumption voice count |
| 8v | 8 | /a/ | Double the working assumption |
| 4v fricative | 4 | /z/ (voiced fricative) | Parallel branch cost vs. the 4v /a/ phase — same voice count, only the phoneme differs |
| 4v recompute-only | 4 | /a/ | Calls `tract_advance_subblock()` only, skipping the per-sample `tract_process_mixed()` loop entirely — isolates coefficient-recompute cost vs. the 4v /a/ phase |
| 4v swept | 4 | /a/ | `formant_shift`/`bandwidth_scale` triangle-swept across their full CC range every buffer, vs. the 4v /a/ phase held at 1.0x |

Reading procedure: flash, let it free-run, read the profiling-pin duty cycle during
each phase (a scope/logic analyzer trigger on the phase transitions, or just enough
patience to catch each ~4s window), record against the table below.

Held-vowel-vs-fricative and static-vs-swept are both expected, from code
inspection, to measure the *same* as their comparison phase: `tract_process_mixed()`
always ticks the cascade, fricative and nasal resonators unconditionally regardless
of a phoneme's `av`/`af`/`an` mix weights (no branch skips a silent resonator), and
`tract_advance_subblock()` always recomputes every resonator's coefficients every
sub-block regardless of whether `formant_shift_tgt`/`bandwidth_scale_tgt` actually
moved. If the real numbers disagree with that prediction, that is more interesting
than confirmation — it would mean some other effect (icache, compiler scheduling)
is in play, and needs explaining, not just recording.

### Measured, `breadboard_rp2350`, 2026-08-07

| Phase | idle | 1v | 2v | 4v | 8v | 4v /z/ | 4v recompute-only | 4v swept |
|---|---|---|---|---|---|---|---|---|
| Duty cycle | 0% | 2.82% | 5.5% | 11% | 22% | 11% | 1.3% | 11% |

Converted to cycles/output-frame at 3401 cycles/frame = 100% (idle is 0%, so no
baseline to subtract, unlike the tracker's #16 rig):

| Voices | Total | Per voice (c/f) | Per voice (%) |
|---|---|---|---|
| 1 | 95.9 c/f | 95.9 | 2.82% |
| 2 | 187.1 c/f | 93.5 | 2.75% |
| 4 | 374.1 c/f | 93.5 | 2.75% |
| 8 | 748.2 c/f | 93.5 | 2.75% |

Flat at **~93.5 cycles/frame/voice (2.75%)** from 2 to 8 voices; 1v's 2.82% is
within scope-reading resolution of the same flat line, not a real per-voice
discontinuity.

**Held vowel vs. voiced fricative: identical (11% both).** Confirms the
code-inspection prediction — the fricative/nasal branches are unconditional, not an
incremental cost paid only by phonemes that use them. There is no separate
"fricative branch cost" to add on top of the flat per-voice number above.

**Coefficient recompute, isolated: 1.3% at 4 voices** = 44.2 c/f total = **11.0
c/f/voice (0.32%)**. Subtracting that from the 4v total gives the per-sample-only
cost: 93.5 − 11.0 = **82.5 c/f/voice (2.43%)** for excitation + the 7-resonator tick
+ ZOH accumulate.

**Static vs. swept `formant_shift`/`bandwidth_scale`: identical (11% both).**
Confirms the second prediction — recompute happens every sub-block regardless of
whether the CC target actually moved, so a live sweep is free; the cost is already
fully included in the flat per-voice number, not an addition to it.

### Discrepancy vs. the predicted budget

Measured ~93.5 c/f/voice total is noticeably above speech.md's ~60–75 c/f
prediction — about 25–55% over the top of that range, the kind of gap "do not trust
these numbers" was written to expect. But it isn't evenly spread across the
budget's four line items:

- **Coefficient recompute measured at 11.0 c/f, the predicted line item was ~10
  c/f.** This part of the estimate was essentially right.
- **The remaining 82.5 c/f (excitation + resonators + ZOH) is well above the
  35–50 + 10 + 5 = 50–65 c/f those three line items predicted** — roughly 17–32
  c/f higher, i.e. the entire discrepancy lives here, not in coefficient recompute.

Explanation: the budget table priced the DSP math itself (resonator taps, pulse/
noise generation, doubling samples) but not the per-sample "glue" `render.h`'s loop
actually runs for every voice, every native sample — the `cur_amp` exponential
declick filter, the `voiced_src`/`noise_src` mix-and-scale multiplies, and writing
each ZOH-doubled sample into *two* accumulator buffers (4 stores per native sample,
not 2). None of that is its own line item in the prediction, but the code pays it
unconditionally regardless of phoneme — which is exactly why the fricative and
swept-parameter phases above cost nothing extra: that per-sample tax and the
resonator branch count are both fixed, not phoneme-dependent. On an M33 doing
single-precision float work with no vectorization, a handful of extra multiply/
store instructions per sample is a plausible source for the remaining gap, sized
about right for what it costs.

### Decision: `MAX_SPEECH_VOICES = 8`

Landed in `src/engines/speech/engine.h` (was 4 since #27).

- **93.5 c/f/voice is flat across the whole measured range (1–8 voices)** — no
  falloff or superlinear growth to worry about at higher counts.
- **8 voices costs 22% of Core 1 on their own.** Delay/reverb are mutually
  exclusive (mono send/return, one active effect at a time — `audio_engine.cpp`),
  so the worst realistic total is 8 voices + reverb = 22% + 8% = **30%**,
  comfortably under the ≤50% ceiling the tracker's own #16 decision used as its
  target.
- This is double the #27-era working assumption of 4, so **"robot chorus" (a
  preset with per-voice detune and stereo spread, speech.md's Open Questions) is
  now explicitly on the table for P5** — the payoff speech.md said to name if the
  number landed better than expected.
- Not pushed further than 8: that's what was measured, and the ≥30% total with
  reverb is a reasonable place to stop banking headroom against P3's still-unbuilt
  segment sequencer, which speech.md itself budgets as "a table read plus
  coefficient computation" but hasn't been measured yet.

`speech.md`'s Open Question 2 (voice count) is struck and moved into Settled
Decisions.

## FM Engine (build skeleton, #41)

Fifth build-time engine (`make ENGINE=fm`, `breadboard_rp2350` default),
proving the build seam before any DX7 logic exists — `fm.md` says "start at
P0 and do not skip it," but P0's measurement rig needs a build target to
measure *on*, so this slice exists first, exactly as #13 preceded #15/#16 for
the tracker and #27 preceded #31 for speech:

- `MAX_VOICES = 16`, defined in `src/engines/fm/engine.h` ahead of its
  `#include "engine_base.h"`, per #10 — `fm.md` §3.4's working assumption for
  full 6-operator polyphony, explicitly **provisional pending the P0
  measurement gate**. Only this engine — the other four are untouched.
- Standard `ParamExchange`/`voice_alloc` path, same as speech and unlike the
  tracker: `fm.md` has no fixed channel→voice mapping, so there's no reason to
  deviate. `src/voice_alloc.cpp` links normally, no `CMakeLists.txt` override.
- `fx/delay.h`/`fx/reverb.h` **are** linked — `fm.md` §2: "FM's whole working
  set is ~12 KB... nothing in this design should introduce a dependency on
  PSRAM," and there's no sample-RAM pressure to protect either. Confirmed by
  the `.bss` table below: FM's 202,768 bytes sit alongside groovebox's
  199,764 and subtractive's 198,344 (both link delay+reverb), nowhere near
  stripped.
- `src/engines/fm/sine_tab.h` — the FM-specific 4096-entry `int16_t` operator
  sine table from `fm.md` §5.1: quarter-wave symmetric generation (only the
  first quarter computed with `sinf()`, the rest mirrored/negated into
  place) but the full table stored, indexed by `phase >> 20` with **no
  interpolation** (§3.5: interpolation costs ~45% more per operator for no
  audible benefit under FM's own harmonic density). Header-only inline
  variable (C++17), same pattern as `res2p.h`'s `res2p_radius_lut` — no
  separate `.cpp` needed, so the top-level `CMakeLists.txt`'s engine source
  list needed no changes at all (unlike the tracker's #13, which added
  `ENGINE_VOICE_ALLOC`/`ENGINE_TRACKER_PLAYER` overrides). `osc/sine.*`
  itself is untouched — the shared 1024-entry interpolating table stays
  exactly as the other four engines left it, and is still used here for
  `pan.h`'s quadrature pan-gain lookup (a different table, different job).
- `src/engines/fm/render.h`'s `fm_render_test_tone()` is the literal shared
  source between the device path (`audio_engine.cpp`, called from the Core 1
  render loop) and the new host target below — same shape as speech's
  `speech_render_test_tone()`, proving the table/phase seam identically on
  both before any operator kernel exists. No pico-sdk dependency (only
  `sine_tab.h` + `pan.h`).
- `src/engines/fm/display.cpp` and `midi_controller.cpp` are stubs (no UI, no
  patch/note logic yet) so the shared `gfx.cpp` path and MIDI transports
  still link — same reasoning as speech's #27 stubs, including staying off
  the shared `src/midi/midi_controller.cpp`, which expects a
  `presets.h`/`VoicePreset` shape this engine's minimal `VoiceParams`
  doesn't have. The `presets.h`-existence gate that broke on speech (#38) is
  unaffected — FM has no `presets.h` either, same as speech and groovebox at
  this stage.
- Sound source: voice 0 is a hardcoded, always-on 440 Hz test tone (centre
  pan), rendered through the FM sine table via `fm_render_test_tone()` at the
  full 44.1 kHz output rate (no ZOH — FM runs at the shared `SAMPLE_RATE`,
  unlike speech's half-rate native path) — a build/boot smoke test, not a
  synth. Voices 1–15 are unused placeholders. `PROFILE_PIN` (GPIO 22) is
  bracketed around the render call, ready for the P0 measurement slice.
- Host target: `render_fm` (`tools/host_render/render_fm.cpp`, built via
  `make host`) calls the identical `fm_render_test_tone()`, renders 2 s to
  `fm_test_tone.wav`, and checks the sine table is an odd function about the
  origin (`fm_sine_table[i] == -fm_sine_table[N-i]`) — the invariant its
  quarter-wave-symmetric construction depends on — rather than trusting it by
  ear. All checks pass as of 2026-08-08.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=fm`, alongside a fresh rebuild of all four other engines to confirm
#41 changed nothing about them:

| Engine | text | bss | dec |
|---|---|---|---|
| fm (skeleton) | 27,784 | 202,768 | 230,552 |
| subtractive (default) | 206,096 | 198,344 | 404,440 |
| groovebox | 55,580 | 199,764 | 255,344 |
| tracker | 58,124 | 409,080 | 467,204 |
| speech | 73,156 | 196,588 | 269,744 |

`make`, `make ENGINE=groovebox`, and `make ENGINE=tracker` reproduce their
#27-era table entries byte-for-byte, confirming #41 didn't touch them; the
speech figures have simply grown since #27 (P2–P4 landed in the meantime),
unrelated to this change.

## Host DSP Tooling

`tools/host_render/` (issue #5 phase 2) is a standalone CMake project — no
pico-sdk, host compiler — for verifying pure-DSP common-layer headers by
rendering them to WAV instead of on real hardware. `make host` configures and
builds it into `tools/host_render/build/` (git-ignored, same as the device
`build/`); binaries there also write their WAV output alongside themselves.

This exists so tracker/speech/FM module work has a host-render harness to
build on (speech.md calls this "the single most valuable test"), and so new
common-layer DSP components can be proven correct before they're wired into
any real-time engine, instead of validating them by ear against a rewrite of
a working sound.

First consumer: `src/res2p.h`, a two-pole resonator for the speech module's
formant cascade and (pending backport) the groovebox's 808 toms/congas/
cowbell. `render_res2p` sweeps 1600 (frequency, bandwidth) pairs across the
ranges both callers need and asserts every pole lands inside the unit circle,
then renders impulse responses at three representative tunings and checks
measured ringing frequency (zero-crossing rate, <5% error) and decay (late-
window RMS < 10% of early-window RMS). All checks pass as of 2026-08-06.
`res2p.h` is not yet wired into the groovebox — that backport, and the 808
before/after diff, is speech.md P0, done independently once speech work
starts.

## xm2t00t Host Converter (#14)

`tools/xm2t00t/` — a pure-Python, stdlib-only tool (no CMake project, unlike
`host_render/`) that turns a `.xm` module into a t00t-native binary blob. Per
tracker.md, the device never parses XM: this runs once, offline, at build
time. `tracker.md`'s "Multi-format support" section is why this is Python and
not C++ — it says adding MOD/S3M later is "mostly host-side Python", which
only makes sense if the XM loader already is.

```
xm2t00t.py convert <in.xm> <out_prefix>   # writes <out_prefix>_blob.h (a linkable
                                            # `static const uint8_t ..._blob_data[]`
                                            # array, same convention as samples/*.h),
                                            # prints the song-structure dump, applies
                                            # the Q18.14 + SRAM checks below
xm2t00t.py dump <in.xm>                    # prints the dump only, writes nothing
xm2t00t.py gen-header <out.h>              # (re)generate blob_format.h from
                                            # blob_format.py, the format's source of truth
```

### Blob format

Single flat blob, fixed-size headers, every offset a byte offset from blob
start (never a pointer) — `blob_format.py` declares every struct once and is
used both to pack the blob and to generate `blob_format.h`, its C++ mirror,
so the two views of the layout can't drift apart.
`SongHeader -> order_table, PatternHeader[]+Event[] (6 bytes/cell), InstrumentHeader[]
(keymap, envelopes in tick units, vibrato) -> SampleHeader[]`, each sample
carrying a precomputed 96-entry Q8.24 note -> increment table (`periods.py`,
linear and Amiga frequency modes) so the device never runs period math. XM
effects (including the `Exy`/`Xxy`/`Fxx` commands that overload one letter for
several meanings) are normalized into named `Effect`/`VolEffect` enums
(`effects.py`) rather than left as raw nibbles for the device to switch on.
`sample_data_offset`/`sample_data_bytes` mark one uninterrupted PCM+guard-byte
region (guard = loop-start value if looped, last value if one-shot) meant for
a straight `memcpy` into SRAM — nothing else is interleaved into it.

v1 hard limits, both enforced at convert time with an actionable message and
a non-zero exit (no blob written): any single sample over the Q18.14 cap
(262,144 frames), or total sample data over an SRAM budget (`--budget-kb`,
default 380 KB — tracker.md's stated 350-400 KB after code/stacks/DMA/mixer
scratch). No dynamic-loading simulator yet (tracker.md: phase 2) — just a
static sum, per tracker.md's "v1 requires the module to fit in SRAM".

Not wired into `CMakeLists.txt` — #14 is host-only by design; a later mixer
issue links `blob_format.h`/a converted song into the tracker engine build.

**Caveat**: the Amiga-mode period table and both modes' finetune handling are
implemented from XM/FT2's public, well-documented formulas and sanity-checked
(note C-4, finetune 0 -> exactly 8363 Hz, the XM reference; one octave up
doubles the frequency) — not verified bit-exact against a real FT2 period
dump. That precision matters once something plays these increments back (the
mixer issue), not for a v1 loader.

### Validation

`python3 tools/xm2t00t/test_xm2t00t.py` — unit checks (period/effect/struct
sanity, the two rejection paths) always run; corpus-dependent checks run
against whatever `.xm` files are in `xm/` (gitignored — third-party
copyrighted modules aren't committed; populate it yourself, e.g. from Mod
Archive, and re-run) and skip cleanly if that directory is empty:

- Song structure (channels/orders/patterns/instruments/samples counts) vs
  both `openmpt123 --info`'s stdout and libopenmpt's C API directly
  (`openmpt_ref.py`, `ctypes` over `libopenmpt.so.0` — no Python binding
  needed), the latter also diffing the exact order-\>pattern list.
- Delta-decode round-trip: a second, independently-coded implementation of
  XM's delta decode (explicit signed arithmetic + wrap loop, vs. the
  converter's unsigned mod-256/65536 accumulate) compared byte-for-byte
  against the converter's own decode, over every sample in the corpus.
- Guard-byte correctness and full blob self-consistency (re-reading the
  emitted bytes with `blob_format.py` and diffing every header field, the
  order table, pattern row counts, and sample PCM against the source parse).

Verified against 5 real modules (2026-08-07): `118in64.xm` (18ch),
`bzl-hscr.xm` (16ch), `dubmood_-_mario_airlines_keygen_edit.xm` (12ch),
`kenny_beltrey_-_positrons.xm` (32ch — tracker.md's max), `records.xm`
(16ch). All match `openmpt123 --info` and libopenmpt exactly; all pass every
check above; all convert well under the default SRAM budget (93-241 KB of a
380 KB budget).

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
(vibrato), **CC10 (pan)** → `pan` (subtractive engine only; live, applied to
held voices immediately), and **pitch bend** → phase-increment ratio. CC0/CC32
(bank select) are stored but not yet used.
