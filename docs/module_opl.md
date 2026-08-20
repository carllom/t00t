# T00T — OPL Module (OPL2-class)

A 2-operator phase-modulation engine targeting Yamaha OPL2 (YM3812,
AdLib/Sound Blaster-class) feature parity. It is a *mode* — a build-time
engine variant selected via `T00T_ENGINE=opl`. See `engine.md` for the
shared dual-core architecture; `architecture.md` for the cross-engine
`VoiceParams`/CMake pattern; `history_opl.md` for the development record.

This module shares its per-sample operator kernel with `src/engines/fm/`
(the DX7-class module) directly — `#include`s it rather than forking it —
while giving OPL its own envelope, patch data, and fixed routing tables,
none of which are extensions of FM's DX7-shaped equivalents. See
`module_fm.md` for the shared kernel's own documentation; this module doc
covers only what's specific to OPL.

## Overview

Two independently-enveloped operators per voice, one of two fixed
algorithms (FM chain or additive) — real OPL2 hardware has no free routing
or DAG concept, unlike the six-operator DX7 module.

### Specifications

- **Voices**: `MAX_VOICES = 9` — real OPL2 hardware's own channel count
- **Routing**: exactly two fixed algorithms (FM chain: op0 modulates op1,
  which carries; additive: op0 and op1 both carry), each a `constexpr
  FmRouting` literal — no runtime DAG resolution, unlike the DX7 module's
  free 6-operator routing
- **Envelope**: `EnvOpl`, one 4-stage log-domain instance per operator (2
  per voice), OPL-native rate/level/KSL/TL fields, sharing `EnvDX`'s own
  gain-conversion domain so no new conversion code was needed. Every stage
  is a linear ramp in that log domain, at a rate law verified against
  Nuked-OPL3 (`tools/opl_ctl_diff.py`); KSL and TL are exact matches to real
  hardware's own tables — see Future/TODO for the one remaining curve-shape
  gap (attack)
- **Waveform select**: 4 waveforms per operator (sine, half-sine,
  full-wave-rectified, a quarter-cycle pulse), a per-operator table pointer
  reused from FM's own per-operator table field
- **Vibrato**: one small fixed-rate sine LFO per voice, depth scaled by the
  mod wheel — not a reuse of FM's full per-patch-configurable LFO, since
  real OPL2 hardware has one global fixed-rate vibrato, not DX7's
- **Key/rate scaling, feedback, MIDI velocity**: all resolved once at
  note-on, zero per-sample cost
- **No pitch envelope, no per-operator amplitude/pitch LFO sensitivity** —
  real OPL2 hardware has neither
- **Patches**: a small hand-authored set ships in the repository
  (`patches.h`), checked in directly — no bank converter exists yet (see
  Future/TODO)

### MIDI Mapping (Input Capabilities)

| Message | Function |
|---|---|
| Note On/Off | Standard dynamic allocation (`voice_alloc`), one voice slot per note |
| Pitch Bend | Folded into `phase_inc` by Core 0 before it reaches Core 1 |
| CC1 | Mod wheel — scales vibrato depth |
| CC10 | Pan |
| CC72 | FX param 1 |
| CC73 | FX wet/dry mix |
| CC74 | FX type select |
| CC75 | FX param 2 |
| Program Change | Patch select — `OPL_PATCHES[value % OPL_PATCH_COUNT]` |

### Display (Presentation Capabilities)

Same chrome every engine's panel shares (title bar, VOICES dot bar, CPU load
bar, NOTE row), plus the current patch name (for whichever channel most
recently triggered a note or a Program Change) and a two-cell algorithm
indicator (carrier vs. modulator role per operator, feedback highlighted on
op0) — a much smaller version of the DX7 module's six-cell diagram, since
OPL only ever has two operators and two possible algorithms.

## Technical Overview

### Source Layout

