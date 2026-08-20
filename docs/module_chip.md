# T00T — Chip Synthesis Module

A chip-music synthesis engine built around the MOS 6581/8580 SID, structured
to admit other 8-bit sound chips (AY-3-8910/YM2149 shipped; SN76489, NES
2A03, GB DMG considered) without rework. It is not a SID player and not a
`.sid` file emulator — it is a synth whose voices sound like SID (and
family) voices and whose instruments are expressive the way those chips'
instruments were expressive, via a per-frame table VM, not via LFOs. See
`engine.md` for the shared dual-core architecture; `history_chip.md` for
build-phase results, measurement scorecards, and bug-discovery narrative.

## Overview

### Specifications

- **Voices**: 32, dynamic allocation (`voice_alloc`), flat pool — one note
  is always exactly one voice, no group/unison allocation
- **Chip families**: SID (6581 shipped, LUT-fitted cutoff + saturation;
  8580 deferred — see Future/TODO), AY-3-8910/YM2149 (shipped). SN76489,
  NES 2A03, GB DMG, POKEY considered but not started.
- **SID voice**: 24-bit phase accumulator (sawtooth/triangle/pulse/noise,
  plus an AND-approximated combined-waveform mode), a rate-counter envelope
  with piecewise-exponential decay segments, and a per-voice sub-oscillator
  field for sync/ring modulation (not yet wired into the real engine — see
  Future/TODO)
- **AY voice**: tone + noise + hardware envelope generator, 6
  hand-authored instruments including an AY8910/YM2149 model switch
- **Frame table VM**: per-instrument ADSR + vibrato + three per-frame
  tables (wave/pulse/filter), 50 Hz default frame rate, runs on Core 1
  inside the render pass
- **Filter buses**: 4, a typed pool voices route into (not a per-chip
  filter) — bind-or-degrade-to-unfiltered policy
- **Speaker simulation**: an output stage (not an effect), 5 presets
  (Commodore 1702, portable TV, Game Boy, arcade cabinet, bypass) sharing
  one HP→peak→LP→soft-clip signal chain
- **Effects**: shared post-mix insert (delay or reverb), upstream of the
  speaker stage
- **Instruments**: 4 SID (`ARP_LEAD`, `PWM_PLUCK`, `FILTER_PAD`,
  `VIBRATO_LEAD`) and 6 AY (`LEAD`, `BUZZ_BASS`, `NOISE_PERC`, `ARP`,
  `PLUCK`, `LEAD_YM`), hand-authored; GoatTracker `.ins` files are
  importable with documented scope limits (see Tools)

### MIDI Mapping (Input Capabilities)

Notes trigger voices through the standard dynamic allocator; pitch bend is
per-channel, live-pushed to every held voice on that channel.

| CC / Message | Function |
|---|---|
| Note On/Off | Standard dynamic allocation |
| Pitch Bend | Per-channel, applied to every currently-held voice |
| CC17 | Speaker simulation preset (global, applies immediately) |
| CC72 | FX param 1 |
| CC73 | FX wet/dry mix |
| CC74 | FX type select |
| CC75 | FX param 2 |
| Program Change | Configuration: instrument select (next note) — one combined space spanning both SID and AY instruments |

A player picks a patch, not a silicon — instrument index `< INSTRUMENT_COUNT`
is SID, the rest is AY, but the selection is one linear list either way.

### Display (Presentation Capabilities)

Shows voices/CPU/last-note (same shape as the subtractive and speech
displays), the active speaker preset name, the currently selected
instrument's name, and a fixed 8-voice grid (of 32) showing each voice's
instrument index and current wave-table row, colour-coded held vs.
ringing-out. The grid covers voices 0–7 only — `voice_alloc` always
allocates from voice 0 first, so this is representative up to 8-note
polyphony; past that the voice-count row stays exact but the grid stops
being the whole picture. Both SID and AY voices report real telemetry
(instrument names are shown only on the one-line instrument-select row,
not in the grid, which stays numeric — a name doesn't fit eight cells at
once).

## Technical Overview

### Source Layout

Topology-free primitives, shared between the firmware and the host
validation harness:

