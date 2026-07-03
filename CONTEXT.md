# T00T — Context

Polyphonic digital synthesizer firmware for the **RP2350 (Raspberry Pi Pico 2)**. An
experiment in real-time tone generation and classic-sound-chip emulation. Goal is
efficient, simple real-time synthesis — not audiophile quality.

## Hardware / boards

Two board targets, selected at build time via `BOARD=` (cmake caches it, so `make clean`
when switching):

| `BOARD` | Hardware | Buttons | DAC | Notes |
|---|---|---|---|---|
| `breadboard_rp2350` **(default)** | Pico 2 on breadboard | none (MIDI only) | PCM5122, I2S GPIO 16(BCK)/17(LRCK)/18(DIN) | Carl's actual rig |
| `vgaboard_rp2350` | Pimoroni VGA Demo + Pico 2 | A/B/C on GPIO 0/6/11 | PCM5100A | |

- Board headers live in [src/boards/](src/boards/); they are also included by the assembler,
  so they must contain **only preprocessor directives**.
- **Carl runs the breadboard board.** Build with plain `make`. Do NOT flash a vgaboard UF2
  to it — it produces silence.
- Profiling pin: GPIO 22 (scope probe; high while Core 1 renders a buffer).

## Build & flash

- `make` → `build/t00t.uf2` (breadboard). `make BOARD=vgaboard_rp2350` for the other.
- MIDI transport overrides: `make MIDI_USB=0` (DIN only) / `make MIDI_UART=0` (USB only);
  default `"default"` lets the board header decide (both on).
- pico-sdk/ and pico-extras/ are vendored at repo root (see [building.md](building.md) if missing).
- Flash: hold BOOTSEL, plug in, `cp build/t00t.uf2 /media/$USER/RPI-RP2/`.

## Architecture (dual-core)

- **Core 0** (main loop, [src/main.cpp](src/main.cpp)): polls MIDI (USB + UART), buttons;
  runs the voice allocator; writes voice parameters.
- **Core 1** ([src/audio_engine.cpp](src/audio_engine.cpp)): the synthesis engine. Blocks on
  the multicore FIFO for a DMA buffer-fill request, renders all 16 voices, interleaves to
  stereo (mono duplicated L/R), sends an active-voice bitmap back to Core 0.
- **Audio**: 44.1 kHz, 256 samples/buffer ([src/audio_common.h](src/audio_common.h)), I2S DMA
  out via pico-extras `pico_audio_i2s` ([src/output.cpp](src/output.cpp)).

### Cross-core parameter exchange (lock-free)

`ParamExchange` in [src/engine.h](src/engine.h) — double-buffered `VoiceParamBlock`s. Core 0
writes the shadow block, then `commit()` flips a single volatile byte (atomic) and `__sev()`s
Core 1. Core 0 never touches the committed block; Core 1 never touches the shadow. No locks.
`VoiceParams` carries only synthesis inputs (phase_inc, amplitude, waveform, LFO/filter
config…) — **no phase state**. Per-voice runtime state (phase, envelope, filter, LFSR) lives
in file-scope arrays in the audio engine, owned solely by Core 1. `trigger` is a generation
counter; Core 1 detects a change to (re)start a note.

## Synthesis features

- **16 voices** (`MAX_VOICES`), dynamic allocation ([src/voice_alloc.h](src/voice_alloc.h)):
  steal priority = silent → released → oldest active, driven by Core 1's active bitmap.
- **Oscillators** ([src/osc/](src/osc/)): sine (wavetable), square, triangle, saw, noise,
  band-limited BLEP square/saw, and sample playback.
- **Envelope**: ADSR ([src/envelope.cpp](src/envelope.cpp)), per-sample.
- **Filter**: state-variable (LP/BP/HP/notch), fixed-point ([src/filter.h](src/filter.h)),
  with envelope- and LFO-modulated cutoff.
- **Modulation**: per-preset LFO → amplitude (tremolo) / pitch (vibrato) / PWM / filter cutoff;
  plus a dedicated mod-wheel vibrato LFO (5 Hz) independent of the preset LFO.
- **Presets** ([src/presets.h](src/presets.h)): `VoicePreset` describes a sound; master
  `presets[]` array is the single source of truth, referenced by index. Currently Fairlight
  sample, square PWM, saw filter.

## MIDI

Transport-agnostic controller ([src/midi/midi_controller.h](src/midi/midi_controller.h))
parses raw bytes and maps notes to voices; fed by pluggable transports:
- **USB MIDI** (TinyUSB) — [src/midi/usb_midi.cpp](src/midi/usb_midi.cpp), USB used for MIDI
  only (stdio disabled).
- **UART/DIN MIDI** — [src/midi/uart_midi.cpp](src/midi/uart_midi.cpp), UART1 RX on GPIO 5
  (pin 7), 31250 baud, via optocoupler.

## Docs

- [architecture.md](architecture.md) — full system design (detailed).
- [engine.md](engine.md) — synthesis engine deep-dive.
- [building.md](building.md) — toolchain, SDK setup, build/flash steps.
- [migration.md](migration.md) — porting notes.

## Notes / gotchas

- Build output in `build/` is checked into the working tree state but is generated.
- Editing a board header? Preprocessor directives only (assembler includes it).