```
src/engines/opl/
  engine.h            VoiceParams, VoiceParamBlock, ParamExchange
  audio_engine.cpp    audio_engine_run(): render pass, voice loop, FX insert
  opl_scale.h         OPL_TABLE_BITS -- no separate gain-domain anchor
                       (see Decision Record)
  waveforms.h         the four OPL2 waveform tables
  patch.h             OplOpParams/OplPatch (runtime form) + the two fixed
                       algorithm routings, expressed as FmRouting literals
  env_opl.h           EnvOpl, OPL's own envelope
  opl_voice.h         note-on/step/note-off/active/render voice glue,
                       calling ../fm/op.h's kernels directly
  patches.h           hand-authored test patches, checked in
  input_subsystem.cpp note on/off, bend, pan, mod wheel, patch select
  display.cpp         status panel: voices/CPU/note, current patch,
                       algorithm/feedback indicator (see Display above)
```

There is no `rig.h`/measurement rig and no free-routing DAG resolver —
OPL2 has only two fixed algorithms, so there's nothing to benchmark
topology-wise or resolve at note-on.

### Build

Build with `make ENGINE=opl`.

### Tools

`tools/host_render/render_opl.cpp` — renders every patch in `patches.h`
through the exact device code path to a WAV file and confirms note-off
actually releases the voice within a bounded tail; the practical sanity
check available without hardware.

