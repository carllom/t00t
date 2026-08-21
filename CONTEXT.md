# T00T — Context

Polyphonic digital synthesizer firmware for the **RP2350 (Raspberry Pi Pico 2)**. An
experiment in real-time tone generation and classic-sound-chip emulation. Goal is
efficient, simple real-time synthesis — not audiophile quality.

## Hardware / boards

Two board targets, selected at build time via `BOARD=` (cmake caches it, so `make clean`
when switching):

| `BOARD` | Hardware | Buttons | DAC | Notes |
|---|---|---|---|---|
| `breadboard_rp2350` **(default)** | Pico 2 on breadboard | none (MIDI only) | PCM5122, I2S GPIO 16(BCK)/17(LRCK)/18(DIN) | the author's actual rig |
| `vgaboard_rp2350` | Pimoroni VGA Demo + Pico 2 | A/B/C on GPIO 0/6/11 | PCM5100A | |

- Board headers live in [src/boards/](src/boards/); they are also included by the assembler,
  so they must contain **only preprocessor directives**.
- **The author runs the breadboard board.** Build with plain `make`. Do NOT flash a vgaboard UF2
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
- pico-sdk/ and pico-extras/ are vendored at repo root (see [docs/building.md](docs/building.md) if missing).
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
- **Effects** ([src/fx/](src/fx/)): one global post-mix insert on Core 1 (fixed cost),
  selectable by **CC74** (range split into bands): **Off / Delay / Reverb**. The same three
  knobs drive whichever is active — **CC73 = mix**, **CC72 = feedback/room-size**,
  **CC75 = time/damping**. Params ride the `ParamExchange` block as `EffectParams`
  (type + 3 raw 0–127 values; each effect maps them to its own scale). Delay = 128 KB int16
  feedback line ([delay.h](src/fx/delay.h)); reverb = float Freeverb, 8 comb + 4 allpass,
  ~50 KB ([reverb.h](src/fx/reverb.h)) on the M33 FPU. Buffers clear on a type switch.
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

- [docs/logs/architecture.md](docs/logs/architecture.md) — full system design (detailed).
- [docs/engine.md](docs/engine.md) — cross-module dual-core architecture (pin allocation, buffer
  flow, IPC, voice allocation, MIDI input) shared by every synthesis module.
- [docs/building.md](docs/building.md) — toolchain, SDK setup, build/flash steps.
- [docs/logs/migration.md](docs/logs/migration.md) — porting notes.
- **Per-module docs** — one `docs/module_<name>.md` (current spec/usage) and, where the module
  has development history worth keeping, a `docs/logs/history_<name>.md` (dated build/measurement
  narrative, kept separate so it doesn't clutter the spec):
  [module_subtractive.md](docs/module_subtractive.md) / [history_subtractive.md](docs/logs/history_subtractive.md),
  [module_chip.md](docs/module_chip.md) / [history_chip.md](docs/logs/history_chip.md),
  [module_fm.md](docs/module_fm.md) / [history_fm.md](docs/logs/history_fm.md),
  [module_groovebox.md](docs/module_groovebox.md),
  [module_opl.md](docs/module_opl.md) / [history_opl.md](docs/logs/history_opl.md),
  [module_speech.md](docs/module_speech.md) / [history_speech.md](docs/logs/history_speech.md),
  [module_tracker.md](docs/module_tracker.md) / [history_tracker.md](docs/logs/history_tracker.md).

## Language

Vocabulary for the Core 0 input pipeline. The category vocabulary below
(Note/Strike/Modifier/Configuration/Clock/Transport) originates from #84; the
mechanism vocabulary (Input pipeline, Input subsystem, Router, Input event,
Normalization, Handler, Voice Allocation Interface) belongs to a from-scratch
redesign effort superseding #84/#85's narrower "shared vocabulary + table
dispatch" design — wayfinder map #94, spec #99, tickets #100-104.

**Input pipeline**:
The complete flow from a physical/transport input source to a module's own
state: transport reception, parsing, Shaping, routing (the Router), and
module-specific handling (the Input subsystem, which includes each module's
own Normalization). The umbrella term for the whole redesign.

**Input subsystem**:
The module-specific tail of the Input pipeline — everything downstream of
the Router once an Input event has been matched and handed off: Handlers
(which perform their own Normalization), the Voice Allocation Interface, and
any module-owned state. Distinct from the generic, module-agnostic stages
upstream of it (parsing, Shaping, routing).

**Router**:
The generic mechanism that takes an Input event and hands it to the right
module Handler. Source- and module-agnostic; replaces the #86-era
`input_dispatch` as the working name (implementation shape not yet decided).

**Input event**:
A source-agnostic event the Router matches on and passes to a Handler — the
output of Shaping, common across MIDI and non-MIDI sources. Still carries
source-native values (e.g. a raw 0-127 MIDI velocity), not module-native
ones — see Shaping vs. Normalization below. Replaces the #86-era
`InputValue` as the working name.

**UI command**:
A sibling to Input event, not a member of it: the parsed form of one of
four generic, deliberately context-dependent commands — `+`/`-` (aka
Increase/Decrease, Next/Previous) and `enter`/`exit` (aka select/yes, no).
Still flows through the ordinary Sensor event → Shaping stages (no new
parsing/debounce/shaping plumbing), but diverges *before* the Router —
Router/Handler stays exclusively for dispatching Input events to
module-owned audio-engine state, and a UI command never reaches a module
Handler. This split is deliberate, not incidental: the Input pipeline's
whole design point is that a module's mapping table and Handler logic stay
"entirely my own — never forced through a shared routing hook another
module's needs shaped" (spec #99's own user story 2), which is the
opposite of what UI navigation needs — one identical, standardized
behavior shared by all 7 engine modules, not seven per-module forks.
Whatever consumes UI command must be fully optional: a board/config with
no UI-navigation control wired (or `HAS_LCD=0`) leaves audio functionality
completely unaffected, matching the board-conditional pattern the LCD
itself already follows. Only one concrete meaning for these four commands
is decided so far — see Page's entry below for Page navigation, wayfinder
ticket "Page navigation: how a module's Pages relate and get switched,
control-agnostic". The general command vocabulary as a reusable primitive
across other, not-yet-designed interaction contexts (value editing,
confirm/cancel) is intentionally left open.

