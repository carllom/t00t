# T00T — Groovebox Engine: TB-303 + TR-808

A build-time engine variant: a mono **TB-303-style acid bass** synth
alongside an **808-style analog drum machine**. It is a *mode* — it
replaces the general subtractive engine for a build, not something that
runs alongside it. See `engine.md` for the shared dual-core architecture;
`history_groovebox.md` for the original design plan and how it diverged
from what shipped.

## Overview

One monophonic TB-303-style voice plus a fixed 808-style analog drum kit,
driven by MIDI notes/CCs and a basic step sequencer for the 303 part.

### Specifications

- **Voices**: 16, fixed voice map (not dynamic allocation) — one 303 voice,
  ten drum voices, five spare/unused
- **TB-303 voice**: saw/square oscillator, dedicated 4-pole ladder low-pass
  filter, two envelopes (amp + filter), accent, slide/glide, mono
  last-note-priority with legato retrigger
- **808 drum kit** (`kit_808`): bass drum, snare (2 sine shells + noise),
  low/mid/hi toms, clap (3-burst envelope), cowbell (2-oscillator metal
  bank), a single shared closed/open hi-hat voice (6-oscillator metal
  bank), crash/cymbal (6-oscillator metal bank)
- No rimshot voice, no 909 kit, no sample-based drum voice type — see
  Future/TODO
- **Sequencer**: MIDI-clock-driven (24 PPQN), 303-only, 3 fixed preset
  patterns, no recording/editing
- **Effects**: shared post-mix insert (delay or reverb), same as the
  subtractive engine

### MIDI Mapping (Input Capabilities)

| Message | Channel | Function |
|---|---|---|
| Note On | 10 (drum) | Note: fixed note → drum voice map (below), one-shot, no note-off — `set_note()` branches on channel to reach this path |
| Note On/Off | any other | Note: TB-303, fixed voice, mono last-note priority; legato (overlapping notes) triggers slide |
| Program Change | 16 (pattern select) | Configuration: selects one of 3 sequencer patterns by program number directly (0-indexed) — `channel_filter`'s exact-match on channel 16 routes it to a dedicated Handler instead of colliding with a real preset select |
| Velocity | 303 channel | ≥96 triggers accent (deeper filter sweep, +level, +drive) |
| MIDI Clock | — | Clock: 24 PPQN; advances the sequencer one 16th-note step every 6 pulses, driving the 303 directly without a second Router pass |
| MIDI Start | — | Transport: realigns the sequencer to the downbeat |
| MIDI Continue | — | Transport: resumes the sequencer clock |
| MIDI Stop | — | Transport: stops the sequencer clock |
| Program Change | any other | Parsed, currently a no-op (kit switching not implemented) |
| CC16 | 303 | Filter cutoff (live) |
| CC17 | 303 | Filter resonance (live) |
| CC18 | 303 | Filter env amount (next note) |
| CC19 | 303 | Filter env decay (next note) |
| CC20 | 303 | Accent depth (next note) |
| CC21 | 303 | Oscillator wave: saw ↔ square (next note) |
| CC22 | 303 | Ladder overdrive / bite (next note) |
| CC24 | any | FX type select |
| CC25 | any | FX wet/dry mix |
| CC26 | any | FX param 1 |
| CC27 | any | FX param 2 |

CC assignments follow the Arturia BeatStep Pro's controller layout
(CC16–31, absolute mode); there is no per-drum CC control yet (tune, decay,
tone, snappy are all fixed by the kit table). Pitch bend applies to the 303
voice only.

Drum note map (channel 10, GM-style layout):

