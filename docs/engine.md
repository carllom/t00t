# T00T Audio Engine Architecture

Dual-core architecture and infrastructure shared by every synthesis module.
For a specific module's synthesis spec, see `module_<name>.md`; for its
development history, `history_<name>.md`. The subtractive engine — the first
module, predating this split — has its module-specific spec in
`module_subtractive.md` and its measurement history in
`history_subtractive.md`.

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
    on press/release: value = shape_button_event(SensorEvent, button's ButtonShapingConfig)
                       midi_controller_dispatch_note(shadow, note, value)
  if changed: params->commit()          // flip param double-buffer
```

Each button's debounced edge becomes a `SensorEvent` (`src/sensor_event.h`),
Shaped into an `InputValue` via `shape_button_event()` (`src/button_shaping.h`
-- supplying the button's fixed note, channel, and (on press) a fixed
velocity a bare switch has no signal for, `value.voice` left unresolved),
then dispatched through `midi_controller_dispatch_note()` into
`subtractive`'s shared `kMappingTable`/`set_note` Handler, the same one
MIDI note-on/off drives. `set_note()` itself is where
`voice_alloc_allocate()`/`release()` actually get called -- never
upstream in `controller_tick()` or the MIDI parsing/dispatch loop, see the
Voice Allocation Interface entry below.

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

"Transition ADSR to release phase" is `Envelope::release(cfg)`'s default
behavior — immediate, from whatever stage the envelope is in. `EnvConfig`
also carries a `gated_attack_decay` option (default `true`, matching that
default): set false, a release requested during attack or decay is deferred
until they finish naturally instead of interrupting them. No preset sets it
false today.

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
Each button plays one fixed note, on its own channel. Voice is allocated on
press, released on release. Long release (800ms) ensures multiple voices
can be heard simultaneously.

## Host DSP Tooling

`tools/host_render/` (issue #5 phase 2) is a standalone CMake project — no
pico-sdk, host compiler — for verifying pure-DSP common-layer headers by
rendering them to WAV instead of on real hardware. `make host` configures and
builds it into `tools/host_render/build/` (git-ignored, same as the device
`build/`); binaries there also write their WAV output alongside themselves.

This exists so tracker/speech/FM module work has a host-render harness to
build on (module_speech.md calls this "the single most valuable test"), and so new
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
before/after diff, is module_speech.md P0, done independently once speech work
starts.

## MIDI Input

Control comes from buttons (VGA board only) and MIDI. There is no intermediate
event queue — each input source writes the param shadow and commits directly.

- **USB MIDI** (`MIDI_USB`): TinyUSB device; `usb_midi_task()` runs the stack and
  `usb_midi_poll()` feeds received bytes to the transport-agnostic MIDI controller.
- **DIN/UART MIDI** (`MIDI_UART`): UART1 RX at 31250 baud. An IRQ fills a ring
  buffer; `uart_midi_poll()` drains it each main-loop pass. Default-on for the
  breadboard (GPIO5); off for the VGA board, where GPIO5 is SD_CLK.

Every engine provides its own `src/engines/<name>/` MIDI routing file
(`CMakeLists.txt` picks it by `T00T_ENGINE` — no fallback to a shared
file; `src/midi/` holds only genuinely module-agnostic code, never a
per-engine implementation). A migrated engine's file is named
`input_subsystem.cpp` — it's no longer MIDI-only, serving non-MIDI
sources like GPIO buttons too (see below); one not yet migrated onto the
Router keeps the older `midi_controller.cpp` name until its own migration
ticket. Both transports route through `midi_controller_process()`, which
parses MIDI bytes and dispatches each message -- never resolving a voice
itself; see the Voice Allocation Interface entry below. Every engine --
`subtractive`, `fm`, `chip`, `tracker`, `groovebox`, `speech` -- builds
this as a one-line call into `midi_controller_process_generic()`
(`src/midi/midi_controller_generic.h`), built from
`src/midi/midi_dispatch.h`'s shared, module-agnostic generic helpers — one
per MIDI message shape (`midi_dispatch_cc()`, `midi_dispatch_pitch_bend()`,
`midi_dispatch_program_change()`, `midi_dispatch_note()`,
`midi_dispatch_clock()`, `midi_dispatch_transport()`). A module's own
`input_subsystem.cpp` shrinks to its `kMappingTable`, its
Handlers (including voice resolution, inside its own NOTE Handler), and
that one-line call; `tracker` has no `NOTE`/`MODIFIER` entries at all (no
live-note traffic, per module_tracker.md), which is fine — an unmatched
category/id is always a no-op, same mechanism, just an empty table region.

For `subtractive`/`fm`/`chip`, note on/off, **pitch bend**, per-module CCs
(mod wheel/pan/instrument-select/speaker-sim as applicable), **CC72-75 (FX
type/mix/param1/param2)**, and **Program Change** (patch/preset select)
all route through a shared per-module mapping table and the generic Router
(`src/input_layer.h`, `input_dispatch()`) — Program Change carries only
its raw 0-127 program number, dispatched under `CONFIGURATION` with a
synthetic id (`MIDI_CONFIG_ID_PROGRAM`, `src/midi/midi_dispatch.h`) shared
across every module using this convention, the same way pitch bend shares
a synthetic Modifier id. `tracker` uses the same `CONFIGURATION`/Program
Change path for a different purpose (seek to an order, not select a
preset — `CONFIGURATION`'s `value.index` field means whatever a module's
own Handler decides it means), plus **MIDI Start/Continue/Stop** dispatched
under a new `TRANSPORT` category (`midi_dispatch_transport()`,
`src/midi/midi_dispatch.h`) added specifically to fit it — Transport
carries no payload beyond which message it is (module-global, like
play/stop), so distinct Handlers per table entry (not one Handler reading
an id) tell them apart. **CC0/CC32 (bank select)** stay outside the Router
(neither needs channel/velocity matching) but are now module-agnostic,
always-linked state (`src/midi/midi_dispatch.cpp`) rather than per-module
duplicated arrays — a Handler that wants the currently selected bank
alongside a Program Change reads it via `midi_channel_bank_msb()`/`_lsb()`
directly (`subtractive`'s microKORG-numbering translator is the concrete
example).

GPIO buttons (VGA board only) reach the same `kMappingTable`/`set_note`
Handler through a parallel, non-MIDI path: `controller_tick()` turns a
debounced edge into a `SensorEvent` (`src/sensor_event.h`), Shapes it into
an `InputValue` via `shape_button_event()` (`src/button_shaping.h` --
supplying the button's configured note, channel, and a fixed velocity a
bare switch has no signal for), then calls
`midi_controller_dispatch_note()`, a thin wrapper exposing
`kMappingTable`/`input_dispatch()` to callers outside
`src/engines/subtractive/input_subsystem.cpp`.

The dispatch mechanism above (shared vocabulary and a per-module mapping
table, #86) started `subtractive`-only; a from-scratch redesign of the
whole input pipeline settled a fuller vocabulary and requirement set on top
of it without discarding the mechanism itself — see Decision Record entry 5
— and `fm`/`chip`/`tracker`/`groovebox`/`speech` have since joined
`subtractive` in also sharing `midi_controller_process()`'s own generic
message-dispatch shape, not just the Router underneath it. `groovebox`
reached that point last and by a different route: its channel-semantic
routing (drums/pattern-select/303, each picking a different category per
channel) didn't fit the shared loop's uniform `NOTE`/`CONFIGURATION`
dispatch, so it composed `midi_dispatch.h`'s lower-level helpers directly
in its own process loop for a while, until remapping its inputs (Decision
Record entry 9) removed that barrier. `speech` fit the generic loop's
shape (dynamic `voice_alloc`, one voice per note, same as
subtractive/fm/chip) directly, no fork ever needed — its own remap
unified Program Change with a redundant preset-select CC instead (see
module_speech.md's Decision Record). Non-MIDI input sources beyond GPIO
buttons (e.g. potentiometers -- `SensorEvent` already covers the shape,
see `src/sensor_event.h`) remain open work.

### Input categories

Note, Strike, Modifier, Configuration, Clock, and Transport are the category
vocabulary a source event is classified into before reaching a module's
mapping table. Definitions live in root `CONTEXT.md`'s "Language" section,
the single source of truth for this vocabulary — not duplicated here, to
avoid a second copy drifting out of sync as the vocabulary evolves. Every
engine (`subtractive`, `fm`, `chip`, `tracker`, `groovebox`, `speech`) is
built against it via the generic dispatch mechanism now — `groovebox` uses
`Configuration`/`Note`/`Modifier`/`Clock`/`Transport`, the richest table so
far, all through the same shared loop every other engine uses (see
Decision Record). `Strike` remains part of the vocabulary but currently has
no user: `groovebox`'s drums were its original motivating case, remapped
onto `Note` (see Decision Record entry 9) once channel-based branching
moved from the process loop into the Handler itself.

Each module declares which categories it supports via a compile-time
capability list, checked against its own mapping table with a
`static_assert`. Whether a value has been converted to a module-native unit
before a setter sees it depends on the category and the setter itself — see
CONTEXT.md's Shaping vs. Normalization entries; it is not "never raw," as an
earlier version of this design assumed. Configurability is build-time only
(no persistence mechanism exists in this codebase).

## Decision Record

1. **Input layer shares vocabulary and dispatch mechanism, not routing** —
   `docs/logs/history_groovebox.md:618-771` documents a "shared MIDI shell +
   per-engine routing hook" design that was planned then abandoned, because
   per-module routing *logic* diverges too much to share via a hook (only the
   ~60-line parser, now `midi_parser.h`, earned its sharing). The input layer
   (#84) deliberately shares less than that attempt: category vocabulary and
   a generic "walk a per-module table, call a setter" mechanism only — mapping
   tables and setters stay exactly as forked as they are today.
2. **No new event queue** — dispatch is synchronous, direct calls (source →
   generic dispatch → per-module setter → shadow commit), preserving the "no
   intermediate event queue" invariant in MIDI Input above.
3. **"Strike," not "Trigger"** — `trigger` already names the per-voice
   generation counter in every module's `VoiceParams` (see #83), so the new
   generic-discrete-input category needed a different word to avoid the two
   meaning different things at different layers in the same codebase.
4. **`tracker` and `groovebox` excluded from generic Note/Strike** — `tracker`
   has no note traffic at all; `groovebox`'s routing is channel-semantic
   (fixed drum/pattern-select/mono-303 map), not "one voice per note."
   Forcing either through the generic Note/Strike dispatch would recreate the
   mismatch that sank the routing-hook design in decision 1. (Both later
   migrated onto the Router anyway, without contradicting this: what stayed
   excluded was the *shared loop*, never the category vocabulary or the
   Router mechanism itself — see decision 8 for `tracker`, decision 9 for
   `groovebox`, whose channel-semantic barrier was eventually removed by
   remapping its inputs rather than changing the mechanism.)
5. **A from-scratch input-pipeline redesign superseded #84/#85, without
   discarding #86's mechanism** — a wayfinder effort (issue #94, spec #99)
   revisited the whole Core 0 input pipeline with a goal the original design
   didn't have: decoupling dynamic voice allocation from MIDI parsing
   entirely, into a "Voice Allocation Interface" a module's own setter opts
   into and calls directly. It found no reason to replace `input_dispatch()`/
   `InputMapEntryT` — every new requirement (module-global Modifiers, a
   non-MIDI `SensorEvent` source, a self-expiring `Strike` pattern) fit on
   top of it — so that mechanism continues, extended rather than rebuilt.
   Confirmed decision 4 still holds by re-examining `groovebox`/`tracker`'s
   actual routing rather than assuming it. The full settled vocabulary lives
   in `CONTEXT.md`, not here.
6. **`midi_controller_process()` itself became shared, generic building
   blocks** (`src/midi/midi_dispatch.h`/`.cpp`), not just the Router
   underneath it — once `fm`'s freshly-migrated `midi_controller_process()`
   turned out nearly identical to `subtractive`'s, the remaining
   differences (a microKORG-specific Program-Change decoding, and
   per-module-duplicated bank-select storage) turned out to be Handler-
   local and generic-plumbing concerns respectively, not genuine routing
   divergence — so they didn't reopen decision 1's "routing stays forked"
   boundary. Program Change is now a standing convention (`CONFIGURATION`,
   a shared synthetic id) for every module built against the Router, not a
   per-module choice; CC0/CC32 bank-select storage is shared, always-linked
   state a Handler reads on demand.
7. **Voice allocation moved into `set_note()` itself**, correcting a
   deviation from spec #99's own Voice Allocation Interface design that
   had been present since the original #100-104 migration: the first pass
   at `midi_controller_process_generic()` (decision 6) called
   `voice_alloc_allocate()`/`release()` from *within* the generic dispatch
   loop, before a Handler ever ran — exactly the "interleaved inside event
   parsing" shape CONTEXT.md's Voice Allocation Interface entry and spec
   #99 both explicitly ruled out, just relocated one layer up from the old
   per-module `midi_controller_process()` rather than actually removed.
   `set_note()` now resolves its own voice via its own `note_voice[]`
   lookup (steal-on-retrigger included), so `midi_controller_process_generic()`
   needs no per-note/per-voice tracking arrays at all — it dispatches NOTE
   the same uniform way as every other category, `value.voice` left at -1
   (unresolved) for the Handler to fill in. `controller.cpp`'s button path
   was fixed the same way, dropping its own pre-allocation and
   `ButtonState::allocated_voice` entirely.
8. **`tracker` migrated onto `midi_controller_process_generic()` too**,
   despite having zero live-note traffic — its `kMappingTable` simply has
   no `NOTE`/`MODIFIER` entries, and an unmatched category/id is already a
   no-op by the Router's own contract, so nothing needed to change about
   the generic loop's NOTE/CC handling to accommodate that. What the loop
   *did* need, and gained specifically for `tracker`: generic `TRANSPORT`
   dispatch (`midi_dispatch_transport()`, Start/Continue/Stop), the first
   module to actually exercise that category. Program Change stays
   `CONFIGURATION` -- `tracker`'s own Handler just means "seek to an
   order" by it, not "select a preset," consistent with `CONFIGURATION`'s
   `value.index` field meaning whatever a module's Handler decides.
9. **`groovebox` fully adopted `midi_controller_process_generic()` by
   remapping its inputs, not by changing the mechanism.** Its routing was
   channel-semantic — channel alone decided whether a message was drums
   (dispatched as `Strike`, a fixed kit-voice lookup), pattern-select (a
   `NOTE_ON`-shaped message dispatched as `Configuration` instead, spec
   #99's own worked example for this exact case), or the TB-303 (`Note`,
   a fixed voice, not dynamic `voice_alloc`) — a shape the generic loop's
   uniform `NOTE`/`CONFIGURATION` dispatch couldn't express, so it
   composed `midi_dispatch.h`'s lower-level helpers directly in its own
   forked loop instead.
   Three remaps removed that barrier: drums now dispatch as plain `Note`,
   with `set_note()` itself branching on `value.channel` to reach the
   kit-lookup path (the branch moved into the Handler, where it always
   had to end up once the loop became uniform); pattern-select moved from
   the `NOTE_ON`-shaped message to a real Program Change on the pattern
   channel, dispatched as `CONFIGURATION` like every other module's
   preset select (`channel_filter`'s exact-match routes it to
   `set_pattern_select()` without colliding with a real preset select);
   and pitch bend's drum-channel exclusion moved from a loop-level
   channel check into `set_303_bend()` itself. The one piece that
   couldn't move into a remap — `MIDI_CLOCK` — was missing from
   `midi_controller_process_generic()` itself, not from `groovebox`'s
   routing, so it was added there directly (`midi_dispatch_clock()`, a
   no-op for every module with no `CLOCK` table entry, same as an
   unmatched `NOTE`/`MODIFIER` id; `midi_dispatch_strike()`, added
   alongside it for the drum path, was later removed once drums stopped
   using `Strike`). With all three settled, `groovebox`'s
   `midi_controller_process()` collapsed to the same one-line call every
   other migrated module uses; `patterns.h`'s `SEQ_PATTERN_BASE_NOTE`,
   orphaned by the remap, was removed too.
10. **`speech` migrated onto `midi_controller_process_generic()` directly,
    no fork ever needed** — unlike `groovebox`, its `NOTE`/`MODIFIER`
    dispatch already fit the generic loop's shape (dynamic `voice_alloc`,
    one voice per note, same as `subtractive`/`fm`/`chip`) from the start.
    The remap this module needed was a different kind: Program Change used
    to mean something different depending on the channel's current tract
    (select an utterance under formant/SAM, select a `KEY_PER_WORD` page
    under LPC lattice — a raw branch inside the `MIDI_PROGRAM_CHANGE` case
    itself), and CC16 duplicated Program Change's "load a preset"
    operation as a second, BeatStep-Pro-safe input path (same rationale
    `fm`'s CC30/`chip`'s CC16 were dropped for). Both resolved by
    recognizing a `KEY_PER_WORD` page *is* a preset — `presets.h`'s
    `SpeechPreset` already carried `tract`/`lattice_page` fields — so
    Program Change's Handler (`set_program()`) never branches on tract at
    all: it addresses `presets[]` and a compile-time-generated
    `lpc_page_presets` table (sized from `LATTICE_WORD_COUNT`, the
    gitignored, locally-generated corpus's own word count, or exactly one
    page without one) as one contiguous space, and CC16 was dropped as
    the now-fully-redundant path. All six engines — `subtractive`, `fm`,
    `chip`, `tracker`, `groovebox`, `speech` — now share
    `midi_controller_process_generic()`.
