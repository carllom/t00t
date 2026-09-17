# T00T — Wavetable/Granular Module

**Status: skeleton + measurement rig implemented, all three interpolation
modes measured on hardware.** `src/engines/wavetable/` (a minimal
MIDI-driven engine: one wavetable partial per voice, ADSR, no filter, no
LFO, no partial pool) and `osc/wavetable.h` (the shared read primitive) both
exist and build. The measurement rig (`rig.h`,
`tools/host_render/render_wavetable_rig.cpp`) passes its host correctness
check, and hardware passes measured all three `WT_RIG_MODE` kernels:
**~14.8 c/f/partial** (nearest), **~37.6** (bilinear), **~43.0**
(bilinear+window) — see Status and Plan and `history_wavetable.md` for the
full sweeps. Still open: `MAX_PARTIALS` itself isn't decided (it also
depends on the still-unbuilt partial pool and optional filter), and
`WT_RIG_BLOCK` alternatives are unmeasured. See `engine.md` for the shared
dual-core architecture this module builds on.

## Overview

A PPG-Wave/Korg Wavestation-style wavetable engine, generalized to also
cover granular synthesis, built around a single shared primitive: reading a
short buffer (a single-cycle wave, a PCM grain source, or a longer sample)
through a position, an increment, and — for granular use — a window. PPG
wave-scanning, Wavestation-style wave sequencing, and granular synthesis are
not three different DSP problems here; they are the same primitive at three
different settings of step duration and grain overlap (see the Partial
Primitive entry below).

Fidelity is not a goal — the project's existing lo-fi stance (`CONTEXT.md`)
extends naturally to short, low-resolution, PPG-style single-cycle waves.
Priority order: voice/partial count and modulation depth first,
multitimbrality last (see Decision Record entry 1 for why this settles the
module-boundary question).

### Specifications

- **Voices**: 16 (`MAX_VOICES`), dynamically allocated, one wavetable
  partial per voice — the skeleton's actual state today. A real partial pool
  (`MAX_PARTIALS` shared across voices, see Two-Level Allocation) is future
  work; its size isn't decided until the measurement rig runs on hardware.
- **Wave source**: PPG-style single-cycle wavetables — 8 authored keyframe
  waves per table plus a 64-entry interpolation index, read-time blended
  (see Wave Storage and Memory). Two built-in tables ship today
  (`src/engines/wavetable/tables.h`), procedurally generated (band-limited
  additive), not authored/ROM data.