**Shaping**:
Generic, cross-module value-adjustment that runs before the Router, in the
input's own source-native terms (raw MIDI bytes, not a module's native
units) — fixed-value substitution, channel filtering, range filtering. Lives
here, not in Normalization, because a human configuring it (e.g.
`fixed_velocity = 100`) needs to reason in source-native terms that mean the
same thing regardless of which module is compiled in; a module-normalized
number wouldn't have one consistent meaning across modules.

**Normalization**:
Module-specific conversion from a source-native value (raw MIDI byte, ADC
count) to that module's own native unit (Hz, semitones, Q15, a dB value,
...). Module-owned — lives inside the Input subsystem (typically inside a
Handler), not a generic pre-Router pipeline stage, because only the module
knows its own target representation.
_Changed from_: an earlier framing (this session) that treated Normalization
as one generic pre-Router stage. Split into Shaping (generic, pre-Router,
source-native) + Normalization (module-owned, module-native) once a concrete
scenario (a fixed-velocity override needing to mean "raw MIDI 100" rather
than some module's internal amplitude scale) showed the two were being
forced under one name.

**Sensor event**:
A source-specific parsed event from a physical, non-MIDI input — a button
press/release, a potentiometer reading, or (if ever added) an accelerometer
or temperature reading. Covers discrete (button-like) and continuous
(pot-like) readings alike. Sibling to a MIDI event: both are per-source
parsed types that Normalization turns into a common Input event.
_Avoid_: "Controller" (MIDI already uses it for CC numbers; this codebase's
`controller.cpp`/`controller_init()`/`controller_tick()` also already name
the button-polling code, a third meaning) and "Input" (too generic once
"Input event"/"Input pipeline" already claim the word at other layers).

**Handler**:
Whatever a module provides to plug into the Router. General term — a
"setter" (a Handler that writes a param) is one kind; an implementation of
the Voice Allocation Interface is another.

**Voice Allocation Interface**:
The contract between the voice allocator and a module, replacing today's
direct, interleaved `voice_alloc_allocate()`/`voice_alloc_release()` calls
inside MIDI-parsing code. Applies to both Note and Strike — both need a free
synthesis voice; neither's allocation bookkeeping should live inside input
parsing.

**Note**:
A pitched input: note number, velocity. Allocation-agnostic — voice
alloc/release is a separate, composed concern reached via the Voice
Allocation Interface, not implied by the category itself.
_Changed from_: the #84-era definition, which bundled "implicit voice
alloc/release" into the category itself. Decided against: decoupling
allocation from input handling is this redesign's reason to exist, so the
category can't presuppose the coupling being removed.

**Strike**:
A discrete, unpitched input (a drum-pad hit, a one-shot sample trigger). An
identifying number may still be present and required (e.g. which drum/sample
to trigger) — the defining trait is that the number, if any, is never
interpreted as pitch. Like Note, may still reach the Voice Allocation
Interface for a free synthesis voice; the category is about the absence of
pitch, not about whether an identifier or allocation is involved.
_Changed from_: "no note number, no pitch" — read literally, wrong for a
drum-pad hit, whose MIDI note number is a required identity field (which
drum/sample to trigger via a kit lookup), just never converted to pitch/Hz.
Surfaced while sanity-checking against `groovebox`'s real drum-trigger code
(`trigger_drum()`'s `kit_find(kit_808, note)`) during wayfinder ticket #98.
_Avoid_: Trigger (already means the per-voice generation counter in
`VoiceParams`, see `docs/engine.md`'s Trigger/Gate Signaling and issue #83 —
using it here would mean the same word means two different things at two
different layers).

**Modifier**:
A live, continuous input value, voice-scoped or module-global depending on
an explicit scope/target on the call (e.g. pitch bend targets held voices;
an FX CC is module-global).

**Configuration**:
An input that selects a preset/template. Some selected values seed initial
Modifier values; others are immutable character choices (oscillator type,
LFO shape).

**Clock**:
A tempo-pulse input (e.g. groovebox's 24 PPQN MIDI Clock). Distinct from
Transport.

**Transport**:
A play/pause/stop input, first-class rather than a generic Strike.
_Avoid confusing with_: "MIDI transport" (USB vs. UART — the byte-carrying
layer under MIDI, see the MIDI section above). Same word, unrelated concept.

**Data Exchange**:
Parked, not decided: a possible category for opaque byte-blob store/retrieve
of module-specific table data (curves, waveforms, patch data). Mechanically
distinct from Configuration (bulk/bidirectional/opaque vs. scalar template
selection). No module implements anything like this today.

### Display / UI

Vocabulary for standardizing shared LCD UI components and page structure
across modules — wayfinder map "Display: shared UI components and page
structure" (breadboard board only; the LCD's own driver vocabulary lives in
[src/wslcd/](src/wslcd/), not here).

**Page**:
One of a module's several top-level display screens, each presenting a
different aspect of the module (e.g. a main performance view vs. an FX view).
Refreshes at a shared 10Hz base cadence unless it overrides that rate for
genuinely time-critical content (e.g. tracking live playback position).
Navigated by UI command's `+`/`-` (step through the module's own ordered
Page list, wrapping at both ends) and `exit` (jump directly to the
Performance page) — the default, fallback meaning of those commands
whenever no more specific interaction context has claimed them, not a
separately-entered "Page navigation mode". A shared Page/Header framework
owns the current-Page cursor for every module — a module only declares its
own ordered Page list, never its own navigation logic — so this behavior
is one implementation, identical across all 7 engine modules, not a
per-module fork. See UI command (Input pipeline vocabulary, above) for why
this doesn't route through Router/Handler like audio-facing input does.
Settled on wayfinder ticket "Page navigation: how a module's Pages relate
and get switched, control-agnostic".
_Avoid_: "Mode" — already claimed by several unrelated concepts in this
codebase (`FilterMode`; `SpeechMode`'s LOOP/GATED/ONESHOT playback behavior;
groovebox's own display already has a literal `MODE` row showing DRUM-vs-303
channel state) — none of which mean "which screen is showing."

**Widget**:
A reusable rendering unit shared across modules' Pages — e.g. a parameter
row, a value readout, a title/breadcrumb, a meter/bar — that renders one
piece of module state to the screen in a standard way. A Widget may offer
more than one **presentation** of the same value at different screen
footprints, so a module can choose footprint per value under space
pressure — e.g. PercentageBar's full label+%+bar form alongside Resource
bar (see below), which folds CPU load and active-voice-count into one
compact, unlabeled header indicator. This ticket settled only that one
concrete composite, not a general compact-form-for-every-Widget catalog —
see Resource bar's own entry for what's decided and what isn't.
_Avoid_: "Component" — already used loosely in this repo's own docs/history
for generic, non-UI reusable code pieces (e.g. `history_groovebox.md`'s
"Existing component" table, `history_speech.md`'s "Common Component
Extraction").

**Value row**:
A Widget showing a label plus its value as plain text (today's `draw_val`/
`draw_label` pattern, independently hand-rolled per module). Kept for values
that want to stay generic and maximally readable (e.g. NOTE, VOICES count) —
not being replaced outright by Value bar, which suits a different case (see
below).

**Value bar**:
A Widget showing a label overlaid on a proportional fill bar, for CC-style
continuous values (pan, FX mix, filter cutoff) — more screen-estate-compact
than a Value row for this case, since the fill level itself carries most of
the value's meaning. The overlay is pixel-precise: the fill boundary can
fall inside a single character's cell, so rendering it needs a lower-level
primitive than today's whole-glyph text draw, choosing a fill- or off-
colored background per pixel column rather than per character. Still
ordinary sequential overwrites, not a boolean/compositing operation; see
[docs/lcd-driver-capabilities.md](docs/lcd-driver-capabilities.md), which
already established the ST7789 has no hardware compositing ALU and this
driver keeps no shadow framebuffer.

**PercentageBar**:
A Value bar specialized for 0–100% values (e.g. CPU load), whose fill color
is chosen automatically from a fixed severity threshold rather than supplied
by the caller. Otherwise the same overlay shape as Value bar.
_Avoid_: "LoadBar" — ties the name to its one current use (CPU load) rather
than the general 0–100%-with-auto-color shape.

**ActivityGrid**:
A Widget showing a grid of cells, one per voice or channel, filled or dim to
indicate simple on/off activity — used where only *which* slots are active
matters, not per-slot detail.

**VoiceGrid**:
A Widget showing a grid of per-voice cells, each carrying a short text label
and one of three caller-assigned colors — for voice detail that doesn't
reduce to plain on/off, unlike ActivityGrid.
_Avoid_: "activity-grid" as a name for either ActivityGrid or VoiceGrid on
its own — the two are independent Widgets, not variants of one grid.

**Resource bar**:
A Widget living in the Header/title-bar chrome, not a Page body row: a
horizontal bar, background-colored backdrop showing unfilled capacity,
fixed at a 32px maximum width regardless of a module's real `MAX_VOICES`
(32 is the largest real value, chip/tracker; other modules scale their
per-voice pixel width so the bar still spans the full 32px at max voice
count — 2px/voice at 16, 4px/voice at 8, a single proportional-rounded
fill rather than per-voice bricks for counts that don't divide evenly,
e.g. opl's 9). Folds two independent signals into one indicator: **fill
length** is active voices ÷ `MAX_VOICES` (magnitude only); **fill color**
is CPU load severity, reusing PercentageBar's existing threshold (green
under 50%, amber under 80%, red past that) — so the bar can read "CPU-
bound" (red) at half voice usage, or "comfortable" (green) at full voice
usage. Carries no text or label of any kind; meaning is documented off-
device, read the way a hardware synth's LED cluster is read. This is a
real precision trade — exact CPU% and exact voice count aren't readable
from this Widget, only relative magnitude and load-severity color are —
made deliberately for the Performance page, where panel space is the
scarce resource; PercentageBar and VoiceGrid's full labeled forms remain
available wherever that detail is actually wanted (which Pages exist
beyond Performance, and what they contain, is a separate, still-open
question — see Performance page and the map's "Not yet specified").
Settled on wayfinder ticket "Compact Widget variants: catalog alternate
low-footprint presentations" as the one concrete compact-presentation
catalog entry this ticket resolved — superseding two more elaborate
proposals discussed and discarded in the same session (a separate CPU
status-square icon alongside an independent voices bar; body-row compact
forms reusing PercentageBar's shape for voice count). Prototype (four
variants, pixel-accurate against the real panel/font) on branch
`prototype/compact-widgets`.

**Performance page**:
The one required Page every module presents: a dense, per-module-curated
summary of the values most relevant to live playing, serving as the
module's default/main view. Other Pages hold whatever doesn't fit there, or
doesn't belong densely packed alongside it. Not an exhaustive listing of
every Modifier or Configuration entry (see Input pipeline vocabulary
above): "most" is a soft target, not a mandate — a module may omit
lower-priority values under space pressure, provided they stay reachable on
another Page. A module's single Configuration entry (its preset/patch
select) is commonly included too, since knowing what's loaded is itself
performance-relevant. No numeric minimum/maximum Widget count is implied;
density is bounded only by panel space and by which Widget presentation
(see Widget entry above) a module picks per value. A module may also define
a small number of preset-triggered row variants sharing the Performance
page's slot, when its live-relevant parameter set genuinely differs by
selected preset (e.g. speech's per-voice-model parameters) — a permitted
mechanism, not a requirement; most modules keep one static layout. Which
fields actually appear on chip's/fm's/etc. Performance page, and how
they're laid out, is a separate per-module follow-up (out of scope for this
map) — this entry fixes only the rule above, not any module's concrete
content.

**Header**:
The top chrome region of a Page — two rows (module name; Page name plus a
Page indicator when a module has more than one Page). No literal "t00t"
wordmark: the per-module accent color (`COL_TITLE`) already carries the
branding. The Page indicator is a row of small filled/dim cells, one per
Page, reusing ActivityGrid's shape — omitted entirely for a single-Page
module. Content stays clear of the panel's rounded corners (see
[docs/lcd-driver-capabilities.md](docs/lcd-driver-capabilities.md)).

## Notes / gotchas

- Build output in `build/` is checked into the working tree state but is generated.
- Editing a board header? Preprocessor directives only (assembler includes it).
