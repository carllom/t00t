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

## FM P0 Rig (#42)

`fm.md` §11: "Start at P0 — measure." This is the measurement rig itself, not
the measurement — it's blocked ahead of the actual bench session (a future
issue), the same relationship #16's tracker mixer rig and #31's speech
profiling rig had to their own follow-up bench passes. It **decides nothing**
about voice count, table size, BLOCK, or any other §3.6 lever; it only makes
all of them buildable and switchable in one bench sitting.

Self-contained in `src/engines/fm/rig.h`, entirely separate from #41's real
engine skeleton (`engine.h`/`sine_tab.h`/`render.h` are untouched) so nothing
here risks the already hardware-verified #41 build. Selected by `make
ENGINE=fm FM_PROFILE=1` (`T00T_FM_PROFILE`, same pattern as speech's
`SPEECH_PROFILE` — see `src/engines/fm/audio_engine.cpp`'s `#if`/`#else`),
with every `fm.md` §3.6 lever as its own compile-time switch, documented in
the Makefile next to `FM_PROFILE`:

| Switch | Values | Default |
|---|---|---|
| `FM_RIG_VOICES` | voice count | 24 (past the predicted 12–21 ceiling, §3.4) |
| `FM_RIG_BLOCK` | 8 / 16 / 32 | 16 |
| `FM_RIG_TABLE_BITS` | 10 (1024) / 12 (4096) | 12 |
| `FM_RIG_INTERLEAVE` | 0 / 1 | 0 |
| `FM_RIG_NOT_IN_FLASH` | 0 / 1 | 0 |
| `FM_RIG_SMULWB` | 0 / 1 | 0 |

`FM_RIG_NOT_IN_FLASH` only moves the kernel *code*; the table itself
(`fm_rig_table`) is a runtime-generated, non-`const` array — always `.bss`/
SRAM, never flash `.rodata` — so that axis was never independently
toggleable to begin with, and the lever really measures code placement only.
`FM_RIG_SMULWB` fuses the `gain >> 8` + multiply into one M33 instruction;
this GCC's `arm_acle.h` has no standalone `__smulwb` wrapper, only
`__smlawb` (multiply-*accumulate*) — `__smlawb(gain, sample, 0)` is exactly
SMULWB with the accumulate operand forced to zero.