- **Envelope**: 1 ADSR per voice, fixed shape (no per-preset ADSR yet).
- **Filter**: none yet — deferred, see Optional Filter.
- **Effects**: 1 shared post-mix insert (delay, reverb, phaser, flanger,
  chorus, bitcrusher, or overdrive), identical shape to every other module's
  chain (`engine.md`'s Effects section).
- **Grain source material**: PCM, same storage/residency constraints as the
  tracker module's sample data (see Wave Storage and Memory) — not
  implemented yet; the skeleton has wavetable partials only, no grains.
- No mipmaps/band-limiting by default — aliasing at the top of the keyboard
  is treated as part of the sound, matching the subtractive module's
  existing naive-vs-BLEP precedent (see Decision Record entry 5).

### MIDI Mapping (Input Capabilities)

| Message | Function |
|---|---|
| Note On/Off | Standard dynamic allocation (`voice_alloc`), one voice slot per note |
| Pitch Bend | Folded into `phase_inc` by Core 0 before it reaches Core 1 |
| CC1 | Live wave-position scan (PPG-style continuous position modulation) — not a vibrato depth, since this skeleton has no LFO |
| CC10 | Pan |
| CC72–75 | FX param 1 / mix / type / param 2, same convention every module uses |
| Program Change | Wavetable bank select — `WT_BANK[value % WT_BANK_COUNT]` |

### Display (Presentation Capabilities)

One Page (Performance only, no DIAG yet): the shared Widget/Header/Page
library (`CONTEXT.md`'s Widget catalog) — Header's Resource bar (voices +
CPU), the current wavetable bank (name + index), a "WAVEPOS" Value bar for
the live CC1 scan, and the FX chain as Value bars/Label, matching every
other module's Performance page shape.

## Technical Overview

### Source Layout

- `src/osc/wavetable.h` — the wavetable read primitive (phase × wave-position
  bilinear read, plus a nearest-neighbour variant), in the shared,
  engine-agnostic `osc/` directory regardless of how the module-boundary
  question settled (Decision Record entry 2) — the same "prove the shared
  component in a working engine first" precedent `res2p.h` set for the
  speech module's resonator (`engine.md`'s Host DSP Tooling section).
- `src/engines/wavetable/` — the module: `engine.h`
  (`VoiceParams`/`ParamExchange`, per the `engine_base.h` template every
  module instantiates), `tables.h` (the built-in wavetable bank),
  `audio_engine.cpp` (Core 1 render loop, and the measurement rig build
  behind `WT_PROFILE`, same one-file idiom as `chip`'s
  `T00T_CHIP_PROFILE`), `rig.h` (the measurement rig itself),
  `input_subsystem.cpp`, `display.cpp`. No partial pool yet (Two-Level
  Allocation is future work).

### Build

`make ENGINE=wavetable` — same per-module `T00T_ENGINE=` selection every
other module uses (`building.md`).

### Tools

`tools/host_render/render_wavetable_rig.cpp` — host-side correctness check
for `rig.h`: renders every `WT_RIG_MODE` and confirms real, finite,
non-clipping output before anyone straps a scope to it (mirrors
`render_fm_rig.cpp`'s role for the FM module's own rig). Passing this rig's
correctness check is not a performance result — see Status and Plan for
what's still needed.

## Architecture

### Relation to the Subtractive Chassis

The reason this needs to be a new module rather than an extension of
`subtractive` shows up directly in that module's own measured cost: one
subtractive voice is ~5–6% of Core 1 (`module_subtractive.md`'s
Performance section), while the tracker module's sample-playing voice —
interpolated fetch, loop wrap, per-sample volume ramp, stereo pan — costs
~31.4 cycles/frame at 44.1 kHz/150 MHz (`module_tracker.md`'s Performance
section), only ~1% of the 3401 cycles/frame budget. Subtractive's per-voice
cost is dominated by its ADSR/LFO/two-pass-SVF chassis, not by which
oscillator or sample source feeds it — swapping in a wavetable read there
would inherit that chassis cost for every partial. A wavetable/granular
module wants 2–4+ partials per voice plus overlapping grains; multiplying a
~170–200 c/f chassis by partial count is the wrong shape. The tracker's own
mixer proves the same fetch-interpolate-scale-accumulate work runs at ~31
c/f as a purpose-built kernel with no chassis attached — that is the number
this module's partial kernel should be judged against, not subtractive's.

### The Partial Primitive

One primitive, called a **Partial**: a source pointer, a read position, an
increment, a wave-position (for wavetables), an amplitude ramp (L/R), and
an optional window (for grains). Reading a wavetable is bilinear
interpolation across two axes (phase × wave-position); reading a grain is
the same fetch multiplied by a window lookup; reading a longer PCM sample
(Wavestation-style) is the tracker's own single-axis interpolated fetch.
Control-plane behavior — how position/increment/window are driven over
time — is what actually distinguishes the three source materials:

| | Step length | Overlap | Partials/voice |
|---|---|---|---|
| PPG-style wave scan | continuous (position under modulation) | none | 1–2 |
| Wavestation-style wave sequence | tens of ms – seconds | crossfade only | 2 during a crossfade |
| Granular | 5–100 ms | heavy, randomized | 4–16 |

### Two-Level Allocation

Distinct from every other module's flat `voice_alloc` (`engine.md`'s
Dynamic Voice Allocation): `voice_alloc` hands out voices as it does
elsewhere, but a separate partial pool hands out partials to voices, with
grain partials returning to a free list on window completion rather than on
note-off. This is the module's one genuinely novel piece of control-plane
machinery — the analogue of the tracker's tick ring or speech's per-voice
segment clock (`engine.md`'s Deviation From the Shared Layer Model
precedent) — and needs its own design pass once partial-count numbers from
the measurement rig are in hand.

