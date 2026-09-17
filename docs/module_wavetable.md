# T00T — Wavetable/Granular Module

**Status: skeleton implemented, playing real PPG Wave 2.3 factory waveforms,
with a basic per-voice filter; all three partial-kernel interpolation modes
measured on hardware, the full filtered chassis not yet measured.**
`src/engines/wavetable/` (a minimal MIDI-driven engine: one wavetable
partial per voice, ADSR, a basic 2×-cascaded lowpass filter, no LFO, no
partial pool) and `osc/wavetable.h` (the shared read primitive) both exist
and build. `tools/ppg/convert_ppg_waves.py` converts the real PPG Wave 2.3
ROM data already recovered under `tools/ppg/` into the engine's native
format — see PPG Architecture Reference below and `history_wavetable.md`
for the conversion/integration record. The measurement rig (`rig.h`,
`tools/host_render/render_wavetable_rig.cpp`) passes its host correctness
check, and hardware passes measured all three `WT_RIG_MODE` kernels:
**~14.8 c/f/partial** (nearest), **~37.6** (bilinear), **~43.0**
(bilinear+window) — see Status and Plan. Still open: the *combined*
per-voice cost (partial + envelope + filter) is unmeasured, which is why
`MAX_VOICES` is set conservatively to 8 (matching real PPG polyphony)
rather than pushed to the partial-kernel ceiling those numbers alone would
suggest — see Decision Record. See `engine.md` for the shared dual-core
architecture this module builds on.

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

Design intent (per the initial bluesky discussion): match the PPG Wave
2.2/2.3 *sound* as an initial target, not its architecture or limitations
to the letter — real factory waves and a PPG-scale voice/filter budget
first, with room for wave sequencing, granular use, and other wavetable
functionality PPG hardware never had, once the partial pool (Two-Level
Allocation) exists.

### PPG Architecture Reference