- `src/chip/sid_osc.h` — accumulator, waveform logic, 23-bit noise LFSR
- `src/chip/env_sid.h` — the rate-counter envelope
- `src/chip/sid_filter.h` — SVF with a mode mask, pluggable cutoff LUT, 6581 saturation, the C64 board's output network
- `src/chip/sid_voice.h` — the fixed-point output contract
- `src/chip/sid_tables.h` — generated, committed cutoff/DAC tables
- `src/chip/sid_chip.h` — the real chip's 3-voice/1-filter/adjacency-sync topology; used only by the `CHIP_STRICT` host harness and P1's register-stream playback path
- `src/chip/ay_osc.h` — `AyTone`, `AyNoise`
- `src/chip/ay_envelope.h` — `AyEnvelope`, DAC tables, `ay_mix()`

Engine (`src/engines/chip/`):

- `engine.h` — `VoiceType` (`VT_SID`/`VT_AY`), `MAX_VOICES = 32`
- `audio_engine.cpp` — per-voice dispatch, two-phase bus render, frame VM tick
- `input_subsystem.cpp` — the Input pipeline's module-specific tail:
  mapping table, Handlers, Voice Allocation Interface calls, and
  filter-bus binding (a second, chip-specific allocator-like concern
  resolved in the same NOTE Handler as voice allocation), built on
  `src/midi/midi_dispatch.h`/`midi_controller_generic.h`'s shared generic
  dispatch layer (also used by `subtractive`/`fm`)
- `display.cpp` — LCD status
- `instrument.h` / `instruments.h` — SID instrument format / GENERATED table
- `ay_instrument.h` / `ay_instruments.h` — AY instrument format / hand-written table
- `speaker_sim.h` — the output stage
- `note_freq.h` — frequency-register conversion
- `rig.h` — the P0-era CPU measurement rig, preserved behind `CHIP_PROFILE=1`

### Build

Build with `make ENGINE=chip`. `CHIP_PROFILE=1` builds the measurement rig
(`rig.h`) instead of the real MIDI-driven engine — preserved so the
hardware-measured numbers in Status and Plan stay re-measurable against
later changes. `CHIP_RIG_*` flags (voices, buses, filtered/unfiltered,
oversampling, saturation, wave DAC) each require a separate build, since a
runtime switch would put a branch inside the loop being measured.

### Tools

`tools/chipgen.py` — hand-authored text (`tools/chip_instruments.txt`) →
`src/engines/chip/instruments.h`. `tools/ins2chip.py` — GoatTracker `.ins`
→ the same text format, feeding `chipgen.py` rather than replacing it.
Refuses (hard error) rather than mistranslates constructs the frame-table
model can't represent: WAVECMD rows, absolute-pitch wavetable rows, and
per-song shared-table tricks. Verified against 200 real community
instruments (89 converted correctly; the rest are documented, correct
refusals).

`tools/sid_ref/` and `tools/ay_ref/` — reference-diff harnesses against
reSID and ayumi respectively; see their own `README.md` for setup.
`tools/sid_ctl_diff.py` / `tools/ay_ctl_diff.py` — exact control-plane
diffs. `tools/sid_compare.py` — spectral scorecard, reused unmodified for
both chips (generic over any two float32 WAVs).

## Architecture

### Topology-Free Primitives and the `CHIP_STRICT` Split

Every primitive in `src/chip/` knows nothing about chips, buses,
allocation, or grouping. This is what lets the same code run in two
different topologies:

| | `CHIP_STRICT` | Free mode (this engine) |
|---|---|---|
| Where | host only, `tools/host_render/render_sid.cpp` | firmware |
| Topology | 3 voices/chip, 1 shared filter, adjacency-wired sync | flat 32-voice pool, typed filter buses, per-voice sub-oscillator |
| Input | register write stream | MIDI + frame VM |
| Validated against | reSID/ayumi, spectral + exact diff | inherits primitive correctness |

Free mode is the same validated primitives wired differently — the whole
reason the topology-free rule exists is that this costs almost nothing if
planned for and is painful to retrofit.

### Per-Voice Dispatch and Allocation