### Fixed-Point Formats

Two formats, not one, each fitted to its own read pattern:

- **Wavetable partials**: a `uint32_t` Q0.32 phase accumulator against a
  power-of-two-sized table. Wraps for free on overflow, no mask needed
  (`idx = phase >> (32 - table_bits)`), and gives ~1e-5 Hz tuning
  resolution — better than the subtractive module's Q22.10 phase format
  (`module_subtractive.md`) and not subject to the tracker's own
  Q18.14-vs-Q22.10 detune tradeoff (`module_tracker.md`'s Fixed-Point
  Formats section), since a wavetable read has no loop-length constraint to
  balance against.
- **Grain/PCM partials**: the tracker's own Q18.14 position format
  (`mixer.h`) and `osc/sample.h`'s interpolated-fetch shape, reused as-is —
  the read pattern (arbitrary sample rate ratio, loop-aware) is identical
  to what that module already solved.

### Wave Storage and Memory

A wavetable stores 8–16 authored keyframe waves plus a 64-entry
`(wave_a, wave_b, blend)` index table (~128 B), rather than 64 full waves —
interpolated between keyframes at *read* time. This is free: the kernel
already pays for a wave-position lerp on every sample, so blending between
two stored keyframes instead of reading one directly adds no new cost. At
roughly 2 KB/table (assuming ~128-sample single-cycle waves), dozens of
tables fit resident in SRAM.

Grain source material follows the tracker module's own constraint
(`module_tracker.md`'s Memory Strategy section): PCM data must be SRAM-
resident, not read from flash/XIP, because scattered non-integer-stride
reads across many voices would thrash the 8 KB XIP cache continuously.

SRAM budget is shared with every other fixed consumer — notably
`src/fx/delay.h`'s `DELAY_LEN`, currently a hardcoded 65536 samples
(128 KB, `.bss`) plus reverb's own ~50 KB (`engine.md`'s Effects section).
Freeing meaningful SRAM for grain buffers needs `DELAY_LEN` to become an
engine-overridable constant rather than a fixed global.

No mipmaps: see Decision Record entry 5.

### Optional Filter