**Topology.** No patch, no DAG compiler (`fm.md` explicitly: "no patch
logic") — a fixed 6-operator chain per voice, hand-assigned so every
`op_render`/`op_render_fb` accumulate (`+=`) is guaranteed a preceding
`op_render_first` store on the same bus (the property the real engine's
note-on-time router will handle later):

```
op0 -> bus0            first-writer  (op_render_first, in=zero)      \_ interleaved
op1 -> bus1            first-writer  (op_render_first, in=zero)      /  when FM_RIG_INTERLEAVE=1
op2 -> bus1 (+=)       accumulate    (op_render, in=bus0)
op3 -> bus1 (+=)       accumulate    (op_render_fb, self-feedback)
op4 -> bus3            first-writer  (op_render_first, in=bus1)
op5 -> OUT             first-writer  (op_render_first, in=bus3)   -- the voice's carrier
```

All three kernel variants land in a topology that's correct by construction
rather than by convention: bus1 gets three real writers (op1 first, op2 and
op3 accumulating on top), proving the first-writer/no-clearing claim isn't
just asserted but exercised. `bus2`/`bus4`/`bus5` are allocated (`FmRigBuses`
always carries all 6 mod + 1 out) but unused by this particular chain — real
algorithms with more parallel modulators would use them.

**Fixed increments/gains** (`fm_rig_init_voice()`): every operator gets a
harmonic-multiple frequency and a flat gain (`gain_step = 0` throughout —
the field and the kernels' `gain += gain_step` instruction still exist and
execute every sample, since that's the actual cost being measured; it's the
*value* that's fixed, not the instruction). Scaled well below unity so
`FM_RIG_VOICES` summed carriers don't just sit at flat `__ssat` clipping —
a rig that saturates for its whole run can't tell a healthy render from a
broken one by ear or by eye.

**Host correctness check** (`tools/host_render/render_fm_rig.cpp`, `render_fm_rig`
target): calls the exact `fm_rig_render_buffer()` the device's `T00T_FM_PROFILE`
branch calls, in device-sized chunks, and checks the output is non-silent and
its magnitude stays within a generous per-voice-unity bound (catching a real
accumulator overflow without false-triggering on ordinary saturation, which
device `__ssat()` handles the same way regardless). Verified locally against
every lever combination by compiling the driver directly with each `-D` (host
`cmake`/`make` only builds the default combination; sweeping the rest is a
compile-flag exercise, not something worth a matrix of CMake targets):
default, `FM_RIG_INTERLEAVE=1`, `FM_RIG_TABLE_BITS=10`, `FM_RIG_SMULWB=1`,
`FM_RIG_BLOCK=8/32`, `FM_RIG_VOICES=1/32` — all pass, and every non-default
lever reproduces the default's exact peak (7644), confirming each is a pure
performance/placement change with zero effect on the arithmetic.

**Device build.** `make ENGINE=fm FM_PROFILE=1` and a combined-levers build
(`FM_RIG_INTERLEAVE=1 FM_RIG_TABLE_BITS=10 FM_RIG_BLOCK=8 FM_RIG_VOICES=32
FM_RIG_NOT_IN_FLASH=1 FM_RIG_SMULWB=1`) both build clean:

| Build | text | bss | dec |
|---|---|---|---|
| fm, FM_PROFILE=1 (defaults) | 32,080 | 23,064 | 55,144 |
| fm, FM_PROFILE=1 (all levers combined) | 29,104 | 18,296 | 47,400 |
| fm, FM_PROFILE=0 (#41 skeleton, unchanged) | 27,784 | 202,768 | 230,552 |

The profiling build's `.bss` is far smaller than #41's skeleton because
`T00T_FM_PROFILE`'s branch doesn't link `fx/delay.h`/`fx/reverb.h` at all —
the rig has no use for the 128 KB delay line, and `fm.md` never asked this
slice to carry it. `rm -rf build && make ENGINE=fm` (no `FM_PROFILE`)
reproduces #41's exact 27,784/202,768/230,552 byte-for-byte, and `subtractive`/
`groovebox`/`tracker`/`speech` all rebuilt clean and unchanged from their #41
table.

**Emitted assembly vs. `fm.md` §3.2.** Extracted by compiling `rig.h`
directly with the flags `fm.md`'s own provenance note specifies
(`arm-none-eabi-g++ -O3 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard
-mfpu=fpv5-sp-d16 -std=gnu++17`) through a `noinline` wrapper, since
`op_render()` normally inlines completely into `audio_engine_run()` at `-O3`.
First attempt reloaded `op.inc`/`op.gain_step` from memory every sample (15
instructions, not 13) — the C++ source only hoisted `phase`/`gain`/`in`/`out`
into locals, not `inc`/`gain_step`, and GCC's strict-aliasing rules can't
prove an `int32_t*` write to the output bus doesn't alias those `FmRigOp`
fields, so it played it safe and reloaded them. Hoisting all four fixed the
gap — a real, source-level finding this exercise existed to catch, not a
compiler quirk to shrug off. The corrected `op_render()` loop body:

```
ldr   r3, [r2, #4]!         @ modulation bus in (pre-increment)
add   lr, lr, r7            @ phase += inc
add   r3, r3, lr            @ phase + modulation
lsrs  r3, r3, #20            @ table index (20, not fm.md's #22 -- this rig's
                              @ default is the 4096-entry table #41 settled on)
ldrsh fp, [r1, r3, lsl #1]  @ table lookup
asrs  r3, r4, #8             @ gain >> 8
mul   fp, r3, fp             @ x sample
ldr   r3, [ip, #4]!         @ output bus accumulate (pre-increment)
cmp   r5, r2
add   r3, r3, fp, asr #14    @ >>14, accumulate
add   r4, r4, r6             @ gain += gain_step
str   r3, [ip]
bne   .L3
```

**13 instructions, exact match to `fm.md` §3.2's hand-analyzed listing** —
same instruction sequence, same register roles (phase/inc/gain/gain_step/
in-ptr/out-ptr/table-ptr each resident for the whole loop), same
pre-increment addressing GCC chose on its own. The only difference is the
shift amount (`#20` vs. the original draft's `#22`), which is exactly the
1024-vs-4096-table difference `fm.md` §3.5/§5.1 already settled — not a
discrepancy, a confirmation. `op_render_first()` compiles to 12 instructions
(one less: no pre-read of the output bus before storing, exactly the
saving §5.2 attributes to that variant). `op_render_fb()` compiles to
14–15 instructions per iteration (the self-feedback average and history
shuffle add real cost, smaller than the original draft's 1024-table-era
18-instruction estimate but the same direction). None of this is a bench
result — it's confirmation that the *compiled* kernel matches the
hand-analyzed one closely enough that the upcoming cycle-count bench session
is measuring what `fm.md` §3 actually modeled, not a compiler-introduced
detour.

## FM P0 Measurement (#43) — measured, decisions, FX-insert estimate

`fm.md` §11 step 1's actual bench session — the direct analogue of the
tracker's #16 and speech's #31. Measured on `breadboard_rp2350`, GPIO 22,
2026-08-08, via a 13-build sweep (`tools/fm_rig_sweep.sh`) each flashed and
read individually, since the #42 rig holds one fixed voice-count/lever
combination per build rather than self-cycling through phases like #16/#31.

### FX insert in isolation — reused, not freshly measured

`fx/delay.h`/`fx/reverb.h` are unchanged, engine-agnostic, global post-mix
code (`fm.md` §2: "Unchanged — global insert on Core 1"; §11 step 1 itself
suggested this cross-check: "`engine.md`'s existing subtractive table has
delay/reverb deltas that can be sanity-checked against whatever comes out
here"). Their cost is a fixed per-buffer tax applied after the voice mix,
independent of which engine produced that mix — so the subtractive engine's
already-measured deltas (Performance §, post-#12 table) are valid FX-insert
numbers for FM too, without a new build:

| | Idle duty | Delta vs. no-FX | Cycles/frame (×3401) |
|---|---|---|---|
| No FX | 0.6% | — | — |
| Delay FX | 2.1% | +1.5% | **51.0 c/f** |
| Reverb FX | 8.5% | +7.9% | **268.7 c/f** |

This **replaces `fm.md` §9's ~150 c/f (4.4%) reservation** for reverb with a
measured 268.7 c/f (7.9%) — about 1.8× the reservation. Re-running §3.4's
headline arithmetic with the corrected FX cost (available budget =
85% × 3401 − 15 idle − 268.7 FX = 2607.15, vs. the original 2726):

| Derate scenario | Cycles/voice | Voices (§9 reservation, 150 c/f) | Voices (measured FX, 268.7 c/f) |
|---|---|---|---|
| Static, as compiled | 126 | 21 | 20 |
| 25% derated | 158 | 17 | 16 |
| 50% derated (worst case) | 189 | 14 | 13 |

The correction costs about one voice at every tier. **It does not change the
plan-against-16 decision** at the 25%-derate tier fm.md §3.4 flags as the
expected case (2607/158 = 16.5, still rounds down to 16) — and as the
operator bake-off below found, the real per-voice number (measured ~100.5
c/f kernel-only, projected ~120 c/f with P2's still-unmeasured EG/LFO added)
lands nowhere near the 200 c/f/voice tier where the thinner headroom would
have mattered.

Caveat: this is a reused number from a different engine's build, not a fresh
reading from an FM binary with delay/reverb linked (the #42 rig's
`T00T_FM_PROFILE` branch deliberately excludes `fx/`, per engine.md's #42
section, to keep `.bss` small for the operator sweep). The code path is
identical either way, so a divergence would be surprising, but this was not
re-confirmed on an FM-specific build in this pass — a follow-up reading
(`make ENGINE=fm`, normal non-profile build, delay/reverb toggled) would
upgrade this from "reused" to "confirmed," at effectively no cost since
#41's skeleton already links both effects. Not done here since the 16-voice
decision doesn't depend on the last few percent of precision in this number.

### Voice-count sweep

`make ENGINE=fm FM_PROFILE=1 FM_RIG_VOICES=<n>`, defaults otherwise (BLOCK=16,
TABLE_BITS=12, INTERLEAVE=0, NOT_IN_FLASH=0, SMULWB=0, FB=1):

| Voices | Duty | Cycles/frame | Per-voice (c/f) |
|---|---|---|---|
| 1 | 2.97% | 101.0 | 101.0 |
| 2 | 6.2% | 210.9 | 105.4 |
| 4 | 12.1% | 411.5 | 102.9 |
| 8 | 23.8% | 809.4 | 101.2 |
| 16 | 47.3% | 1608.7 | 100.5 |
| 24 | 70.8% | 2407.9 | 100.3 |

Linear regression across all six points: **slope 100.05 c/f/voice, intercept
7.79 c/f** (fixed per-buffer overhead — buffer clear, DMA/FIFO handoff,
`__ssat` output write; no MIDI/IPC cost, since the rig bypasses
`ParamExchange` entirely). Flat to within measurement noise from 2 to 24
voices — no falloff or superlinear growth, same shape #16 and #31 found for
their own sweeps.

**This is markedly *below* `fm.md` §3.3's ~126 c/f static estimate — the
opposite direction from the tracker/speech historical pattern** (measured
usually runs 25–50% *above* static, which is exactly why P0 exists). Fully
explained, not just noted: §3.3's 126 c/f bundles two things this P0 rig
deliberately excludes. (1) It assumed "5 plain operators + 1 self-feedback,"
but the actual topology (§"FM P0 Rig (#42)" above) is 4 first-writer
operators + 1 modulated `op_render` + 1 `op_render_fb` — `op_render_first`
is cheaper (12 compiled instructions vs. 13, #42's assembly extraction), so
4 of the 6 operators cost less than §3.3 assumed. (2) §3.3's ~19 c/f/voice
"per-block overhead amortised (6× EG step + exp2, LFO, pitch EG, bus setup)"
has nothing to measure here — P0's rig has no EG, no LFO, no pitch EG by
design (`fm.md` §1's P0 scope). Recovering per-operator costs by fitting the
`FM_RIG_FB=0` vs. `FM_RIG_FB=1` delta below to the compiled instruction-count
ratios (12:13:14.5 for first-writer:plain:self-feedback) gives **first ≈
15.6 c/f, plain render ≈ 16.9 c/f, self-feedback ≈ 21.3 c/f** — matching
§3.2's original hand-analyzed 16–18 / 21–23 c/f predictions closely. §3.2's
*per-operator* numbers were right; §3.3's *per-voice total* was only off
because of the topology miscount and the not-yet-existing EG/LFO line item,
both now accounted for.

### Lever bake-off (all at `FM_RIG_VOICES=16`, one lever changed per row vs. the table above's 100.5 c/f/voice baseline)

| Lever | Duty | c/f/voice | Δ vs. baseline |
|---|---|---|---|
| Baseline (all defaults) | 47.3% | 100.5 | — |
| Interleaved pair (`FM_RIG_INTERLEAVE=1`) | 46.57% | 99.0 | **−1.5 c/f (−1.5%)** |
| SRAM-resident kernel (`FM_RIG_NOT_IN_FLASH=1`, original rig) | 47.3% | 100.5 | **0 — lever didn't engage, see below** |
| BLOCK=8 | 44.97% | 95.6 | **−4.9 c/f (−4.9%)** |
| BLOCK=32 | 52.41% | 111.4 | **+10.9 c/f (+10.8%)** |
| 1024-entry table (`FM_RIG_TABLE_BITS=10`) | 47.3% | 100.5 | **0** |
| `smulwb` fusion | 45.9% | 97.6 | **−3.0 c/f (−3.0%)** |
| Plain vs. self-feedback (`FM_RIG_FB=0`) | 45.2% | 96.1 | **−4.5 c/f** (isolates the fb premium: 21.3 − 16.9 ≈ 4.4, matches) |

Two results need explaining, not just recording:

**`FM_RIG_NOT_IN_FLASH` measured zero effect — because the lever didn't
actually engage.** Checked directly: `arm-none-eabi-objdump -h` on both
builds showed an identical `.text` section (size and load address) whether
`FM_RIG_NOT_IN_FLASH` was 0 or 1. Cause: `op_render()` etc. are `inline`
functions that fully inline into `audio_engine_run()` at `-O3` (the same
inlining #42's assembly-extraction section relied on). `__not_in_flash_func`
places a function's *out-of-line* code in a linker section; once GCC inlines
the body away, there is no separate symbol left for the attribute to apply
to, so it does nothing — confirming the code really was always executing
from flash (`.text` sits at `0x10000000`, RP2350's XIP range; SRAM starts at
`0x20000000`).

**Fixed and re-measured, tests 14/15** (`tools/fm_rig_sram_retest.sh`,
`breadboard_rp2350`, 2026-08-08). `rig.h` now uses the pico-sdk's own
`__no_inline_not_in_flash_func` for `FM_RIG_NOT_IN_FLASH=1` — the SDK
documents this exact inlining trap and ships the fix (adds `noinline` so
there's a real symbol for the section-placement attribute to act on).
Device-verified the placement genuinely changed this time: `nm` shows
`op_render`/`op_render_first`/`op_render_fb` at `0x2000....` (SRAM) for
`=1`, vs. `0x1000....` (flash) for the new `=2` control (noinline, still
flash — isolates the SRAM-vs-flash effect from the call/return overhead
`noinline` itself adds, which the fully-inlined default never paid):

| Build | Duty | c/f/voice | Δ vs. inlined-flash baseline (100.5) |
|---|---|---|---|
| 14: noinline, flash (control) | 51.83% | 110.2 | **+9.7 c/f (noinline cost alone)** |
| 15: noinline, SRAM | 54.13% | 115.1 | **+14.6 c/f (noinline + SRAM)** |

**Isolated SRAM-vs-flash effect (15 − 14): +4.9 c/f/voice — SRAM is *more*
expensive, not less.** Backwards from the naive "SRAM is faster than flash"
assumption `fm.md` §3.6 item 2 built the "non-negotiable" framing on.
Explained, confirmed by evidence rather than asserted: `nm` on the `=1`
build shows three linker-generated veneer stubs
(`___Z9op_renderR7FmRigOpm_veneer` and two siblings) that the `=2` build
does not have at all. Flash sits at `0x10000000` and SRAM at `0x20000000` —
a ~256 MB gap, outside a Thumb `BL`'s encodable range, so every call from
the still-flash-resident render loop into the SRAM-placed kernel must
detour through an indirection the same-region flash call never pays. RP2350
also XIP-caches flash reads, and this kernel is small and reused every
sub-block — exactly the case where the cache erases most of flash's
latency disadvantage, leaving the veneer indirection as a net cost with
nothing to offset it.

**Decision: keep the kernel inlined in flash.** Not just "SRAM measured
worse than the noinline-flash control" — inlined-flash (today's actual
default, `FM_RIG_NOT_IN_FLASH=0`) beats *both* noinline variants by a wide
margin (100.5 vs. 110.2/115.1), since inlining also removes the call/return
overhead entirely. There is no configuration in this data where moving the
kernel out of flash helps; `fm.md` §3.6 item 2 is closed against the
opposite of its original assumption.

**Caveat, `fm.md` open question 10.** All of the above was measured with
Core0 doing essentially no flash-side work — MIDI/LCD/control are still
stubs. RP2350's 16 KB XIP cache is one shared resource for both cores
(`hardware_xip_cache.h`), so this margin isn't guaranteed once Core0 has
real LCD/MIDI/control traffic that can evict the FM kernel's cache lines
right when Core0 is busiest — exactly the timing where an audio glitch
would be most noticeable, and exactly what this measurement, run with a
quiet Core0, could not catch. Not retested here since there's no real Core0
workload yet to contend against. Mitigation on hand if it turns out to
matter: `xip_cache_pin_range()` (RP2350-only) permanently reserves the
kernel's flash range against eviction by anything else. If that doesn't pan
out, SRAM's "measured worse" verdict above was itself measured in
isolation — SRAM sidesteps this specific shared-cache problem entirely (its
own contention risk is per-bank and controllable), so it remains a fallback,
not a closed door.

**BLOCK direction is inverted from `fm.md` §3.6 item 3's framing, and the
cause is unrolling, not per-block amortisation.** §3.6 assumed larger BLOCK
saves cost by amortising per-block overhead — but that overhead (EG step,
exp2, LFO) doesn't exist in this rig (same reason as the voice-sweep
discrepancy above), so there was nothing for a larger BLOCK to amortise.
What actually happened: `arm-none-eabi-size` on the compiled
`audio_engine.cpp.o` shows `audio_engine_run()` at 3,172 bytes (BLOCK=8),
**5,568 bytes (BLOCK=16, the largest of the three)**, and 1,336 bytes
(BLOCK=32) — and disassembly confirms why: at BLOCK=32 the per-operator
sample loop compiles to a real branching loop (`bne.n`, 13-instruction body,
exact match to §3.2's hand-analyzed listing), while BLOCK=8 and BLOCK=16 get
substantially unrolled by GCC (no backward branch in an isolated,
`noinline`-wrapped probe of the same loop), eliminating the per-sample
loop-control cost that BLOCK=32 keeps paying. This is a compiler
unrolling-threshold artifact specific to this EG/LFO-free rig, not a
property of BLOCK size itself — once P2 adds the real per-block control-rate
work, that will reintroduce genuine amortisation and could shift the balance
back. Recorded as the "operator-cost side of the trade" `fm.md` asked P0 to
settle; final confirmation is still P2's, as already planned.

The interleaved-pair result (−1.5%) is real but far smaller than §3.6 item
1's "likely the single largest win" expectation. Plausible explanation, not
confirmed by disassembly: op0/op1 are called as two sequential
`op_render_first()` invocations even at `FM_RIG_INTERLEAVE=0`, and once both
fully inline into `audio_engine_run()`, GCC's own instruction scheduler has
the same independent-load-use-stall visibility across that boundary that the
hand-written interleaved kernel provides deliberately — so much of the
win may already be captured by the compiler before the lever is even
applied.

Table size (0) and `smulwb` (−3.0%) landed exactly as predicted: §3.5's
"identical instruction count" claim for 4096 vs. 1024 holds, and the DSP
fusion buys a small, real win with no correctness cost (already host- and
device-verified in #42).

### Decisions

- **`MAX_VOICES = 16`, confirmed** (was `fm.md` §3.4's provisional plan
  value since #41). Projected real per-voice cost = measured kernel (100.5
  c/f) + `fm.md` §3.3's still-unmeasured ~19 c/f EG/LFO reservation (P2
  scope, out of reach for this EG/LFO-free rig) ≈ **119.5 c/f/voice**.
  Against the FX-corrected budget (2607 c/f, below), 16 voices costs 1,608
  c/f — a comfortable ~27% margin even before any credit for the P0 kernel
  number beating its own static estimate. The projection also clears §3.4's
  ≤130 c/f "20+ voices" threshold, but that number leans on an unverified P2
  estimate rather than a bench reading, so raising `MAX_VOICES` past 16 is
  deferred to a P2 bench pass once `EnvDX`/LFO exist to measure for real,
  per §3.4's own "surplus is spent on polyphony, not features" guidance —
  not decided here on a projection.
- **BLOCK = 16, provisional** (unchanged from `fm.md`'s assumption). The
  kernel-only measurement doesn't clearly favor a change: BLOCK=8 is 4.9%
  cheaper but BLOCK=32 is 10.8% more expensive, and both effects are
  compiler-unrolling artifacts of this EG-free rig rather than the
  per-block amortisation §5.3's actual BLOCK/EG-resolution tradeoff is
  about. Final call stays P2's, against real rate-99 attacks, as `fm.md`
  already planned.
- **Kernel form: plain, not interleaved.** −1.5% doesn't justify the
  two-operand interleaved kernel's added complexity (only valid for
  mutually-independent operand pairs, more code paths in the eventual
  routing compiler). `op_render_pair()` stays in `rig.h`, unused by the
  decision.
- **Self-feedback stays "always on," no cheap fallback needed.** Measured
  premium (~4.4–4.5 c/f, isolated two ways: the FB=0/FB=1 rig delta and the
  fitted per-operator decomposition) matches `fm.md` §3.3's assumed 22-vs-17
  budget closely (fitted: 21.3 vs. 16.9). No surprise here to explain.
- **Keep the kernel inlined in flash — do not move it to SRAM** (tests 14/15,
  above). Isolated SRAM-vs-flash effect is +4.9 c/f/voice *worse*, not
  better (linker veneers on every call crossing the flash→SRAM gap, with no
  offsetting win since RP2350's XIP cache already erases most of flash's
  latency disadvantage for a small, reused-every-block loop like this one).
  Inlined-flash beats both noinline variants outright regardless of
  placement. Closes `fm.md` §3.6 item 2 against the opposite of its
  original "non-negotiable" assumption.
- **Freeverb stays.** Real cost is 268.7 c/f (7.9%), not the ~150 c/f (4.4%)
  reservation, but the 16-voice budget still clears with margin (above) —
  see the FX-insert section below for the full number.

`fm.md`'s provenance caveat, §3.4, and open questions 1–2 are updated to
match — see `fm.md` directly.

The `FM_RIG_FB` lever, the CMake/Makefile plumbing for it, and the isolated-
plain-vs-feedback topology fork in `fm_rig_render_voice_block()` did not
exist before #43 — #42's fixed topology could exercise `op_render_fb()`
correctly but had no way to A/B it against a plain operator in the same
chain position. Host-verified (`render_fm_rig`, direct compile-flag sweep):
`FM_RIG_FB=0`, `FM_RIG_FB=0 FM_RIG_INTERLEAVE=1`, and the `FM_RIG_FB=1`
default all pass the existing bounded/non-silent checks. Device-verified:
`make ENGINE=fm FM_PROFILE=1 FM_RIG_FB=0` builds clean, and a combined-
levers build with `FM_RIG_FB=0` added to #42's existing combination
(`FM_RIG_INTERLEAVE=1 FM_RIG_TABLE_BITS=10 FM_RIG_BLOCK=8 FM_RIG_VOICES=32
FM_RIG_NOT_IN_FLASH=1 FM_RIG_SMULWB=1`) builds clean at 29,144/18,296/47,440
(text/bss/dec) — smaller than #42's all-levers-combined 29,104/18,296/47,400
by the expected margin (`op_render()` is shorter than `op_render_fb()`, and
that's the only thing this lever changes). The default `FM_PROFILE=1` build
(all levers unset, `FM_RIG_FB` implicitly 1) reproduces #42's exact
32,080/23,064/55,144 byte-for-byte, and the plain `make ENGINE=fm` skeleton
(no `FM_PROFILE`) reproduces #41's exact 27,784/202,768/230,552 — confirming
this slice changed nothing observable about either existing build.

## FM Engine — EnvDX + BLOCK Confirmation (#45)

`src/engines/fm/env_dx.h` (new): the DX7 envelope (`fm.md` §5.3, P2's gate).
Deliberately not `envelope.h`'s ADSR (`fm.md`: "Do not reuse `envelope.*`")
-- 4 x (rate, level) stages per operator, direction-agnostic (any stage can
ramp up or down to any target), stepped once per control block, log domain
throughout with a single exp2-table conversion to linear at each block
boundary. `op_render`/`op_render_first`/`op_render_fb` (op.h) are
byte-for-byte unchanged from #44 -- the EG only decides what `gain`/
`gain_step` are at each block boundary; the per-sample kernel still just
does `gain += gain_step`, confirmed against the emitted assembly below.

### Fixed-point design

Everything lives in a single "log2 offset from an operator's reference
level" domain (Q iiii.8, `EG_LOG2_FRAC_BITS = 8`, matching the 256-entry
exp2 table 1:1, no interpolation needed). Three independent 0-99 DX7
parameters resolve into this domain and simply add:

- **Operator output level (TL)**, resolved once at note-on.
- **Each EG stage's target level**, looked up fresh every stage transition.
- **Velocity sensitivity** (0-7), resolved once at note-on: 0 at
  sensitivity 0 (regardless of velocity) or at max velocity (regardless of
  sensitivity); increasingly negative for softer hits on a more sensitive
  operator (~24 dB / 4 octaves of range at sensitivity 7, velocity 0).

Level 0 (both TL and an EG stage target) maps to a deliberately deep floor
(-40 octaves) rather than merely "very quiet" -- deep enough that
`eg_to_linear()` underflows to an *exact* int32 zero for any valid
reference (references are always < 2^31; 2^31 x 2^-40 < 1). This is what
makes "a voice reports itself free only when its carriers have actually
decayed" a real guarantee: `EnvDX` has a genuine terminal `EG_IDLE` state,
reached only by completing the release stage, not an epsilon-on-a-
decaying-value guess -- the tracker's #21 bug ("key-off never frees a
voice") was exactly a missing version of this guarantee, in a different
engine.

The level curve (both TL and EG stage levels) and the rate curve (0-99 ->
octaves/second, independent of BLOCK size) are honest approximations of
the real DX7's general shape -- linear-in-dB level curve (~96 dB across
1-99), exponential rate curve (~20s slowest full sweep, ~6ms fastest) --
not a byte-exact reproduction of Yamaha's hardware tables. Exact
replication is P3/P6 territory, once real `.syx` patches exist to compare
directly against Dexed.

### Host-verified (`tools/host_render/render_fm`, extended)

- **Level table**: level 99 = the reference exactly; level 0 = an exact
  digital 0; monotonic across 0-99; level 50's linear gain is far below
  `reference * 50/99` (a real nonlinear/log curve, not "a linear
  approximation" -- an explicit acceptance criterion).
- **Velocity sensitivity**: sensitivity 0 is a true no-op at any velocity;
  sensitivity 7 is real and monotonic (softer hits quieter, more so at
  higher sensitivity).
- **Six independent EGs**: stepping `FM_TEST_PATCH`'s real (non-flat) EGs
  block-by-block and reading each operator's `gain` directly (not the mixed
  audio, which can't cleanly attribute a level to one operator) --
  op4 (the modulator driving the carrier, EG level `{99,20,15,0}`) decays
  to 3.2% of its own 5ms level by 800ms; op5 (the carrier, `{99,70,60,0}`)
  *grows* to 4.9x its own 5ms level over the same window (still mid-attack
  at 5ms) -- and all six operators land at six distinct gains at 800ms.
  This is `fm.md`'s DX-EP shape (bright pluck settling into a mellower
  sustain) confirmed in the actual per-operator numbers, not just eyeballed
  from a mixed WAV.
- **Release**: a held note stays active through release (no #44-style hard
  cutoff), goes idle (`EG_IDLE`) 783 ms after note-off at `FM_TEST_PATCH`'s
  rates, and the carrier's `gain`/`gain_step` are both an exact 0 at that
  point -- not merely below some threshold.
- **Routing/spectrum** (#44's checks, re-verified against a "flat EG"
  variant of `FM_TEST_PATCH` -- same routing/ratios/levels, EG jumps to and
  holds at full immediately -- so the routing claim is measured in
  isolation from #45's deliberately fast-decaying real EG shape): unchanged
  from #44's PASS.

### Kernel identity, confirmed against the emitted assembly

`make ENGINE=fm` (real, non-profiling build) compiles clean;
`arm-none-eabi-objdump` on `audio_engine.cpp.o` shows the same 48 `smlawb`
instances as #44's build, and the per-sample body is unchanged in shape:

```
adds  r4, r6, r2      @ phase += inc (register-carried across the unrolled block)
add   r0, r4          @ + modulation bus value
lsrs  r0, r0, #20      @ table index -- unchanged
ldrsh.w r6, [r5, r0, lsl #1]  @ table lookup -- unchanged
add   lr, r7           @ gain += gain_step -- the ONE add #45 adds nothing to
smlawb r6, ip, r6, sl   @ x gain -- unchanged
add.w r0, r0, r6, asr #6  @ accumulate/store -- unchanged
str   r0, [r3, #0]
```

Exactly one extra `add` per sample versus #44 (`add lr, r7` / `add ip, r7`,
feeding the running gain register into the next `smlawb`) -- `fm.md` §5.3's
"per-sample cost in the kernel is one add" claim, confirmed against real
compiled output, not assumed from the source.

### BLOCK Confirmation (fm.md open question 3, closed)

`op.h`'s `FM_BLOCK` is now a compile-time override (`T00T_FM_BLOCK`, wired
through `CMakeLists.txt`/`Makefile` as `make ENGINE=fm FM_BLOCK=8|32`, same
convention as `DMA_BUFFER_SIZE`) -- distinct from `FM_RIG_BLOCK`, which only
affects the #42 profiling rig. Compared via a standalone host probe
stepping a single carrier operator (R1=99, the fastest DX7 attack) through
`env_dx_step_block()` at each candidate BLOCK size, recording gain at every
block boundary:

| `FM_BLOCK` | ms/block | Steps to 99% of full scale | Time to 99% | vs. BLOCK=16 |
|---|---|---|---|---|
| 8 | 0.181 | 34 | 6.17 ms | smoothest, most accurate |
| 16 | 0.363 | 17 | 6.17 ms | baseline |
| 32 | 0.726 | 9 | 6.53 ms | coarsest, 8% timing overshoot |

BLOCK=32 loses on both axes now measured -- #43's kernel-only bench already
found it 10.8% more expensive, and this reading adds a real timing/
granularity cost on top (fewest distinct gain steps across the fastest
attack, and the only one of the three whose "reached target" check lands
measurably late). Ruled out.

BLOCK=8 edges out BLOCK=16 on both the #43 kernel cost (4.9% cheaper) and
this reading (smoothest ramp, no timing overshoot) -- but #43's rig has no
EG, and BLOCK=8 means twice as many `env_dx_step_block()` calls per second
as BLOCK=16 (a 64-bit divide plus a handful of table lookups per operator,
6 operators per voice). A back-of-envelope projection (`fm.md` §3.3's own
~19 c/f/voice BLOCK=16 EG-overhead estimate, doubled for BLOCK=8 ≈ 37.5
c/f/voice, ×16 voices ≈ +300 c/f) plausibly outweighs BLOCK=8's ~78 c/f
total kernel saving (16 x 4.9% of 100.5 c/f) -- but this is a projection,
not a bench reading; #43's own "surplus isn't spent on a projection"
discipline applies here too.

**Decision: BLOCK=16 confirmed, not changed.** Neither the smaller nor
larger candidate is a clear-cut win once EG overhead exists to weigh
against kernel cost, and BLOCK=32's regression on both axes is unambiguous
enough to rule out outright. Revisit BLOCK=8 with a real profiling-pin
reading (not this projection) if a future issue wants to chase it.

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

## syx2patch Host Converter (#47)

`tools/syx2patch.py` — a pure-Python, stdlib-only tool (no CMake project,
same as `xm2t00t/`) that turns a real DX7 32-voice `.syx` bulk dump into
`src/engines/fm/patches.h`. Per fm.md §7, the device never parses DX7
sysex — this runs once, offline. Unlike `xm2t00t`/`speechgen.py`, most of
the actual DSP conversion work was *already done* by #45's `env_dx.h`
(rate/level tables, log-domain conversion, all resolved at runtime from raw
0-99 bytes) — this converter's real job is almost entirely structural: unpack
the bit-packed sysex format correctly, and turn a DX7 algorithm number into
`patch.h`'s `mod_target`/`feedback` shape.

```
syx2patch.py convert <in.syx> <out.h>   # writes patches.h (enum FmPatchId,
                                          # FM_PATCH_COUNT, const FmPatch
                                          # FM_PATCHES[], FM_PATCH_NAMES[])
syx2patch.py dump <in.syx>               # prints a per-voice summary
                                          # (algorithm, name, warnings), writes nothing
```

### Algorithm decode

`DX7_ALGORITHMS[32]` is Dexed's own bus-flag table (Source/msfa/fm_core.cc,
Apache-2.0), reused as data — not because Dexed's 2-bus render scheme is
copied, but because it's a compact, already-correct encoding of all 32
algorithms' topology. `decode_algorithm()` simulates DX7's fixed OP6→OP1
processing order once, generically, to reconstruct each operator's real
`mod_target`: a bus (1 or 2) is a stack, written by a contiguous run of
operators, and the next operator that reads that bus number is,
unambiguously, every writer in that run's actual downstream target. One
function handles all 32 rows — the acceptance criterion "generated or
table-driven, not 32 hand-written cases" is met by the data being a table
and the decode being one generic simulation, not per-algorithm branches.
Hand-verified against algorithm 1 (two independent chains,
`6→5→4→3→OUT`/`2→1→OUT`, feedback on OP6) and algorithm 32 (all six
operators as independent carriers, feedback on OP6) — both match every
published DX7 algorithm chart.

Algorithms 4 and 6 each have a second operator with the FB_OUT bit set
alone (not paired with FB_IN, unlike the algorithm's primary feedback
operator) — real DX7 hardware routes that operator's output into the same
shared feedback register the primary operator reads, a genuine
operator-spanning loop closed only across block-processing order. Detected
generically (not by hardcoding "algorithm == 4 or 6"): a test graph adds a
tentative edge from every such secondary operator to the primary, and a real
cycle check runs over it. `needs_interleaved` is set when a cycle is found;
the fallback is simply that the secondary edge was never added to the real
emitted routing in the first place — `patches.h` gets a per-patch comment
and the tool logs a warning, `#54` is the eventual real fix.

### Packed-voice unpacking

`unpack_voice()` is a bit-for-bit port of Dexed's
`Cartridge::unpackProgram` (Source/PluginData.cpp, Apache-2.0), cross-checked
against the DX7 MIDI Data Format Sheet's own published "Bulk Dump Packed
Format" table. Every field is range-checked against its documented range
(0-99, 0-31, 0-7, …) and raises rather than silently clamping — unlike
Dexed's own `normparm`, which clamps corrupted bytes defensively since it
has to keep running against whatever's already loaded. `parse_syx_bulk()`
validates the full `F0 43 0n 09 20 00 … checksum F7` envelope, including the
masked 2's-complement checksum.

### Two bugs caught by actually rendering a real bank, not by design review

1. **Multi-carrier int16 overflow.** `FmOpParams::level` (the reference-gain
   ceiling — not DX7 data, see `patch.h`'s own comment) was set flat per role
   using `FM_TEST_PATCH`'s own precedent (`FM_CARRIER_LEVEL_REF = 1<<21`,
   `FM_MODULATOR_LEVEL_REF = 1400000000`). `FM_TEST_PATCH` only ever has one
   carrier, so this was never exercised; real DX7 algorithms 19-32 sum 3-6
   carriers into one voice, and each at the same flat reference clipped
   badly (measured on ROM1A: BRASS 1's peak hit 43185, E.ORGAN 1's 46675,
   both well past int16's 32767). Fixed by scaling each carrier's reference
   by `1/carrier_count` — N summed carriers at output_level=99 land back at
   the same envelope one carrier alone would, exactly mirroring how
   `output_level` already carries the real per-patch balance.
2. **Carrier release level never reaching idle.** A carrier operator with a
   nonzero EG L4 (release-stage target) never reaches `env_dx.h`'s
   `EG_IDLE` — the exact shape of the tracker's #21 "key-off never frees a
   voice" bug, just reached through DX7 patch data instead of engine logic.
   `convert_voice()` forces carrier L4 to 0 with a logged warning; the
   audible cost is that a handful of sustained-pad-style patches release
   fully instead of holding a residual level, since this engine has no
   sustain-pedal semantics to make that distinction meaningful anyway.

### Patch select

Behind `T00T_FM_HAS_PATCHES` (both `CMakeLists.txt`s — see below):
`midi_controller.cpp` wires Program Change and CC30 (the BeatStep Pro can't
reliably send real Program Change, same reasoning #36 gave speech's
phrase-bank CCs) to `FM_PATCHES[value % FM_PATCH_COUNT]`. Without
`patches.h`, every voice still plays `FM_TEST_PATCH`, unconditionally,
exactly as before #47.

### Gitignored, not wired into CMakeLists.txt unconditionally

Both the `.syx` input and the generated `patches.h` are gitignored — a real
DX7 bank is Yamaha's own commercial patch data, the same policy `xm2t00t`'s
`xm/` already established for copyrighted `.xm` songs. `patches.h`'s
presence is detected at configure time (`if(EXISTS …)`, same pattern the
top-level `CMakeLists.txt` already uses for other optional generated files)
and gates `T00T_FM_HAS_PATCHES` in both the device build and
`tools/host_render`'s `render_fm` target — the build succeeds either way.

### Validation

`python3 tools/test_syx2patch.py` — unit checks (all 32 algorithms decode
and only 4/6 flag interleaved, algorithm 1/32 topology by hand, coarse/fine
ratio formula, fixed-frequency skip, carrier L4 forcing, feedback-level-0
exactness, multi-carrier level scaling, bit-packing round-trip on a
synthetic voice, checksum/header rejection paths) always run; corpus-
dependent checks run against whatever `.syx` files are in `../syx/`
(gitignored, same as `xm/`) and skip cleanly if empty. Verified against
ROM1A and ROM1B (2026-08-08): 28/32 and 31/32 voices converted respectively
(skips: fixed-frequency-mode patches — ROM1A's "TUB BELLS"/"STEEL
DRUM"/"REFS WHISL"/"TRAIN", ROM1B's "PIPES 2" — all deferred to #48); ROM1B
exercises the algorithm 4/6 fallback for real ("CLAV 2"/"CLAV 3"/"PIPES 4").
`render_fm`'s patch-bank render (host-only, `T00T_FM_HAS_PATCHES`-gated)
renders a 3-second A3 note per converted patch to `fm_patches/*.wav` and
checks every one is bounded and non-silent — all 28 of ROM1A's pass, 5488 B
total flash cost (196 B/patch, matching fm.md §8's ~200 B/patch estimate).
`make ENGINE=fm` builds clean both with and without `patches.h` present.

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
