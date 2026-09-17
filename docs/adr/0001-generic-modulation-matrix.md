# Generic modulation matrix, piloted in wavetable

Status: proposed

## Context

Modulation routing is hardcoded per module today. `subtractive`'s
`VoiceParams` (`src/engines/subtractive/engine.h`) carries four dedicated
fields — `lfo_depth`, `lfo_pitch_depth`, `lfo_pwm_depth`,
`lfo_filter_depth` — each applied via its own handwritten branch in
`subtractive/audio_engine.cpp` (`if (lfo_depth_q15 > 0) ...`, repeated per
destination). `fm` and `speech` each reimplement their own LFO phase/depth
logic independently — only `envelope.h`/`envelope.cpp` is actually shared.
Adding one new route (e.g. "LFO → pan") means a new struct field, a new
preset field, and new engine code, in every module that wants it.
`wavetable` currently has none of this infrastructure at all (no LFO;
`module_wavetable.md`'s only modulation is a live CC1 wave-position scan
and a fixed ADSR→cutoff amount).

PPG Wave 2.3 hardware solves the same class of problem generically, not
per-destination:
`docs/research/ppg-wave-23-sound-architecture-and-data-formats.md`'s
"Modulation sources and matrix" documents a fixed set of five sources
(**K**eyboard position, **M**LFO, **T**ouch, **V**elocity, **B**ender) and
four destinations (**W**ave-position, **F**ilter cutoff, **L**oudness,
**M**od-intensity), connected by 12 genuine source×destination switches —
not one dedicated field per route. Wavetable's own defining mechanic,
wave-position scanning, is literally the summed output of that matrix on
real hardware (keyboard-tracking + Envelope 1 + LFO + aftertouch all
contributing to one 0–63 value) — this project's own `osc/wavetable.h`
wave-position axis already conceptually matches it.

## Decision

Adopt a generic modulation-matrix concept — module-agnostic *sources*,
module-specific *destinations*, applied by one shared routine — and build
it first in `wavetable` rather than retrofitting `subtractive`.

- **Sources** split into control-rate scalars (velocity, keyboard-tracking,
  aftertouch, mod wheel, bend — set by Core 0 at note-on/CC time) and
  audio-rate signals (LFO, envelope — resolved once per sub-block on Core
  1). This split isn't a style choice: `engine.md` states `VoiceParams`
  carries no phase state, and per-voice runtime state (phase, envelope,
  filter, LFSR) lives in file-scope arrays owned solely by Core 1 — so
  audio-rate sources can only ever be resolved on Core 1. Both kinds land
  in one small `int16_t src[N_SOURCES]` per voice, matching the existing
  Q15 convention (`lfo_depth_q15`, pan, amplitude).
- **Destinations stay module-specific** — an `enum Dest` and an
  `int32_t dest_accum[N_DEST]` per module, matching PPG's own fixed
  4-destination precedent. This is a routing layer over each module's
  *existing* modulation targets (cutoff, wave-position, amplitude, ...),
  not a rewrite of them.
- **The matrix-apply loop runs entirely on Core 1**
  (`dest_accum[d] += (src[s] * depth) >> 15` per active slot), at the same
  sub-block site `subtractive`'s own hardcoded LFO application already runs
  at today — not split across cores. Splitting it would require either
  duplicating Core-1-owned phase state on Core 0 or merging two partial
  results per sample, for no benefit.
- **v1 is capped at a small fixed slot count** (proposed: 4, matching
  `subtractive`'s current route count, not PPG's 12) and **excludes
  depth-modulating-depth** (PPG's `TM` route, aftertouch modulating another
  route's own depth) — real added complexity with no current use case.
- **Wavetable is the pilot**, not the newest module by coincidence: it has
  zero existing modulation infrastructure to migrate, and wave-position
  scanning is already conceptually this exact mechanism.

## Considered Options

- **Keep per-module hardcoded routing (status quo).** Rejected — every new
  route is a multi-file, per-module change, and LFO/envelope-application
  code is already duplicated across `fm`/`subtractive`/`speech` with no
  shared abstraction.
- **Matrix-apply on Core 0.** Rejected — audio-rate sources (LFO, envelope)
  only exist as Core-1-owned phase state; the loop has to live where its
  inputs live.
- **Match PPG's exact 5×4 fixed matrix.** Rejected for v1 — PPG's shape is
  hardware-constrained (fixed switches, not continuous depths); a smaller,
  continuous-depth slot model is more useful in software and cheaper to
  bound the cost of.

## Consequences

- `subtractive`'s existing four hardcoded LFO routes are explicitly **not**
  touched by this decision. Migrating them to the matrix is a separate,
  later decision once the shape is proven in `wavetable`.
- Per-sample/sub-block cost of the matrix vs. today's hardcoded branches is
  **not yet measured** — needs a rig pass (`WT_RIG_MODE`-style) before this
  lands in wavetable's real chassis, matching this project's own
  measure-before-committing precedent (`module_wavetable.md`'s Decision
  Record entry 13, on `MAX_VOICES`).
- Gives `fm`/`subtractive`/`speech`'s independently-duplicated LFO code a
  natural forcing function to unify into a shared source-resolution helper,
  as a prerequisite for those modules ever adopting the matrix too.
- **Deferred to v2: caching control-rate slot contributions.** v1 recomputes
  every active slot's `dest_accum` contribution every sub-block,
  unconditionally. For a slot sourced purely from a control-rate scalar
  (e.g. `VELOCITY → CUTOFF`), that value is constant for the life of a held
  note, so this recomputes an identical result for however many sub-blocks
  the note sustains — real, unbounded-with-sustain-time waste, not a
  rounding error, unlike audio-rate sources (LFO/envelope) which are
  legitimately dirty every sub-block and gain nothing from a dirty check.
  The natural hook is `ParamExchange`'s existing commit/flip signal, which
  Core 1 already observes each pass — cache the control-rate contribution
  and only recompute it when a new block just committed, rather than
  building a separate dirty-bit mechanism. Not in v1: ship the flat
  recompute-every-sub-block version first, measure it on the rig, and only
  add this split if the numbers say it matters — same
  measure-before-committing reasoning as the rest of this ADR.