Per-partial filtering is not proposed. If a voice-level filter is wanted at
all, it should follow the chip module's own `FilterBus` precedent
(`module_chip.md`'s Filter Buses section) — a small typed pool voices bind
into, with a bind-or-degrade-to-unfiltered policy — rather than every
partial paying subtractive's per-voice SVF cost. A filter is specifically
what makes a voice's cost land in subtractive's ~170–200 c/f range instead
of the tracker's ~31 c/f; keeping it an optional, bounded-size pool caps
worst-case cost by construction the same way chip's design already does,
instead of letting it depend on how many partials happen to be sounding.

## Status and Plan

### Performance

**Measured** (`breadboard_rp2350`, 44.1 kHz/150 MHz, `PROFILE_PIN` duty
cycle, all three `WT_RIG_MODE` kernels swept; `history_wavetable.md` has the
full sweeps and regressions):

| Partial variant | c/f | Basis |
|---|---|---|
| Wavetable, nearest wave, linear phase, masked | **~14.8** (measured) | `WT_RIG_MODE=0` — no wave-position lerp, no loop concept at all |
| Wavetable, bilinear (phase × wave-position) | **~37.6** (measured) | `WT_RIG_MODE=1` — 4 taps, 3 lerps |
| Grain (bilinear fetch + window lookup) | **~43.0** (measured) | `WT_RIG_MODE=2` — window costs only ~5.4 c/f more than plain bilinear |
| Per-voice chassis (envelope, mix, pitch, no filter) | ~20 (est.) | Accumulate already lives in the partial itself; not separately measured |
| Per-voice filter bus (if bound) | ~50–60 (est.) | Subtractive's own SVF cost, per `module_subtractive.md`; not yet built |

Fixed per-buffer overhead measured at 18.7–22.4 c/f across all three modes
— genuine per-buffer cost (buffer clear, GPIO toggle, FIFO push), not
kernel-dependent.

At the same ≤50%-of-Core-1 target `module_tracker.md`/`module_fm.md` used
(1700.5 c/f), minus fixed overhead:

| Mode | Partials @ 50% | @ 50% minus reverb (~272 c/f) |
|---|---|---|
| Nearest | ~113 | ~95 |
| Bilinear | ~44–45 | ~37 |
| Bilinear+window | ~39 | ~33 |

All three comfortably exceed the ~28–40 planning estimate this table
originally carried. Engaging a voice-level filter bus (still unbuilt) would
fall back to something like ~8 filtered voices at 2 partials each, per the
unmeasured filter-bus estimate above. For scale, `MAX_VOICES` across
existing modules ranges 8 (speech) to 32 (chip/tracker) — the skeleton's
own `MAX_VOICES=16` sits comfortably inside even the most expensive
measured kernel's ceiling, with room to grow once a partial pool exists to
make use of the headroom.

### Future / TODO

- **Decide `MAX_PARTIALS` and design the partial pool (Two-Level
  Allocation)** — all three kernel variants are now measured
  (`history_wavetable.md`), so this is no longer blocked on a rig run; what
  remains is the actual pool design (voice-to-partial allocation, grain
  return-to-free-list on window completion) plus deciding a concrete
  partial-count ceiling against the measured numbers above. Nothing above
  the kernel (wave sequencing, real grains) should be built before this,
  matching how `fm`/`opl` scoped their own operator-count and routing
  decisions.
- **`WT_RIG_BLOCK` alternatives (8/32 vs. the default 16)** — unmeasured;
  expected to be a minor effect next to the per-kernel differences already
  measured, per the tracker/FM modules' own findings for their equivalent
  sub-block-size levers.
- **A DIAG page** — the Performance page exists; no second page with
  per-voice detail yet, unlike most sibling modules.
- **Wave sequencing / vector mixing control surface** — the Wavestation
  half of this module's scope; needs the partial pool built first.
- **Grain randomization** (position/duration jitter) — the granular half of
  this module's scope; same dependency.
- **Band-limited wave variants for the upper keyboard** — only worth
  building if the no-mipmap default (Decision Record entry 5) proves
  audibly objectionable in practice, not planned up front.
- **`DELAY_LEN` becoming engine-overridable** (`src/fx/delay.h`) — needed
  to reclaim SRAM for grain buffers; a small, independent change any module
  could benefit from, not specific to this one.
- **Multitimbrality** — deliberately last-priority, per Overview.

## Decision Record

1. **This is a new module, not a split or refocus of `subtractive`.**
   Subtractive's per-voice cost is dominated by its ADSR/LFO/two-pass-SVF
   chassis (~170–200 c/f), not by its oscillator or sample source — a
   wavetable/granular voice wants several partials at once, so attaching
   that chassis per partial is the wrong shape (see Relation to the
   Subtractive Chassis). Splitting subtractive into separate
   digital/sample and virtual-analog modules was considered and rejected:
   the split buys nothing at runtime (the cost is the chassis, not the
   sample source) and only costs a preset/doc/UI fork. Refocusing
   subtractive entirely into a DCO-driven digital module was also
   considered and rejected for the same reason — neither restructuring
   changes where the actual cost comes from.
2. **A wavetable read primitive (`osc/wavetable.h`) is added to the
   shared `osc/` directory regardless of the module-boundary decision** —
   validating the wave data format and interpolation math (and letting it
   be heard) in a working engine before a partial allocator depends on it,
   the same precedent `res2p.h` set for speech's resonator (`engine.md`'s
   Host DSP Tooling section).
