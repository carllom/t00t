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
- **LCD (breadboard only):** Waveshare 1.83" 240×284 IPS, Rev2 = **ST7789P**, on
  **SPI1** — DC 8, CS 9, CLK 10, DIN 11, RST 12, BL 13 (PWM). Driven by Core 0 at
  low priority; driver in [src/wslcd/](src/wslcd/). `HAS_LCD` is 1 for breadboard,
  0 for vgaboard (those pins are VGA colour / Button C there).

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
  Core 0 must call `voice_alloc_update()` once per pass to drain that bitmap: button
  boards do it in `controller_tick()`, MIDI-only boards do it at the top of the main loop.
- **Oscillators** ([src/osc/](src/osc/)): sine (wavetable), square, triangle, saw, noise,
  band-limited BLEP square/saw, and sample playback.
- **Envelope**: ADSR ([src/envelope.cpp](src/envelope.cpp)), per-sample.
- **Filter**: state-variable (LP/BP/HP/notch), fixed-point ([src/filter.h](src/filter.h)),
  with envelope- and LFO-modulated cutoff.
- **Modulation**: per-preset LFO → amplitude (tremolo) / pitch (vibrato) / PWM / filter cutoff;
  plus a dedicated mod-wheel vibrato LFO (5 Hz) independent of the preset LFO.
- **Effects** ([src/fx/delay.h](src/fx/delay.h)): global feedback **delay**, a post-mix insert
  on Core 1 (fixed cost, ~1 voice). Params ride the `ParamExchange` block as `EffectParams`
  (delay length, feedback, wet/dry). MIDI **CC71 = time (20–1000 ms), CC73 = feedback,
  CC74 = mix**; default dry (mix 0). 128 KB delay line (`.bss`). Reverb is the planned next pass.
- **Presets** ([src/presets.h](src/presets.h)): `VoicePreset` describes a sound; master
  `presets[]` array is the single source of truth, referenced by index. Currently Fairlight
  sample, square PWM, saw filter.

## Display (LCD)

Self-contained driver in [src/wslcd/](src/wslcd/), owned by Core 0 (audio + MIDI
take precedence). Rolled our own — no ST7789 driver ships with the SDK.
- [lcd_st7789.cpp](src/wslcd/lcd_st7789.cpp): SPI1 + a dedicated **polled** DMA
  channel (no IRQ, so it never contends with the audio DMA IRQ). ST7789P init
  matching the verified Rev2 demo; no GRAM offset (Rev1 needed +20, Rev2 doesn't).
  SPI at 64 MHz (runs clean on the breadboard jumpers). Backlight PWM on GP13.
  **CS is framed per transaction** (pulsed high after each command/data burst);
  holding it low continuously leaves the panel black despite a correct init.
- [gfx.cpp](src/wslcd/gfx.cpp): tile-based drawing (no full framebuffer) — a small
  RAM scratch tile, DMA-blitted region by region. `gfx_rgb`/`gfx_fill_rect`/
  `gfx_text` (8×8 font in [font8x8.h](src/wslcd/font8x8.h)). Colours are
  byte-swapped ("wire format") RGB565 so the byte-DMA needs no swap.
- [display.cpp](src/wslcd/display.cpp): Core-0 API. `display_init()` paints static
  chrome; `display_task()` runs from the main loop, self-limits to ~20 Hz, and
  redraws only changed fields (cheap value-compares when idle). Live UI shows
  voices (each dot: **fill = sounding, white border = note pressed**), CPU load,
  last note/velocity, preset, bend, mod.
  `display_bringup_test()` (colour bars + banner) is kept for driver diagnostics.
- Telemetry it reads: `voice_alloc_active_mask()` (sounding — Core 1's feedback,
  drained each pass) and `voice_alloc_gated_mask()` (pressed — Core 0's gate
  tracking); `audio_engine_load()` (Core 1 render-time EMA); `midi_controller_ui_state()`.
- If blank/garbled on hardware: lower `LCD_SPI_HZ`, or tune
  `LCD_COL_OFFSET`/`LCD_ROW_OFFSET`/`LCD_MADCTL` in lcd_st7789.

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