Each voice carries a `VoiceType` tag (`VT_SID`/`VT_AY`), dispatched in the
render loop the same way the groovebox dispatches its own `VoiceType` —
the architectural template this whole module follows. `voice_alloc` is
reused unmodified: flat pool, three-tier steal policy (silent → released →
oldest active), no chip-specific grouping.

Two bitmasks track voice state for different purposes: `active_mask`
(`env.counter > 0`, "still audible" — what the allocator's steal policy
needs) and `render_mask` (`p.gate || env.counter > 0` — what the frame-VM
tick and bus-feed loops gate real DSP work on). They differ because a
freshly hard-restarted voice's envelope counter is momentarily 0 before
the attack has had any samples to ramp it, and because a voice's `type`
alone is set once at note-on and never reset, so gating on type instead of
these masks would keep ticking a fully-decayed voice forever.

### SID Oscillator, Waveforms, and Envelope

The accumulator is Q24.8 in a `uint32_t` (the 24-bit hardware accumulator
shifted left 8). Hard sync is plain wrap detection on the accumulator's
MSB rising; the 16-bit frequency register stays the control-plane unit, so
authentic pitch/vibrato quantisation comes for free. Waveforms: sawtooth
is the top 12 bits directly, triangle XOR-folds them about the MSB, pulse
compares against a threshold, noise is a 23-bit LFSR scattered into the
output bits. Combined waveforms (e.g. pulse+triangle) are a bitwise AND —
a nonlinear analog artifact on real hardware, not a bitwise operation, so
this is a documented approximation, cheap but with real audible error on
some combinations (see Future/TODO for the LUT alternative).

The envelope (`EnvSID`) is a rate-counter design with piecewise-exponential
decay segments, replaced at 44.1 kHz by precomputed per-sample increments
that replicate the same segment shape; the 4-bit ADSR nibbles and their
rate tables are the control-plane interface, preserved verbatim. Hard
restart is an instantaneous envelope reset (zero latency) rather than the
hardware's own 1–2 frame ADSR-delay-bug workaround — that bug isn't
modelled, so there's nothing to dodge; an imported instrument carrying a
hard-restart ADSR value simply lands on the fast path.

The per-voice sub-oscillator field (`mod_acc`/`mod_inc`/`mod_mode`, for
sync/ring modulation) exists in the primitives and in the `CHIP_STRICT`
harness's adjacency topology, but is not wired into this engine's own
per-voice state yet — see Future/TODO.

### Filter Buses

Voices carry a `filter_bus` index into a small typed pool of 4
(`FilterBusParams` — model, mode mask, cutoff, resonance), not a
per-voice or per-chip filter. This bounds worst-case CPU by construction
rather than letting it depend on what's played.