3. **One Partial primitive covers PPG wave-scanning, Wavestation-style
   wave sequencing, and granular synthesis** — they differ only in step
   duration and grain overlap (see The Partial Primitive), not in the
   underlying read operation, so one kernel with varied control-plane
   behavior serves all three rather than three separate implementations.
4. **Two fixed-point phase formats, not one.** Wavetable partials use a
   Q0.32 accumulator (free power-of-two wrap, no mask, ~1e-5 Hz
   resolution); grain/PCM partials reuse the tracker's own Q18.14 format
   (`mixer.h`) unchanged. A single shared format would have forced either
   the wavetable read to carry an unneeded loop-length concept, or the
   grain read to give up resolution it needs — see Fixed-Point Formats.
5. **No mipmaps/band-limiting for wavetable partials by default.** Matches
   the subtractive module's own precedent of keeping naive, aliased
   waveforms alongside band-limited ones as an intentional "crusty" sound
   option (`module_subtractive.md`'s Decision Record entry 3) rather than
   an oversight — short, aliasing single-cycle waves are part of the
   PPG-style sound this module targets, not a defect to correct up front.
6. **A voice-level filter, if used at all, is an optional, bounded pool**,
   not a per-partial filter — modeled directly on the chip module's own
   `FilterBus` (`module_chip.md`'s Filter Buses section: a small typed
   pool, bind-or-degrade-to-unfiltered). This bounds worst-case per-voice
   cost by construction, the same way chip's design already does, instead
   of letting it scale with however many partials happen to be active.
7. **Wave tables store 8–16 authored keyframes plus a 64-entry blend
   index, not 64 full waves.** The kernel already pays for a wave-position
   lerp on every sample regardless, so blending between two stored
   keyframes at read time instead of reading one full wave directly is
   free — see Wave Storage and Memory.
8. **The skeleton repurposes CC1 (mod wheel) as a live wave-position scan**,
   not a vibrato depth — this skeleton has no LFO yet, and a continuous
   wave-position sweep is this module's own PPG-style analogue of what
   every other engine uses CC1 for, not an arbitrary substitution.
9. **The skeleton's envelope, FX chain, and pan law are reused unmodified**
   from the shared layer (`src/envelope.h`, the per-module FX block every
   engine already carries, `pan.h`) rather than written fresh — none of
   that is wavetable-specific, and duplicating it would only risk drift
   from the versions every other module already exercises.

## Glossary

- **Partial**: the module's unifying DSP primitive — a source pointer,
  read position, increment, wave-position, amplitude ramp, and optional
  window. One or more partials make up a voice.
- **Wavetable**: a set of authored keyframe waves (single-cycle,
  PPG-style) plus a blend index, read along a wave-position axis in
  addition to the usual phase axis.
- **Wave-position**: the second read axis a wavetable partial has beyond
  ordinary oscillator phase — which point along a table's keyframe
  sequence (or interpolated between two) is currently sounding.
- **Grain**: a short, windowed read of PCM source material — a granular
  partial's unit of playback, returned to the partial pool's free list on
  window completion.
- **Window**: the amplitude envelope applied across a single grain's
  duration, distinct from a voice's own ADSR.
- **Wave sequence**: a Wavestation-style ordered, timed sequence of
  wavetable steps, crossfaded at each transition — distinct from PPG-style
  continuous wave-position modulation.
- **Partial pool**: the shared allocator handing partials out to voices,
  separate from and downstream of `voice_alloc` — see Two-Level
  Allocation.