Guidance for this module. Byte-level wave/wavetable format facts come from
this project's own ROM extraction (`tools/ppg/README.md`), confirmed
**exact** against independent community research (waveform 0 and all 29
wavetable-index records matched a public reference byte-for-byte). Broader
sound-architecture facts (voices, envelopes, filter, modulation matrix) come
from the manufacturer's own PPG Wave 2.2 Owner's Manual and PPG Wave 2.3
Service Manual, fetched and read directly — where a manufacturer manual
disagrees with a secondary community summary
([ppg.synth.net/wave22](https://ppg.synth.net/wave22/)), the manual wins.
See `docs/research/ppg-wave-23-sound-architecture-and-data-formats.md` for
the full source list, citations, and open questions this section
summarizes.

| | Our own ROM extraction (`tools/ppg/`) | Secondary summary ([ppg.synth.net/wave22](https://ppg.synth.net/wave22/)) |
|---|---|---|
| Waveform size | **64 samples**, 8-bit unsigned PCM (confirmed) | states 128 — not used, see above |
| Waveform count | **244** (confirmed) | states "well over 2000"/"over 1800" reachable — the same author's own more detailed technical note explains why: "the PPG Wave only has a set of about 250 waveforms, most of the wavetables' contents are interpolated between 2 of the waveforms... gives the marketing-hype-number" ([Seib, *PPG Wave ROM Waveforms and Wavetables*](https://www.hermannseib.com/documents/PPGWTbl.pdf)) — i.e. simple two-keyframe interpolation across every table position, the same mechanism `osc/wavetable.h` already implements, not additional distinct stored or algorithmically-synthesized waveforms |
| Wavetable count | **29** (confirmed; matches the reference material's "27 primary + Upper Wavetable as table 28", table 29 synth-computed) | states 32 banks — not used |
| Wavetable shape | sparse authored keyframes at specific slot positions (4-31 per table, not evenly spaced), interpolated between them across a 0-63 range | ("intermediate waveforms calculated" — consistent, no contradiction) |
| Factory programs stored | **100** (confirmed: the owner's manual states it directly, and an independent from-scratch decode of the factory cassette data lands on the same count) | not stated |

Architectural color, confirmed directly from the manufacturer's own
Owner's/Service Manuals rather than taken as unverified secondary-source
guidance:

- **8-voice polyphony**, 2 independently-programmable oscillators per voice
  (called Group A/Group B, 16 total) — this module currently implements 1
  partial per voice; see Decision Record for why `MAX_VOICES` is set to 8
  now while a second oscillator per voice remains future work.
- **One SSM2044 (4-pole/24 dB lowpass) filter and one CEM3360 dual-VCA per
  voice** — not a shared resource across voices, confirmed directly from the
  Service Manual's Voice Board parts list. This directly shaped this
  module's own Per-Voice Filter design (see Architecture and Decision
  Record) away from the shared-bus approach `module_chip.md`'s `FilterBus`
  uses.
- **Wave-position modulation sources, corrected**: keyboard-tracking, LFO,
  aftertouch, and a dedicated Envelope 1 — which drives filter cutoff and
  wave-position simultaneously from one shared envelope shape, at
  independently adjustable depths, not two separate envelopes. Velocity is
  **not** a wave-position source on real hardware (only filter and
  loudness); this corrects an earlier version of this section sourced from
  a secondary summary. This module currently wires only a live mod-wheel
  scan (CC1) and the ADSR-driven filter cutoff described below.
- Phase-accumulator oscillators (20-bit accumulator, top bits select wave
  position) — architecturally the same shape `osc/wavetable.h`'s Q0.32
  accumulator already is, just wider here.

### Specifications

- **Voices**: 8 (`MAX_VOICES`), matching real PPG Wave 2.2/2.3 polyphony,
  dynamically allocated, one wavetable partial per voice — a real partial
  pool (`MAX_PARTIALS` shared across voices, see Two-Level Allocation) is
  future work.
- **Wave source**: real PPG Wave 2.3 factory data when
  `tools/ppg/convert_ppg_waves.py` has been run locally (gitignored output,
  see Source Layout) — 244 waveforms, 64 samples each, forming 29
  wavetables with their authored (not evenly-spaced) keyframe positions
  baked in at conversion time. Falls back to 2 procedurally-generated
  band-limited-additive tables (`src/engines/wavetable/tables.h`) when the
  PPG data hasn't been generated.
- **Envelope**: 1 ADSR per voice, fixed shape (no per-preset ADSR yet).
- **Filter**: 1 per voice (not shared), two cascaded 2-pole SVFs
  approximating the PPG's SSM2044's 4-pole/24 dB slope — a deliberately
  basic stand-in, not a transistor-ladder model. Fixed cutoff/resonance for
  now; the ADSR modulates cutoff, matching subtractive's own
  reused-envelope convention. See Per-Voice Filter and Decision Record.
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
  module instantiates), `tables.h` (the wavetable bank — real PPG data or
  the procedural fallback, see Specifications), `ppg_waves.h` (**gitignored**
  — generated locally by `tools/ppg/convert_ppg_waves.py`, not checked in;
  third-party PPG ROM content, same reasoning as the FM module's
  `patches.h`), `audio_engine.cpp` (Core 1 render loop, the per-voice
  filter, and the measurement rig build behind `WT_PROFILE`, same one-file
  idiom as `chip`'s `T00T_CHIP_PROFILE`), `rig.h` (the measurement rig
  itself), `input_subsystem.cpp`, `display.cpp`. No partial pool yet
  (Two-Level Allocation is future work).
- `tools/ppg/` — PPG Wave 2/2.2/2.3 ROM dumps, cassette dumps, and
  extraction tooling (`tools/ppg/README.md`), plus this module's own
  `convert_ppg_waves.py` (reads `extract/w23_waves.bin` +
  `extract/w23_wavetables.json`, writes `src/engines/wavetable/ppg_waves.h`)
  — gitignored in full (third-party ROM/cassette content), see that
  README for provenance and format details.

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

`tools/ppg/convert_ppg_waves.py` — converts `tools/ppg/extract/`'s already-
recovered PPG Wave 2.3 ROM data into `ppg_waves.h` (see Source Layout);
`tools/host_render/render_ppg_waves.cpp` range-checks the converted data
and renders a few real wavetables' wave-position sweeps to WAV as a
listening aid, the same "verify the conversion before wiring it into a
real engine" step `xm2t00t`'s and the FM module's own converters take —
only builds when `ppg_waves.h` has actually been generated (CMake `EXISTS`
gate, `tools/host_render/CMakeLists.txt`).

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

A wavetable stores a handful of authored keyframe waves — real PPG data
has 4–31 per table, at their own authored (not evenly-spaced) slot
positions — plus a 64-entry `(wave_a, wave_b, blend)` index table (~192 B),
rather than one full wave per index entry: interpolated between keyframes
at *read* time. This is free: the kernel already pays for a wave-position
lerp on every sample, so blending between two stored keyframes instead of
reading one directly adds no new cost. `osc/wavetable.h`'s `WT_TABLE_SIZE`
is 64 samples (`WT_TABLE_BITS = 6`), matching the real PPG ROM waveform
size exactly (see PPG Architecture Reference) rather than an arbitrary
choice; a keyframe is 128 B at that size. `WaveTable.keyframes` doesn't
have to be a small per-table buffer — the real PPG data uses one big shared
pool (all 244 waveforms) with each table's index entries holding absolute
indices into it, since many tables reuse the same underlying waveform.
`osc/wavetable.h`'s read functions already index `keyframes[wave *
WT_TABLE_SIZE + i]` regardless of whether `wave` means "this table's own
small keyframe set" or "an absolute index into a shared pool" — no engine
change was needed to support real PPG data's sharing.

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

### Per-Voice Filter

Implemented, revising this section's earlier shared-bus proposal (see
Decision Record entry 12): real PPG hardware gives every voice its own
SSM2044 filter and VCA (PPG Architecture Reference above), not a shared
pool, and at `MAX_VOICES = 8` the arithmetic supports doing the same thing
directly rather than reaching for chip's `FilterBus` compromise. Two
`SVFilter` (`src/filter.h`, subtractive's own SID-style 2-pole SVF, reused
unmodified) instances per voice, ticked in series, approximate the
SSM2044's 4-pole/24 dB slope — a deliberately basic stand-in, not a
transistor-ladder model or an exact match to the chip's own saturation/
self-oscillation character. Filter state resets on note retrigger for a
clean attack, matching subtractive's own SVF convention. Cutoff and
resonance are fixed constants for now (`FILTER_BASE_CUTOFF_HZ`,
`FILTER_RESONANCE_Q15`, `audio_engine.cpp`); the same ADSR that drives
amplitude also modulates cutoff by a fixed amount
(`FILTER_ENV_AMOUNT_HZ`), the same "one envelope, two destinations"
convention subtractive's own filter uses.

Cost is not yet measured for the combined per-voice chassis (partial +
envelope + two filter passes) — only the bare partial kernel has hardware
numbers (Status and Plan). This is why `MAX_VOICES` was set to 8 rather
than pushed toward the unfiltered partial-kernel ceiling those numbers
alone would suggest (Decision Record entry 13's budget math).

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
| Per-voice filter, 2× cascaded SVF (now built) | ~100–120 (est.) | 2× subtractive's own single-SVF cost (`module_subtractive.md`'s ~50–60); the combined chassis+filter+partial total is not yet measured together |

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
originally carried, *for the bare partial kernel*. With the per-voice
filter now built (Per-Voice Filter above) and using bilinear partials: 8
voices × (~37.6 partial + ~20 chassis + ~110 for two filter passes) ≈ 1340
c/f (~39% of budget), comfortably under the ≤50% target with margin for
FX. The same math at `MAX_VOICES = 16` lands around 2680 c/f (~79%) — this
is the arithmetic Decision Record entry 13 uses to justify 8 over 16, and
it's still an estimate: none of the chassis or filter numbers above have
an actual hardware measurement behind them yet, only the bare partial-read
kernel does.

### Future / TODO

- **Measure the combined per-voice chassis on hardware** (partial +
  envelope + two filter passes, `MAX_VOICES = 8`, real PPG data loaded) —
  the estimate in Performance above (~39% of budget) is not yet confirmed;
  this is the next concrete hardware pass, more urgent than the partial-pool
  design below since it validates whether `MAX_VOICES = 8` has the margin
  Decision Record entry 13 assumes.
- **A second oscillator per voice** — real PPG hardware has 2 per voice;
  this module has 1. Needs its own design pass (a second `wave_pos`/`table`/
  `phase` per voice, an oscillator mix or sync control), not a trivial
  VoiceParams addition, and should wait for the chassis measurement above
  since it roughly doubles the partial-read cost per voice.
- **A true SSM2044-style filter** (saturation, self-oscillation character)
  — the current 2×-cascaded SVF is a deliberately basic stand-in (Per-Voice
  Filter above); only worth revisiting once the rest of the chassis is
  measured and the basic version's sound has actually been judged
  insufficient.
- **Per-wavetable filter cutoff/resonance and envelope amounts** — currently
  fixed global constants, not sourced from a patch. The factory cassette
  dumps `tools/ppg/` also recovered decode to fixed-size Program records
  (51 bytes × 100 programs for Wave 2.2/2.3, 50 bytes × 102 programs for
  Wave 2) whose full parameter list is now known
  (`docs/research/ppg-wave-23-sound-architecture-and-data-formats.md`), but
  the exact byte offset of each parameter within a record is still
  unresolved — a candidate source once/if that byte layout gets pinned
  down.
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
7. **Wave tables store a handful of authored keyframes plus a 64-entry
   blend index, not 64 full waves.** The kernel already pays for a
   wave-position lerp on every sample regardless, so blending between two
   stored keyframes at read time instead of reading one full wave directly
   is free — see Wave Storage and Memory.
8. **The skeleton repurposes CC1 (mod wheel) as a live wave-position scan**,
   not a vibrato depth — this skeleton has no LFO yet, and a continuous
   wave-position sweep is this module's own PPG-style analogue of what
   every other engine uses CC1 for, not an arbitrary substitution.
9. **The skeleton's envelope, FX chain, and pan law are reused unmodified**
   from the shared layer (`src/envelope.h`, the per-module FX block every
   engine already carries, `pan.h`) rather than written fresh — none of
   that is wavetable-specific, and duplicating it would only risk drift
   from the versions every other module already exercises.
10. **Real PPG Wave 2.3 ROM data is the primary wave source, converted
    offline and gitignored, not checked in.** `tools/ppg/convert_ppg_waves.py`
    reads the already-recovered ROM extraction (`tools/ppg/README.md`) and
    bakes both the waveform samples and each table's keyframe → 64-position
    interpolation into a generated header (`ppg_waves.h`) — the device
    never runs that expansion, matching `xm2t00t`'s "precompute at
    conversion time" idiom (`module_tracker.md`'s Song Blob Format
    section). Gitignored for the same reason the FM module's `patches.h`
    is: third-party commercial ROM content, not something to check into
    git history. `tables.h` falls back to procedural tables when it hasn't
    been generated (same `EXISTS`-gated-macro convention as
    `T00T_FM_HAS_PATCHES`).
11. **`WT_TABLE_SIZE` was changed from an arbitrary 128 samples to 64**,
    matching real PPG ROM waveforms exactly, once real data was available
    to check against — the original 128 was a guess made before any real
    wave data existed. The read kernels' instruction count is unaffected
    (`WT_TABLE_SIZE` only changes a mask/shift constant, not the number of
    taps or lerps), so this didn't need to invalidate the hardware
    measurements taken at the old size.
12. **The Per-Voice Filter (SSM2044-style) is a true per-voice filter, not
    a shared bus — superseding entry 6 above.** Entry 6 proposed a
    `FilterBus`-style shared pool specifically to bound worst-case cost.
    Real PPG hardware gives every voice its own filter and VCA (PPG
    Architecture Reference), and `MAX_VOICES = 8` (entry 13) keeps the
    arithmetic comfortable for doing the same thing directly (Performance
    above) — so the cost-bounding problem entry 6 solved for doesn't apply
    at this voice count, and matching real hardware's actual topology is
    both simpler and more faithful to the stated goal of matching the PPG
    sound. The shared-bus approach remains available if a future, larger
    `MAX_VOICES` ever needs it.
13. **`MAX_VOICES` is 8, matching real PPG Wave 2.2/2.3 polyphony, not 16.**
    The measured partial-kernel numbers alone (Performance) would support
    far more than 16 voices unfiltered, but the *filtered* per-voice cost
    (partial + envelope + two SVF passes) has no hardware measurement yet
    — only estimated. At 8 voices the estimated total (~39% of budget)
    comfortably clears the ≤50% target other modules use even if the
    filter estimate is somewhat low; at 16 voices the same estimate
    (~79%) would not, and shipping that specific combination unmeasured
    was the risk this decision avoids. Revisit once the combined chassis is
    actually measured (Future/TODO).

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