Binding policy: a channel that already owns a bus reuses it; an unowned
channel takes any free bus; a channel that finds none free renders
unfiltered rather than stealing one. Bus ownership is per-channel and
sticky (held for the channel's lifetime). Last-write-wins when several
notes share a bus and their filter settings disagree — the same thing
real chip-tune drivers did, and the simplest implementation. Because
binding is 1:1 per channel, a bound bus's tonal parameters are read
directly from whichever voice is currently feeding it (tracked in the
same per-sub-block voice scan the render loop already does) rather than
pushed separately from Core 0 — once a voice carries an instrument index,
there's nothing left for a separate live channel to usefully carry.

Idle buses (no voice currently feeding them) skip their clear+filter tick
individually rather than all-or-nothing, and a bus's filter state resets
the moment its voice count goes from zero to nonzero, so a new voice
binding to a just-woken bus doesn't inherit the previous voice's resonant
tail.

Render is two-phase per sub-block: clear bus accumulators, render each
voice into its bus (or straight to the dry mix if unbound), then filter
each bound bus into the dry mix. Unfiltered voices skip the round trip
entirely.

### Frame Table VM

An instrument is ADSR + vibrato + three per-frame tables (wave, pulse,
filter), runs on Core 1 inside the render pass, sub-blocked the same way
the render loop itself is. This is a superset of "LFO + arpeggiator +
envelope", not a subset — a per-frame table is an arbitrary function of
time, including one-shot transients an LFO can't produce, and the 50 Hz
steppiness is not smoothed over: the quantisation is the sound.

Format decisions not fixed by the general model: wave-table `note` values
are relative to the played note, not absolute (matching every real
table-model editor's arpeggio convention). A table's `loop` field holding
`>= len` sustains the last row forever instead of wrapping; `loop < len`
jumps back there. Pulse and filter tables share one row shape (a per-frame
delta held for a duration) since both are ramps with identical stepping
logic.

Arpeggio and vibrato apply as adjustments to the frequency register Core 0
already computed (pitch bend included), not as a Core-1 recompute from a
raw note number: arpeggio multiplies by a precomputed Q16 semitone-ratio
table, vibrato adds a frame-stepped triangle LFO's raw register delta.
Pitch bend therefore composes for free — whatever Core 0 baked into the
register survives both untouched. Vibrato depth is a raw register-wobble
scale, not calibrated to cents or semitones.

Sync/ring toggle bits and `mod_inc` sweeps are reserved in the wave-table
row format but not read by the VM yet — see Future/TODO, since the
sub-oscillator they'd control isn't wired into the real engine either.

### AY-3-8910/YM2149

Tone + noise + the chip's own hardware envelope generator, sharing the
same `MAX_VOICES` pool and dynamic allocator as SID voices (`VT_AY`
alongside `VT_SID`). AY's own instrument format grew a tone table
(arpeggio) and reuses the SID engine's sweep-table shape for a software
volume envelope — the mechanism every AY tracker instrument needs, since
the chip itself has no hardware release.

Arpeggio and vibrato are deliberately not the same math as SID's: AY's
period register is inversely proportional to pitch, so arpeggio divides
the base period by the same semitone-ratio table SID's vibrato multiplies
by (exact), while vibrato stays additive-on-period, a small-angle
approximation scaled by the *current* period so the fractional deviation
stays constant across the pitch range (a fixed absolute delta would wobble
far more at high pitches than low ones, since period is inverse to
pitch).

AY's own mixer/DAC output is unipolar (silence is a literal 0, not a
centred value); it's converted to bipolar by treating the mixer's gate bit
as ±1 before scaling, so it sums correctly into the same bipolar bus SID
voices populate. A real AY-3-8910 quirk — both tone and noise disabled
outputs a constant level, not silence — lands as a half-scale residual DC
under this fix rather than the full DC a naive sum would produce; real
hardware is very likely AC-coupled at its analog output the same way, so
this reads as a missing "free bonus" output stage rather than a logic
error. `AyInstrument` carries a model field (AY8910 vs YM2149) selecting
between two measured DAC curves.

### Speaker Simulation Stage

An output stage, not an effect — downstream of the delay/reverb insert,
upstream of the final clip. One shared HP→peak→LP→soft-clip chain; each
of the 5 presets is a set of corner frequencies and a drive scalar into
one fixed cubic clip curve, not a separate signal path. Mono, duplicated
to both output channels. The bypass preset short-circuits the chain
entirely (`return x` before touching any filter state) rather than tuning
corners to be inaudible — a resonant two-pole filter can't be tuned into a
truly flat response by widening its bandwidth past what its pole radius
supports, so an actual bypass is the only way to guarantee transparency.
Preset selection is global (one `VoiceParams`-replicated CC, since chip
has no other Core0→Core1 channel for a single scalar), not per-voice.

### Host Validation

Mirrors `tools/host_render/`'s existing pattern. For each chip, validation
splits into an exact control-plane diff (every primitive's output compared
bit-for-bit, or within float32 precision for DAC tables, against the
reference implementation) and a spectral scorecard (level/band/attack/
envelope error in dB, computed over a corpus of representative streams).
The exact diff catches errors a spectral score could hide (two mistakes
cancelling); the spectral score catches the aliasing and approximation
error the exact diff can't express, since t00t generates directly at
44.1 kHz with no band-limiting the reference implementations both use.

## Status and Plan

### Performance

Target configuration is 20 voices / 4 filter buses. All per-item figures
below are now cross-checked against the real dynamically-routed engine,
not just P0's compile-time-fixed-routing rig (`history_chip.md` §14h,
§14i). SID voice, unfiltered (`ARP_LEAD`) ~102–108 c/f (~3.1%) regardless
of which FX/speaker stage is layered on top (confirmed additive, no
cross-term); a voice bound to its own filter bus, all 4 buses bound
(`FILTER_PAD`) ~105–110 c/f (~3.2%) once every bus is already bound,
rising above that only while buses are still coming online one at a
time; AY voice ~47 c/f (1.4%). Fixed (not per-voice) stage costs: delay
~46 c/f (1.3%), reverb ~257 c/f (7.6%), speaker simulation ~80 c/f
(2.4%, `arcade` preset — the other four presets share the same DSP
shape). Frame VM cost is folded into the SID figures above (both test
instruments drive their own frame-VM tables) and confirmed small, though
not isolated as its own line. **Worst case** (20v / 4 buses bound /
reverb / speaker): 16v measured directly at 66.0%, extrapolated to 20v
via the measured 8→16v slope at ~78.6% — notably *below* P0's rig-derived
86.6%, i.e. the real dynamically-routed engine has more headroom here
than the conservative rig estimate implied. Hard restart under rapid
retrigger (2/4/8 voices held, both `FILTER_PAD`'s long release and
`ARP_LEAD`'s abrupt one) showed no CPU spikes, distortion, or glitches —
`FILTER_PAD` allocates additional voices under rapid retrigger rather
than reusing the still-ringing one, which is the three-tier steal
policy working as designed, not an anomaly. Full breakdown, including
two real bugs found and fixed during the original P0 measurement (a
pathological rig-only envelope config, and a software-divide in the
filter saturation path): `history_chip.md`.

### Future / TODO

- **8580 filter model** — deferred. The only available source for
  combined-waveform LUT data (reSID's own sampled tables) is GPL-2 and
  deliberately not vendored; unlike the DAC ladder, combined-waveform
  behaviour has no known closed-form derivation to independently re-derive
  from. Raised as a licensing question, not resolved.
- **Combined-waveform LUTs** — the current bitwise-AND approximation has
  documented error up to ~200 dB on some pulse combinations; a correctness
  fix, not polish, but blocked on the same licensing question as the 8580
  model above.
- **Per-voice sub-oscillator (sync/ring) is not wired into the real
  engine** — the fields and the `CHIP_STRICT` harness's adjacency topology
  exist, but no MIDI/instrument path sets them yet, and the wave-table
  format's sync/ring toggle bits and `mod_inc` sweep target go unread as a
  result.
- **Other chip families** — SN76489, NES 2A03, GB DMG, POKEY — as
  additional `VoiceType`s in this same module, not new modules.
- **Multispeed frame rate** (2×/3×/4×) — open question; affects the
  instrument data format, so it wants deciding before more assets are
  generated at the current 50 Hz-only rate.
- **GoatTracker `.ins` import gaps**, all documented refusals rather than
  silent mistranslation: WAVECMD rows (no frame-VM equivalent),
  absolute-pitch wavetable rows (chip's rows are always relative),
  per-song shared-table tricks (not reconstructable from one instrument
  file alone), and standalone-file vibrato commands (pattern-level in
  GoatTracker, not present in a `.ins` at all).

## Decision Record

1. **Signal-path limitations are kept; structural limitations are
   discarded freely.** A limitation is structural if it operates on
   routing or allocation (one filter per 3 voices, sync wired to a fixed
   neighbour, exactly 3 voices per chip) — real hardware constraints with
   no tonal consequence, discarded. A limitation is tonal if it operates
   inside the signal path (16-bit frequency-register quantisation, 4-bit
   master volume, frame-quantised control, the 6581's nonlinear filter,
   summing-node intermodulation) — kept, because it *is* the sound.
2. **Modulation is a per-voice field, not an allocated voice.** A sync/
   ring modulator is just an accumulator and a frequency — far cheaper
   than a whole voice — and this removes `channels_needed` from the
   instrument format entirely: every note is exactly one voice.
3. **Filter buses are a typed, pooled resource**, not a per-voice or
   per-chip filter — a CPU budget guard, bounding worst-case load by
   construction. `FILTER_BUS_COUNT = 4` was confirmed by measurement: a
   bus is cheaper than a voice, so trading buses for voices (tried at
   22 voices / 2 buses) measured worse, not better, than the chosen
   20v/4f split.
4. **The frame VM runs on Core 1**, not Core 0 — sample-accurate frame
   boundaries (Core 0 reading `ParamExchange` once per buffer would
   quantise frame timing to buffer edges, and can't keep up above ~2×
   speed without adopting an ordered ring), and instrument tables are
   small enough to stay resident in SRAM, so the XIP-thrash concern that
   moved the tracker's player to Core 0 doesn't apply here.
5. **Combined waveforms start as a bitwise AND**; a fitted LUT is deferred
   (see Future/TODO) — AND is one instruction and roughly right for the
   triangle combinations, even though it's badly wrong for the pulse ones.
6. **6581 first, LUT-based**, so an eventual 8580 model is a table swap
   plus a saturation bypass rather than a second code path. The LUT's
   provenance is stamped into its generated header — any curve is *a*
   6581, since the real cutoff curve varied enormously between physical
   chips, never presented as canonical.
7. **Velocity is applied as digital scaling, before the bus sum**, not
   after the filter — a quiet note then drives the 6581 nonlinearity less
   hard, which is both physically right and what makes velocity feel like
   dynamics rather than a volume knob. Compiles out entirely under
   `CHIP_STRICT`.
8. **The glossary stays in this doc** rather than moving to
   `architecture.md` — the voice/channel/track terminology collision
   predates this module, but no other module doc carries a glossary
   either, so there was nowhere shared to merge it into.
9. **Primitives are topology-free from the first commit** — the strict
   host harness only proves anything if the same code runs in both
   topologies, and retrofitting this rule after the fact would be far more
   painful than following it from the start.
10. **Hard restart is an instantaneous envelope reset**, not the
    hardware's frame-delayed ADSR-bug workaround — see SID Oscillator,
    Waveforms, and Envelope above for why.
11. **>32 voices is out of scope**, not just deferred — a tag-bit MSB
    scheme for the active-voice FIFO word was considered as the answer if
    it were ever wanted, but won't be built: not reachable except by the
    cheapest PSG voices, and CPU is better spent on the speaker stage.
12. **CC16 (the BeatStep-Pro-safe encoder alternative to Program Change)
    was dropped**, migrating onto the Core 0 input pipeline (Router,
    `src/input_layer.h`) — the same call already made for `fm`'s CC30,
    which had the identical rationale. Program Change alone now selects
    the instrument, per the standing Program-Change-alone convention every
    module on the Router follows.
13. **Filter-bus binding lives inside `set_note()`**, resolved at the same
    point as voice allocation — it's a second, chip-specific
    allocator-like concern (a scarce per-channel resource, not per-voice),
    but its lifecycle already matched voice allocation's own timing
    (decided once per note-on, from whatever instrument is currently
    selected); moving it into the Handler alongside `voice_alloc_allocate()`
    didn't change when it runs, only where the code lives. Instrument
    selection itself (Configuration) never touches the filter bus directly
    — a channel's binding only changes on its *next* note-on, unchanged
    from the pre-migration behavior.

## Glossary

Terms are overloaded across this module's own design discussion and the
rest of the codebase. Definitions here are grounded in current code usage.

| Term | Meaning | Note |
|---|---|---|
| **Engine** | Build-time synthesis module, `src/engines/<name>/` | One per firmware image; already unambiguous |
| **Voice** | A Core 1 render slot: index `0..MAX_VOICES-1`, one bit in the active bitmap, one `VoiceParams` entry | Existing meaning in code — do not redefine |
| **Channel** | MIDI channel, and nothing else | Currently overloaded three ways elsewhere in the project |
| **Track** | A tracker column | `module_tracker.md` says "channel N is voice N"; "track" removes the collision |
| **Note** | One played event; occupies exactly one voice | True under the modulation-as-field design |
| **Preset** | The patch definition (`VoicePreset`, `presets.h`) | t00t-wide term |
| **Instrument** | Chip-module synonym for preset | Alias; do not rename existing code |
| **Chip** | A tonal profile selecting oscillator/envelope/LFSR/filter models | Not a container, not an allocation unit |
| **Filter bus** | A pooled, typed filter instance that voices route into | This module only |
| **Frame** | One tick of the chip control clock (default 50 Hz) | Distinct from "buffer" and from "sample" |