| Note | Drum |
|---|---|
| 36 (C1) | Bass drum |
| 37 (C#1) | Cowbell |
| 38 (D1) | Snare |
| 39 (D#1) | Clap |
| 41 (F1) | Low tom |
| 42 (F#1) | Closed hi-hat |
| 45 (A1) | Mid tom |
| 46 (A#1) | Open hi-hat (shares the closed hat's voice — see Architecture) |
| 48 (C2) | Hi tom |
| 49 (C#2) | Crash/cymbal |

### Display (Presentation Capabilities)

Shows voices, CPU load, last note, mode, and FX — no step-grid UI for the
sequencer yet (see Future/TODO).

## Technical Overview

### Source Layout

- `engine.h` — flat (not union) `VoiceParams`, `VoiceType` enum
- `audio_engine.cpp` — per-voice-type render dispatch: `render_303`,
  `render_tonal_drum` (BD/tom), `render_snare`, `render_metal`,
  `render_clap`, plus an unused `render_noise_drum` path
- `kit.h` — `GrooveVoice` enum (the fixed voice map), `kit_808` table,
  `kit_find()`
- `patterns.h` — sequencer step data and `seq_*` state/functions
- `input_subsystem.cpp` — mapping table, Handlers, and the sequencer;
  `midi_controller_process()` is a one-line call into
  `midi_controller_process_generic()`, the same shared parse-and-dispatch
  loop `subtractive`/`fm`/`chip`/`tracker` use (see Architecture)
- `display.cpp` — Core 0 status display

Also draws on two files that live at the shared top level despite being
groovebox-specific in practice: `src/ladder.h` (the 4-pole ladder filter)
and `src/clap.h` (`ClapEnv`).

### Build

Build with `make ENGINE=groovebox`. No groovebox-specific build flags.

### Tools

No dedicated tools for this module.

## Architecture

### Per-Voice Dispatch

The engine is heterogeneous — a 303 voice and a snare voice run
fundamentally different code — so each voice carries a `VoiceType` tag and
the render loop dispatches per voice, once per buffer (not per sample):

```cpp
enum VoiceType : uint8_t {
    VT_SILENT, VT_TB303, VT_DRUM_BD, VT_DRUM_TOM, VT_DRUM_SNARE,
    VT_DRUM_HAT, VT_DRUM_METAL, VT_DRUM_CLAP,
};
```

`VoiceParams` is a **flat struct**, not a union — every field for every
voice type sits inline, and unused fields per type are simply ignored by
that type's render path (see Decision Record for why). `VT_DRUM_HAT`
(noise → high-pass) exists in the enum but is unused by the shipped kit;
both closed and open hi-hat route through `VT_DRUM_METAL` instead (the
6-oscillator metal bank).

### Voice Allocation

A drum machine has a fixed instrument set, not dynamic polyphony, so this
engine bypasses the dynamic allocator entirely and uses a static voice map
(`GrooveVoice` enum, `kit.h`): voice 0 is the 303, voices 2–11 are the ten
808 drum instruments (see MIDI Mapping's drum note table), the rest are
spare. Retriggering a drum voice just bumps its `trigger` counter — the
existing trigger/gate mechanism already handles "re-fire even if the
previous hit is still ringing."

`voice_alloc.cpp` is still linked into this build and its `init()`/
`update()`/`active_mask()` telemetry path is used, unmodified, to feed the
LCD's voice-activity display — only `allocate()`/`release()` are unused,
since this engine's fixed-map routing never calls them.

**Closed/open hi-hat choke**: both hi-hats share one physical voice slot
(`GV_HAT`) rather than being two separate voices — a closed-hat hit
re-triggers the shared slot, restarting its envelope and naturally cutting
whatever the open hat was still ringing, the same way any other drum
retrigger works. There is no cross-voice envelope write anywhere in the
code.

### TB-303 Voice

The oscillator is `WAVE_SAW`/`WAVE_SQUARE` through the shared `osc/` layer.
The filter is a dedicated 4-pole ladder (`src/ladder.h`, `LadderFilter`,
Stilson/Smith variant) rather than the shared two-pole state-variable
filter (`filter.h`) — see Decision Record for why. A second per-voice
envelope (`aux_env`, alongside the shared `amp_env`) drives the filter
cutoff independently of amplitude. Accent boosts amplitude, filter-envelope
depth, and ladder drive together, scaled by how far a note's velocity
exceeds `ACCENT_VEL_THRESHOLD` (96). Slide blends a Core-1-only `glide_inc[]`
value toward the target `phase_inc` (one-pole, ~20 ms time constant) when
`p.slide` is set, snapping instantly otherwise. Mono last-note priority and
legato are held-note-stack logic in `input_subsystem.cpp`'s `play_303()`.

### 808 Drum Voices

Each drum "voice" is a recipe combining existing oscillators, the shared
filter, and envelopes — not a new DSP primitive:

- **BD / toms**: sine + a pitch envelope (the `aux_env` level scales
  `phase_inc`, giving the downward pitch sweep) + amplitude decay
- **Snare**: two independently pitch-enveloped `osc_sine()` calls (shell
  tones) plus `osc_noise()` through the shared `SVFilter` in band-pass
  mode, mixed by a tone/noise balance
- **Clap**: noise through band-pass, shaped by `ClapEnv` (`src/clap.h`) — a
  small standalone state machine, not a mode of the shared `Envelope`: 3
  bursts about 10 ms apart, then a ~130 ms decaying tail
- **Cowbell / hats / cymbal**: the metal oscillator bank
  (`src/osc/metal.h`, `osc_metal()`) — a fixed 6-entry frequency table, a
  voice picks its subset via `metal_first`/`metal_count` (2 oscillators for
  cowbell, all 6 for hats/cymbal), through band-pass then optional
  high-pass, with a makeup-gain multiplier to compensate filter loss

One-shot decay envelopes (BD, toms, cowbell, hats/cymbal) are not a
separate type: `env_oneshot()` snaps an existing `Envelope` to
`level=1.0, state=ENV_RELEASE`, driven by an `EnvConfig` built with zero
attack/decay/sustain and the desired release time.

### Sequencer

A basic MIDI-clock-driven step sequencer for the 303 voice only
(`patterns.h` + the `seq_*` state/functions in `input_subsystem.cpp`). It
calls the same `play_303()` voice-trigger path MIDI note-on uses — no
engine or IPC change was needed to add it. Driven by incoming MIDI Clock
(24 PPQN, one step per 6 pulses) rather than a freestanding hardware timer;
MIDI Start realigns it to the downbeat. Per-step flags are `SEQ_ACCENT`
(maps to a fixed high velocity that clears the normal accent threshold) and
`SEQ_SLIDE` (glides into the *next* step, the 303 convention). Three fixed
patterns, selected by Program Change on a dedicated pattern-select channel;
no recording or editing.

### Code Layout

Directory-per-engine, selected by CMake (`T00T_ENGINE=groovebox`), not
`#ifdef`s on the divergences: DSP primitives (`osc/*`, `envelope.*`,
`filter.h`, `fx/*`) are shared verbatim, and `engine.h`/`audio_engine.cpp`/
`kit.h`/`input_subsystem.cpp` are forked into `engines/groovebox/`.
`CMakeLists.txt` picks each engine's own MIDI routing file from
`src/engines/${T00T_ENGINE}/` — there is no shared routing shell with a
per-engine hook, but unlike the other forked files, `input_subsystem.cpp`'s
routing itself is *not* forked in any deep sense any more: its
`midi_controller_process()` is a one-line call into
`src/midi/midi_controller_generic.h`'s `midi_controller_process_generic()`,
the same shared parse-and-dispatch loop `subtractive`/`fm`/`chip`/`tracker`
use. What stays this module's own are its `kMappingTable`, its Handlers
(including `set_note()` branching on `value.channel` to decide drum-kit
lookup vs. TB-303, and voice resolution — fixed, not dynamic `voice_alloc`
— inside that same Handler), and the sequencer's per-channel/per-voice
state — see `history_groovebox.md`'s Core 0 Input Pipeline Migration entry
for how this module's routing got here (it wasn't the original shape of
the migration). The shared
`engine_base.h` holds `MAX_VOICES`, `Waveform`, `FilterMode`,
`EffectParams`, and the `VoiceParamBlockT`/`ParamExchangeT` templates every
engine's `engine.h` instantiates; it does not define a shared
voice-parameter base struct — each engine's `VoiceParams` (including this
one's) is its own from-scratch flat struct.

## Status and Plan

### Performance

Not yet measured directly on this module — no CPU budget figures exist for
groovebox specifically. Rough estimates extrapolated from the subtractive
engine's per-voice baselines put a realistic worst-case mix (303 sustaining
+ kick + snare + hat + cowbell) around 40% of Core 1, with the 6-oscillator
metal bank (hats/cymbal) the most expensive single voice. See
`history_groovebox.md` for the estimate breakdown and the reasoning behind
it.

### Future / TODO

- **909 kit**: retuned analog drum voices, a BD click transient, and
  sample-based hats/cymbal/crash/ride via a new `VT_DRUM_SAMPLE` voice type
  (the sample player already exists and is reused as-is)
- **Rimshot/claves voice** — was planned, never implemented (no voice type,
  no kit entry)
- **Per-drum CC control** (tune, decay, tone, snappy) — currently only the
  303 has live CCs; drum voices are entirely fixed by the kit table
- **Kit switching via Program Change** — parsed but currently a no-op
- **Second TB-303 voice** — structurally trivial (assign a spare voice slot
  as a second mono 303 on its own MIDI channel)
- **Sample-trigger pads** — falls out of the 909 sample work; map spare
  voice slots to existing `SampleDef`s
- **Sequencer**: drum-track sequencing (currently 303-only), a freestanding
  timer option (currently MIDI-clock-only), an LCD step-grid UI (the
  display doesn't show one yet), and pattern recording/editing
- **Accent-driven filter-envelope decay shortening** — the amplitude and
  filter-depth accent boosts are implemented; a third originally-planned
  effect (shortening the filter envelope's decay for extra snap on accented
  notes) was never added
- **Runtime CPU measurement of this module** — the current performance
  figures are estimates, not profiling-pin measurements

## Decision Record

1. **`VoiceParams` is a flat struct, not a union** — with only 16 voices
   the extra bytes per unused field are free, and field-by-field init is
   far less error-prone than a union.
2. **The TB-303 filter went straight to a dedicated 4-pole ladder design**,
   skipping a cheaper two-pole-cascade prototype stage — the authentic
   squelch character was worth building directly rather than staging.
3. **Ladder filter coefficients are computed in floating point per
   sample**, not fixed-point — lets the filter envelope sweep continuously
   without needing a fixed-point resonance-compensation table. The costly
   per-sample `expf()` a floating-point resonance compensation would
   otherwise need is replaced by a cubic Taylor approximation. Self-
   oscillation is bounded by a cubic soft-clip on the feedback path plus a
   hard clamp as a divergence safety net.
4. **No separate one-shot-decay envelope type** — `env_oneshot()` reuses
   the existing gate-driven `Envelope` by snapping it directly into its
   release state, avoiding a second envelope implementation.
5. **Clap uses its own dedicated `ClapEnv` state machine**, not a mode of
   the shared `Envelope` — its multi-burst shape doesn't fit a generic
   one-shot-decay model.
6. **The metal oscillator bank is fixed at 6 oscillators**, not
   configurable down to 4 — matches authentic 808 character; not yet
   needed to trade away for CPU headroom.
7. **Closed/open hi-hat share one physical voice slot** rather than being
   two voices with a cross-voice envelope write — a plain retrigger already
   cuts a ringing hit the same way every other drum voice's retrigger does,
   so no extra choke mechanism was needed.
8. **Compile-time engine variant only** — no runtime mode switching between
   subtractive and groovebox. Runtime switching would cost both param
   layouts resident in RAM plus a dispatch layer, for no benefit over a
   reflash.
9. **No shared MIDI-controller routing shell.** Groovebox's
   `input_subsystem.cpp` is a full standalone file rather than a thin
   shared shell delegating to a per-engine routing hook — CMake selects
   whichever engine's file exists wholesale.
10. **`voice_alloc.cpp` stays linked** even though its `allocate()`/
    `release()` are never called — its `init()`/`update()`/`active_mask()`
    telemetry is reused as-is to feed the LCD's voice-activity display.
11. **Drums, pattern-select, and pitch bend reach the Router by remapping
    to fit the shared dispatch loop, not by carving out permanent
    exceptions from it.** Drums dispatch as plain `Note` — `set_note()`
    itself branches on `value.channel` to reach the kit-lookup path,
    rather than the Router branching on category. Pattern-select is a
    real Program Change on the pattern channel, dispatched as
    `Configuration` like every other module's preset select
    (`channel_filter`'s exact-match routes it to `set_pattern_select()`
    without colliding with a real preset select). Pitch bend's
    drum-channel exclusion lives in `set_303_bend()` itself, not the
    process loop. MIDI Clock's Handler still drives the sequencer's own
    Note-like 303 steps directly (`seq_play_step()` -> `play_303()`)
    rather than re-entering the Router a second time. With all of this,
    `midi_controller_process()` is the same one-line call into the shared
    generic loop every other migrated module uses — an earlier design
    routed drums as `Strike` and pattern-select as a `NOTE_ON`-shaped
    message instead, forcing a forked process loop; see
    `history_groovebox.md`'s Core 0 Input Pipeline Migration entry for
    that history. `midi_dispatch_strike()` and `patterns.h`'s
    `SEQ_PATTERN_BASE_NOTE`, orphaned by the remap, were removed.
12. **The shared `engine_base.h` does not define a common voice-parameter
    base struct** — each engine's `VoiceParams`, including this one's,
    stays a from-scratch flat struct rather than inheriting shared fields.
13. **The sequencer is 303-only, MIDI-clock-driven, with 3 fixed
    patterns and no recording/editing** — a deliberately narrow first cut
    of the eventual design.

## Glossary

- **Accent**: a TB-303 performance feature — a note played above a velocity
  threshold gets a deeper filter-envelope sweep, more level, and more
  ladder drive.
- **Slide / glide**: legato portamento — an overlapping note's pitch glides
  from the previous note instead of jumping.
- **Choke**: one drum voice cutting another's still-ringing sound on
  retrigger (here: closed hi-hat cutting open hi-hat, via a shared voice
  slot rather than a dedicated mechanism).
- **Kit**: the per-instrument synthesis parameter table for a drum set
  (analogous to the subtractive engine's `presets[]`).
- **Metal oscillator bank**: a bank of square oscillators at fixed
  inharmonic frequency ratios, summed and filtered — the synthesis method
  behind the cowbell, hi-hats, and cymbal.
- **PPQN**: pulses per quarter note — MIDI Clock's timing resolution (24
  here, standard MIDI Clock rate).