`tools/opl_ref/` — builds [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)
(fetched at a pinned SHA, never vendored — see the DX7 module's own
`tools/fm_ref/` for the same pattern) into `nuked_render`/`nuked_dump`, the
ground-truth reference for this module. `nuked_dump`/
`tools/host_render/t00t_opl_ctl_dump.cpp` dump comparable control-plane CSVs
(the frequency-multiplier and KSL tables, the TL scale, one operator's live
envelope trajectory) from each side; `tools/opl_ctl_diff.py` diffs them
(mirroring the DX7 module's own `fm_ctl_diff.py`) — exact for the register
tables, tolerance-based for the envelope trajectories (see Decision Record).

`tools/host_render/render_opl_patch.cpp` — CLI-driven counterpart to
`render_opl.cpp` above, rendering one `patches.h` patch at a chosen note/
velocity/gate/tail through the same device code path, matching
`nuked_render`'s own CLI shape. `tools/opl_compare.py` renders a patch
through both sides and scores the pair (harmonic/attack/envelope MAE,
reusing the DX7 module's own `fm_compare.py` scorer unmodified);
`tools/opl_regress.py`/`opl_thresholds.json` sweep every patch across a
note/velocity grid against a committed baseline, mirroring the DX7 module's
own `fm_regress.py`/`fm_thresholds.json`.

## Architecture

### Kernel Reuse, Not a Fork

`src/engines/opl/` includes `src/engines/fm/op.h` directly — the first
production engine-directory-to-engine-directory include in this codebase.
That one include is where `FmOp`, the three per-sample kernels
(`op_render`/`op_render_first`/`op_render_fb`), `fm_voice_render_block()`,
`FmVoiceBuses`, the `FmRouting` type, and the shared gain-conversion
(`eg_to_gain()`, from the DX7 module's `env_dx.h`) all come from — reused
unchanged rather than reimplemented, avoiding a second, parallel copy of
the same hard-won fixed-point kernel.

Everything above that layer is OPL's own: `EnvOpl` (not `EnvDX`), `OplPatch`
(not `FmPatch`), and the note-on/envelope-step/note-off/render voice glue
(`opl_voice.h`, not FM's own `fm_voice_note_on()`/`fm_render_voice()` and
friends) — those FM functions are hardcoded to a `const FmPatch&` and to
`FmOp::eg` as a concrete `EnvDX`, neither of which fits OPL's own patch or
envelope shape.

Every voice's `FmOp` array is still the full six-wide `FM_NUM_OPS` array
the reused `fm_voice_render_block()` iterates, even though only two
operator slots ever carry real signal — slots 2-5 are permanently inert
padding, zeroed once at boot, each writing to its own never-read scratch
bus. This is a small, bounded per-voice cost traded for zero new routing
plumbing; see Status and Plan for why real per-voice cost is still an open
question rather than something optimized against ahead of measurement.

### `EnvOpl` — the OPL Envelope

Shares `EnvDX`'s own level domain (Q24 octaves, 0..15 octaves) and gain
conversion (`eg_to_gain()`, unmodified) so no new gain-domain conversion
code exists anywhere the reused kernels are involved — only the shape of
the curve leading up to that shared domain is OPL-native.

- 4 stages: attack (rate 0 = the operator never turns on, matching real
  hardware), decay, an optional sustain hold, release (rate 0 = never
  reaches silence). EG-type selects whether decay holds at the sustain
  level until note-off (sustain mode) or runs straight through it to
  silence on its own (percussive mode, matching real hardware's two EG
  types) — note-off before that finishes still jumps into a release from
  wherever the level currently sits, the same convention `EnvDX` uses.
- TL, KSL, and MIDI velocity all attenuate the same ceiling the attack
  stage rises to — composed once at note-on, the same spot the DX7 module's
  envelope folds output level/key scaling/velocity into one number, just in
  OPL-native dB units converted to this shared octave domain instead of DX7
  microsteps. Real OPL2 hardware has no velocity input at all; folding MIDI
  velocity onto TL attenuation here is a t00t-side addition for MIDI
  playability, not something the chip itself does. KSL uses real hardware's
  own ROM tables (`OPL_KSL_ROM`/`OPL_KSL_SHIFT`), read against a block/
  f-number re-derived from the MIDI note the same way real firmware would
  have programmed those registers, not a flat per-octave slope.
- Every stage is a straight linear ramp in the log domain, at a rate
  (register rate combined with real hardware's own key-scale value, per
  `opl_combined_rate()`) verified against Nuked-OPL3 — see Future/TODO for
  the one known shape gap against real hardware's curved attack.

### Voice Glue and Vibrato

`opl_voice.h` mirrors the shape of the DX7 module's own note-on/step/
note-off/active/render functions, but for `OplPatch`/`EnvOpl` and only two
real operators. A patch's algorithm (FM chain or additive) selects one of
`patch.h`'s two `constexpr FmRouting` literals; since a patch's feedback
amount isn't compile-time, it's copied into a per-voice `FmRouting` and
patched (`kernel[0]`/`fb_shift[0]`/`clear_bus_mask`) at note-on rather than
baked into the literal — real hardware's single per-channel feedback
register lives on operator 0 only.

Vibrato is one small fixed-rate sine LFO per voice (not a reuse of the DX7
module's full per-patch-configurable one — real OPL2 hardware has a single
global fixed-rate vibrato, not a per-patch one), scaled by the mod wheel and
folded into both operators' phase increment once per control block.

## Status and Plan

### Performance

Not yet measured on real hardware — the GPIO-22 profiling-pin rig every
other engine's per-voice cost figure comes from hasn't been run against
this module yet. The six-wide (mostly inert) per-voice operator array (see
Architecture) means "OPL is cheaper per voice than the DX7 module" is a
hypothesis this measurement still needs to confirm, not a number designed
around ahead of time.

### Future / TODO

- **Non-linear attack curve** — real OPL2 attack is a curved
  (fast-then-slower) shape; this module's attack is a linear ramp
  calibrated to the same real total duration (`tools/opl_ctl_diff.py`'s
  `eg/attack-*` cases, at a deliberately wide tolerance), not the shape
  itself. Closing this would need the ramp itself to become non-linear, not
  just another round of rate-table tuning.
- **Patch bank converter** — hand-authored patches only for now; no
  `.op2`/GENMIDI-class converter exists yet.
- **OPL1 (sine-only subset) and OPL3 (4-operator)** — not started. The
  per-operator waveform table pointer and OPL's own (not DX7-derived) patch
  struct were chosen so neither forecloses either expansion.
- **Rhythm/percussion mode** — deferred, not dropped: it needs a
  genuinely different kernel shape than the phase-accumulator-plus-table
  approach every voice here uses, so it wasn't needed to validate the
  kernel-reuse approach this module is built on, but a longer-term
  AdLib-tracker-format-playback goal depends on it.
- **Per-voice display grid** — the DX7 module's multitimbral grid (which
  voice is playing which patch on which channel) has no OPL equivalent
  yet; this module's panel currently only shows the most-recently-touched
  channel's patch.

## Decision Record

1. **The per-sample kernel is included from the DX7 module directly, not
   forked.** `FmOp`, the three render kernels, `fm_voice_render_block()`,
   and the gain-conversion math are the expensive-to-get-right part of
   either module — reusing them verbatim means a fix or a future
   optimization to that shared kernel benefits both engines at once,
   rather than needing to be ported by hand into a second copy.
2. **Every voice's operator array stays six-wide, with four inert slots**,
   rather than a hand-rolled two-operator loop, so the reused
   `fm_voice_render_block()` (and its routing-literal input shape) could be
   used completely unchanged. The bounded extra per-sample cost on the four
   dead slots is accepted as a skeleton-stage tradeoff, not treated as
   settled — see Status and Plan.
3. **OPL's patch and envelope types are not extensions of the DX7 module's
   `FmOpParams`/`EnvDX`.** OPL has waveform select and an EG-type flag DX7
   doesn't; DX7 has ratio/detune/fixed-frequency/key-scaling-curve fields
   OPL doesn't. Stretching one shape to cover both would have made either
   module's real fields optional noise in the other's patch data.
4. **No OPL-specific gain-domain anchor** (`opl_scale.h` defines only the
   waveform table width) — the reused kernels read the DX7 module's
   `fm_scale.h` constants (`FM_CYCLE`/`FM_MOD_SHIFT`/`FM_GAIN_MAX`) as plain
   globals, not template parameters, so this module inherits that
   bus-unit and gain-ceiling convention verbatim rather than choosing a
   second, independent one that the same kernel code would then have to
   silently satisfy both of.
5. **MIDI velocity attenuates TL even though real OPL2 hardware has no
   velocity input.** A MIDI-driven instrument needs velocity to do
   something audible; folding it onto the same output-level attenuation
   TL/KSL already use was the smallest addition that didn't need a new
   composition point in the envelope.
6. **Attack is a linear ramp, not real hardware's curved shape**, even after
   `tools/opl_ctl_diff.py`'s conformance pass — a working, testable,
   monotonic attack calibrated to the real total duration, with the
   curve-shape gap explicitly tracked (Future/TODO) rather than either
   rewriting the stage machinery to support a non-linear ramp or silently
   claiming accuracy the implementation doesn't have.
7. **`opl_ctl_diff.py`'s envelope trajectory tolerance is deliberately
   asymmetric** — tight (0.5 dB mean/5 dB max, matching the DX7 module's own
   `fm_ctl_diff.py`) for decay/release/percussive, since those are honestly
   linear on real hardware too; wide (10 dB mean/45 dB max) for attack,
   since no tolerance can make a linear ramp both pass a real exponential
   curve's shape and still catch an actual rate regression — the tolerance
   is sized to do the latter, not to hide the former.
8. **CC16 (the Program-Change-alternative encoder patch select) was
   dropped**, migrating onto the Core 0 input pipeline (Router,
   `src/input_layer.h`) — it routed to the exact same patch-select logic
   Program Change already did, so keeping it would only have been a second
   table entry for one setter. Program Change alone now selects the patch.

## Glossary

- **Operator**: one waveform generator with its own envelope and frequency
  multiplier — two per voice, versus the DX7 module's six.
- **Algorithm**: OPL2 has exactly two — an FM chain (one operator
  modulates the other, which carries) and additive (both operators carry
  independently) — versus the DX7 module's 32-plus-free-DAG routing.
- **Carrier**: an operator whose output is audible directly.
- **Modulator**: an operator whose output phase-modulates the other
  operator instead of being audible directly.
- **Feedback**: operator 0's output modulating its own phase — real OPL2
  hardware's single per-channel feedback register, unlike the DX7 module's
  per-operator feedback field.
- **EG-type**: whether an operator's envelope holds at its sustain level
  until note-off (sustain) or decays straight through it to silence on its
  own (percussive) — an OPL-native flag with no DX7 equivalent.
- **KSL (key scale level)**: extra output attenuation for higher notes,
  4 settings including off — OPL's analogue of the DX7 module's key level
  scaling curve, in different units.
- **TL (total level)**: an operator's programmed output level, 0-63 OPL
  units in 0.75 dB steps — the DX7 module's equivalent field uses 0-99 DX7
  units on a different (though also log-domain) scale.
