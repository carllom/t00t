# T00T — OPL Module (OPL2/3-class)

A phase-modulation engine targeting Yamaha OPL2 (YM3812, AdLib/Sound
Blaster-class) feature parity, extended with OPL3/4-class 4-operator voices
and full 8-waveform select. It is a *mode* — a build-time engine variant
selected via `T00T_ENGINE=opl`. See `engine.md` for the shared dual-core
architecture; `architecture.md` for the cross-engine `VoiceParams`/CMake
pattern; `history_opl.md` for the development record.

This module shares its per-sample operator kernel with `src/engines/fm/`
(the DX7-class module) directly — `#include`s it rather than forking it —
while giving OPL its own envelope, patch data, and fixed routing tables,
none of which are extensions of FM's DX7-shaped equivalents. See
`module_fm.md` for the shared kernel's own documentation; this module doc
covers only what's specific to OPL.

## Overview

Two or four independently-enveloped operators per voice, depending on the
patch's chosen Algorithm — one of six fixed algorithms total (2 real OPL2
2-op algorithms, 4 real OPL3 4-op connections), a 2-op and a 4-op patch
freely coexisting side by side. Real hardware has no free routing or DAG
concept at either operator count, unlike the six-operator DX7 module. See
Decision Record entries 10-12 and wayfinder map [4-operator OPL4-class voices for the opl engine](https://github.com/carllom/t00t/issues/136)
for how the 4-op extension was designed.

### Specifications

- **Voices**: `MAX_VOICES = 9` — real OPL2 hardware's own channel count
- **Routing**: 6 fixed Algorithms total, each a `constexpr FmRouting` literal
  in `OPL_ROUTINGS[]` (patch.h), indexed directly by `OplAlgorithm` — no
  runtime DAG resolution, unlike the DX7 module's free 6-operator routing.
  2 are OPL2's original 2-op algorithms (FM chain: op0 modulates op1, which
  carries; additive: op0 and op1 both carry). 4 are real OPL3's own
  four-operator connections (full serial chain op0→op1→op2→op3; two
  independent 2-op FM pairs summed, (op0→op1)+(op2→op3); op0 additive plus a
  3-op chain, op0+(op1→op2→op3); op0 additive plus a 2-op pair plus op3
  additive, op0+(op1→op2)+op3) — matching real hardware exactly rather than
  opening the shared kernel's free 6-op DAG. See Decision Record entry 10.
- **Envelope**: `EnvOpl`, one 4-stage log-domain instance per operator (2
  per voice), OPL-native rate/level/KSL/TL fields, sharing `EnvDX`'s own
  gain-conversion domain so no new conversion code was needed. Every stage
  is a linear ramp in that log domain, at a rate law verified against
  Nuked-OPL3 (`tools/opl_ctl_diff.py`); KSL and TL are exact matches to real
  hardware's own tables — see Future/TODO for the one remaining curve-shape
  gap (attack)
- **Waveform select**: 8 waveforms per operator (OPL3/4's full set), a
  per-operator table pointer reused from FM's own per-operator table field.
  0-3 are OPL2's original shapes (sine, half-sine, full-wave-rectified, a
  quarter-cycle pulse). 4-7 are OPL3/4's extension (`sin(2*theta)`
  restricted to the cycle's first half, silent second half; its
  rectified/always-positive counterpart; a plain square wave; a
  per-half-cycle exponential/logarithmic sawtooth). See Decision Record
  entry 12 for the one shape (the sawtooth) where this module's "plausible
  approximation, not a log-ROM port" precedent needed an exception.
- **Vibrato**: one small fixed-rate sine LFO per voice, depth scaled by the
  mod wheel — not a reuse of FM's full per-patch-configurable LFO, since
  real OPL2 hardware has one global fixed-rate vibrato, not DX7's
- **Key/rate scaling, feedback, MIDI velocity**: all resolved once at
  note-on, zero per-sample cost
- **No pitch envelope, no per-operator amplitude/pitch LFO sensitivity** —
  real OPL2 hardware has neither
- **Multitimbrality**: a Multi of 16 Parts (`docs/platform.md`), Part n on
  MIDI channel n by default; Parts on one channel layer, and each has its own
  volume, pan and FX send. Each voice plays its own copy of its Part's
  cooked Patch, so every voice can play a different Patch
- **Patches**: on the patch platform — a Patch is an OPL3 voice in register
  units (42 Descriptors, below), edited over NRPN/SysEx and the Quick-edit
  Page, and stored to the module's user bank. One factory bank, generated
  from the hand-authored set in `patches.h` (no `.sbi`/`.op2` converter
  exists yet, see Future/TODO): the original 5 2-op patches (LEAD, BELL,
  BASS, ORGAN, PERC), plus 4 covering each 4-op Algorithm and exercising all
  8 waveforms between them (CHAIN EP, DUAL SAW, BRASS, ORGAN PAD)
- **Patch shape**: a single `OplPatch` struct serves both 2-op and 4-op
  voices — `OplOpParams op[4]` unconditionally (a 2-op patch leaves the
  upper two slots as unused padding, matching `patch.h`'s existing
  routing-literal padding convention), no separate operator-count field
  (derived from the chosen Algorithm's own `FmRouting.num_ops`, per Decision
  Record entry 9's existing mechanism), and a single `feedback` field
  applying to op0 only, 2-op or 4-op alike. See Decision Record entry 11.

### MIDI Mapping (Input Capabilities)

| Message | Function |
|---|---|
| Note On/Off | Every Part on the channel plays the note (layered), one voice per Part, dynamic allocation (`voice_alloc`) |
| Pitch Bend | Folded into `phase_inc` by Core 0 before it reaches Core 1 |
| CC1 | Mod wheel — scales vibrato depth |
| CC7 / CC10 / CC91 | VOLUME / PAN / FX SEND of the channel's Parts |
| CC72 / CC73 / CC74 / CC75 | The Multi's FX P1 / FX MIX / FX TYPE / FX P2 |
| NRPN | Any Patch parameter, number = parameter ID (below); MSB 126/127 the Part and Multi parameters (`docs/platform.md`) |
| CC0 | Bank Select MSB: 0 the factory bank, 64 the user bank |
| Program Change | Loads that slot of the selected bank into every Part on the channel; an empty slot or missing bank is ignored; sounding notes keep their Patch |
| SysEx | The t00t protocol (`docs/platform.md`): identify, parameter change, edit-buffer and Multi dumps, Store, descriptor dump, bank dump and load |

### Parameters

A Patch is an OPL3 voice in register units (`opl_module.h`, IDs
permanent). Each operator has a block of 10 IDs, OP1's at 0, OP2's at 10,
OP3's at 20, OP4's at 30: MULT, KSL, TL, AR, DR, SL, RR, EG TYPE, KSR, WAVE.
Then ALGORITHM (40) and FEEDBACK (41). OP1–OP4 are the engine's op0–op3; a
2-op Algorithm leaves OP3 and OP4 unused.

- Every parameter is **note-on**: the voice takes its operators' registers
  when a note starts, so an edit is heard from the next note.
- MULT shows the register's ratio (0.5, 1 … 15, with the hardware's repeated
  10, 12 and 15); KSL its slope (OFF, 3dB, 1.5dB, 6dB per octave, in register
  order); ALGORITHM its connection ("1>2", "1+2", "1>2>3>4", "1>2+3>4",
  "1+2>3>4", "1+2>3+4": `>` modulates, `+` sums).
- Defaults are an INIT VOICE: 1>2 with OP1 at TL 63, so OP2 alone plays a
  sustained sine.
- Quick-edit: OP1 MULT and TL, OP2 MULT and WAVE, ALGORITHM, FEEDBACK.

### Display (Presentation Capabilities)

Four Pages (`src/wslcd/page.h`/`header.h`), the shared Widget/Header/Page
library (CONTEXT.md's Widget catalog) applied to OPL:

- **Performance** (required, default): the Header's Resource bar (combined
  active-voice/CPU indicator) plus the current Patch (program number and
  name of the lowest Part on whichever channel most recently played a note
  or a Program Change, `*` while edited) and the Multi's FX (CC73/72/75) as
  compact Value bars ("FXMIX"/"FX P1"/"FX P2") with FX type (CC74) as a
  Label ("OFF"/"DELAY"/"REVERB"/"PHASER"/"FLANGER"/"CHORUS"/"CRUSH"/"DRIVE")
  and the mod wheel (CC1) as a fourth Value bar ("MOD").
- **EDIT**: the Quick-edit Page (`src/patch/quick_edit_page.h`) for that
  Part — its quick-edit parameters, Compare and Store/rename.
- **MULTI**: the Multi Page (`src/patch/multi_page.h`) — the Parts, and
  loading and storing Multis.
- **DIAG**: the exact-value detail Resource bar deliberately sacrifices —
  CPU% (PercentageBar), per-voice sounding activity (ActivityGrid), last
  note/velocity/channel, and a four-cell operator-role indicator (carrier,
  modulator or feedback per operator, from the Part's cooked routing; the
  cells past a 2-op Algorithm's operators show dark).

EDIT, MULTI and DIAG are reachable via the breadboard's rotary encoder
(`src/encoder_nav.h`, docs/engine.md's "Encoder navigation" entry) where
one's wired; Performance-only otherwise (`HAS_ENCODER=0`).

## Technical Overview

### Source Layout

```
src/engines/opl/
  engine.h            VoiceParams, VoiceParamBlock, ParamExchange
  audio_engine.cpp    audio_engine_run(): Core 1's buffer loop around opl_render.h
  opl_scale.h         OPL_TABLE_BITS -- no separate gain-domain anchor
                       (see Decision Record)
  waveforms.h         the eight OPL3/4 waveform tables
  patch.h             OplOpParams/OplPatch (engine form) + the six fixed
                       algorithm routings (OPL_ROUTINGS[]), expressed as
                       FmRouting literals, and opl_resolve_routing()
  env_opl.h           EnvOpl, OPL's own envelope
  opl_voice.h         note-on/step/note-off/active/render voice glue,
                       calling ../fm/op.h's kernels directly
  opl_module.h        the module on the patch platform: parameter IDs,
                       descriptors, the cooked patch and the cook
  opl_factory.h       the factory banks
  factory_bank.h      GENERATED by tools/host_render/gen_opl_bank from
                       patches.h — do not hand-edit; checked in
  opl_render.h        the per-buffer render: voices from their cooked
                       Patches, the Multi's mix and FX (Core 1 and host)
  opl_platform.h      the module's platform and Multi, for the display
  patches.h           the hand-authored patches, checked in: the factory
                       bank's source and the host renderers' patch table
  input_subsystem.cpp notes by Part, bend, mod wheel, the Multi's FX,
                       NRPN, Bank Select/Program Change, SysEx
  display.cpp         Performance, EDIT, MULTI and DIAG Pages (see Display above)
```

There is no `rig.h`/measurement rig and no free-routing DAG resolver —
real hardware has only six fixed algorithms total (2 real OPL2 2-op, 4 real
OPL3 4-op), so there's nothing to resolve at note-on beyond an array lookup.
No hardware pass has measured 4-op per-voice cost yet — see Performance
below, which still reflects 2-op-only measurements.

### Build

Build with `make ENGINE=opl`.

### Tools

`tools/host_render/gen_opl_bank.cpp` — writes `factory_bank.h` from
`patches.h` as stored Patches, checking that cooking each one gives back
its registers and routing; `--check <file>` fails if the checked-in header
is stale. Rerun it after editing `patches.h`:
`gen_opl_bank src/engines/opl/factory_bank.h`.

`tools/host_render/test_opl_platform` (platform seam: Program Change, NRPN
and Part CCs into a Multi of OPL Parts, cooked patches out; the factory bank
against `patches.h`; a Store loaded back from the user bank) and
`test_opl_render` (Core 1's render against `opl_voice.h` on the same patch,
sample for sample for every factory patch; edits waiting for the next note,
Patch loads under a held note, a note waiting for its cook, the Part mix)
build with `make host`.

`tools/host_render/render_opl.cpp` — renders every patch in `patches.h`
through the exact device code path to a WAV file and confirms note-off
actually releases the voice within a bounded tail; the practical sanity
check available without hardware.

`tools/host_render/test_opl_4op.cpp` — same check, one synthetic patch per
4-op Algorithm (`OPL_ALGO_4OP_*`), independent of whatever `patches.h`
currently ships.

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

Every voice's `FmOp` array is still the full six-wide `FM_NUM_OPS` array,
but `fm_voice_render_block()` (`../fm/op.h`) only loops `order[0..num_ops-1]`
— a field on `FmRouting` each routing sets for itself, FM's own
`fm_resolve_routing()` always to `FM_NUM_OPS` (every DX7 algorithm is
structurally six operators wide), OPL's own `OPL_ROUTINGS[]` literals
(`patch.h`) to 2 for the original OPL2 algorithms or 4 for the OPL3 4-op
connections. Slots past `num_ops` in every voice's array are simply never
visited by the per-sample kernel, not computed at zero gain and discarded —
see Decision Record entries 9 and 10.

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
note-off/active/render functions, but for `OplPatch`/`EnvOpl` and a variable
2 or 4 real operators — every function loops `i < routing.num_ops` rather
than a hardcoded bound, so the same glue drives both operator counts. A
patch's algorithm selects one of `patch.h`'s `OPL_ROUTINGS[]` literals; since
a patch's feedback amount isn't compile-time, `opl_resolve_routing()` copies
the literal and patches it (`kernel[0]`/`fb_shift[0]`/`clear_bus_mask`)
rather than baking it into the literal — always operator 0, 2-op or 4-op
alike (real hardware's second Operator pair has its own feedback register
too, but it's architecturally unwired on every 4-op connection, so this
module doesn't model it). On the device that happens in the cook;
`opl_voice_start()` then starts a voice on a resolved routing.

Vibrato is one small fixed-rate sine LFO per voice (not a reuse of the DX7
module's full per-patch-configurable one — real OPL2 hardware has a single
global fixed-rate vibrato, not a per-patch one), scaled by the mod wheel and
folded into every real operator's phase increment (`routing.num_ops` of
them) once per control block.

### Patches on Core 0, Notes on Core 1

Core 0 cooks a Part's Edit buffer (`OplModule::cook()`, `opl_module.h`):
register values become the engine's `OplPatch`, and `opl_resolve_routing()`
resolves its Algorithm and feedback into an `FmRouting`. The cook is
published to the Part's slot (`docs/platform.md`).

A note carries its Part, the Patch load and the Part's latest cook
sequence number — no patch data, so `ParamExchange` stays small. Core 1
(`opl_render.h`) copies the Part's cooked Patch into the voice when the
note starts (`PartReceiverT::note_patch()`): the current cook if it is the
note's cook or a later one of the same Patch, the previous one if the note
belongs to the Patch that cook replaced, and if the note's cook hasn't
arrived yet the note waits a buffer. With every parameter note-on, a
sounding note plays its copy unchanged. Voices mix through the Multi's
output stage (`src/patch/multi_out.h`): each by its Part's volume, pan and
FX send, then the shared FX and the master level.

## Status and Plan

### Performance

First hardware pass (#82, GPIO-22 `PROFILE_PIN`, `breadboard_rp2350`)
measured ~114 c/f/voice on one patch (OPL BELL) — most of the gap against
the ~34 c/f/voice scoping estimate traced to the four unused operator slots
per voice running their full per-sample kernel for nothing, fixed by
`FmRouting::num_ops` (Decision Record entry 9, confirmed a no-op for audio
output via bit-identical WAV render before/after). A second hardware pass
against the fix confirmed the drop: **~67 c/f/voice, a 41% cut**, with
delay/reverb's own fixed overhead unchanged (confirming the fix touched
only per-voice cost). Full numbers and the FM-baseline comparison:
`history_opl.md`, "Hardware Voice-Count Sweep, Post-`num_ops` Fix".

A second patch (OPL ORGAN — no feedback, additive, the opposite end of
BELL's feedback-heavy FM chain) measured **~54 c/f/voice**, about 20%
cheaper — attributable to feedback's own extra per-sample arithmetic
(`op_render_fb`, `../fm/op.h`), the one thing in the per-sample kernel that
actually varies cost between patches on this 2-operator, fixed-two-algorithm
module (algorithm choice and waveform select don't touch it). **Real
per-patch cost lands in a ~54–67 c/f/voice range** — two patches bracketing
that one axis is reasonable coverage here, unlike the DX7 module's free
6-operator routing space, where per-patch cost genuinely spans a wide range
and needs broader sampling. At 9 voices + reverb, even the more expensive
patch (BELL) is only about 26.5% duty. Full numbers: `history_opl.md`, "Hardware
Voice-Count Sweep, Post-`num_ops` Fix" and "Second Patch: OPL ORGAN".

### Future / TODO

- **Non-linear attack curve** — real OPL2 attack is a curved
  (fast-then-slower) shape; this module's attack is a linear ramp
  calibrated to the same real total duration (`tools/opl_ctl_diff.py`'s
  `eg/attack-*` cases, at a deliberately wide tolerance), not the shape
  itself. Closing this would need the ramp itself to become non-linear, not
  just another round of rate-table tuning.
- **Patch bank import** — hand-authored patches only for now; no
  `.sbi`/`.bnk`/`.op2`/GENMIDI-class converter exists yet. On the patch
  platform it belongs in host tools and the editor, writing stored Patches.
- **Live parameters** — every parameter is note-on; TL or WAVE could be
  made live by having Core 1 re-read them from the Part's cook each block.
- **4-op hardware performance pass** — per-voice cost for a 4-op voice
  hasn't been measured on real hardware yet; Performance above still
  reflects 2-op-only numbers.
- **OPL1 (sine-only subset)** — not started. The per-operator waveform table
  pointer and OPL's own (not DX7-derived) patch struct were chosen so this
  wasn't foreclosed by the 4-op work above.
- **Rhythm/percussion mode** — deferred, not dropped: it needs a
  genuinely different kernel shape than the phase-accumulator-plus-table
  approach every voice here uses, so it wasn't needed to validate the
  kernel-reuse approach this module is built on, but a longer-term
  AdLib-tracker-format-playback goal depends on it.
- **Per-voice display grid** — the DX7 module's multitimbral grid (which
  voice is playing which patch on which channel) has no OPL equivalent
  yet; this module's panel shows the Patch of the most-recently-touched
  channel's lowest Part.

## Decision Record

1. **The per-sample kernel is included from the DX7 module directly, not
   forked.** `FmOp`, the three render kernels, `fm_voice_render_block()`,
   and the gain-conversion math are the expensive-to-get-right part of
   either module — reusing them verbatim means a fix or a future
   optimization to that shared kernel benefits both engines at once,
   rather than needing to be ported by hand into a second copy.
2. **Every voice's operator array stays six-wide, with four unused slots**,
   rather than a hand-rolled two-operator loop, so the reused
   `fm_voice_render_block()` (and its routing-literal input shape) could be
   used completely unchanged. `FmRouting::num_ops` (entry 9) means those
   four slots cost nothing per sample; the remaining tradeoff is SRAM only
   (`sizeof(FmOp)` × 4 × `MAX_VOICES`, unmeasured but small next to the
   module's other fixed working set).
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
9. **`FmRouting` gained a `num_ops` field bounding `fm_voice_render_block()`'s
   loops**, instead of forking a two-operator-only render path. A hardware
   pass (#82) measured OPL's per-voice cost at ~114 c/f/voice against a
   scoping estimate of ~34 — most of the gap traced to the four unused
   operator slots still running the full per-sample kernel (phase, table
   lookup, gain multiply) every sample, just to write a bus nothing reads.
   `num_ops` is data on the routing (FM's own `fm_resolve_routing()` sets it
   to `FM_NUM_OPS` unconditionally, unaffected), not a template parameter or
   a second render function, so it costs one field and one loop-bound change
   in shared code the DX7 module also uses — and generalizes to a future
   4-operator OPL3 routing (Future/TODO) by setting `num_ops = 4`, with no
   further kernel changes needed.
10. **The 4-op Algorithm set (wayfinder map [4-operator OPL4-class voices for the opl engine](https://github.com/carllom/t00t/issues/136))
    matches real OPL3's 4 four-operator connections exactly, not the shared
    kernel's free 6-op DAG** (which the DX7 module already demonstrates the
    kernel supports). The goal is "any voice type a real OPL4 chip's FM
    section can make," not a superset — matching real connections is both
    sufficient for that and unambiguous to design against, sourced directly
    from Nuked-OPL3 (`docs/research/opl3-4op-algorithms.md`, ticket
    [Real OPL3 four-operator connection topologies](https://github.com/carllom/t00t/issues/138)).
    Each connection becomes one more `FmRouting` literal in the identical
    idiom `OPL_ROUTING_FM`/`OPL_ROUTING_ADD` already established —
    extending entry 9's `num_ops` mechanism to `num_ops = 4` with no further
    kernel changes, exactly as entry 9 anticipated.
11. **`OplPatch` gains no `num_ops` field of its own, and a single
    `feedback` field rather than one per Operator pair** (ticket
    [OplPatch shape for 2-op vs 4-op voices](https://github.com/carllom/t00t/issues/137)).
    Operator count is read off the chosen Algorithm's own resolved
    `FmRouting.num_ops` (entry 9's mechanism again) rather than duplicated
    as patch state — one source of truth, matching this module's existing
    preference (entry 4). Feedback stays a single field, not `feedback[2]`,
    because ticket #138's research found real OPL3 hardware's second
    Operator pair (op2) has its own feedback register but it is never read
    by any of the 4 real four-operator connections — architecturally a
    no-op in every case. A `feedback[2]` field would let a patch author set
    a value that silently does nothing; a single op0-only field matches
    what actually affects sound and needs no new concept beyond what a 2-op
    patch already has.
12. **The 4 new waveforms (register indices 4-7,
    `docs/research/opl3-8-waveforms.md`, ticket
    [OPL3/4's full 8-waveform set](https://github.com/carllom/t00t/issues/139))
    extend `waveforms.h`'s existing "plausible approximation, not a
    log-ROM port" precedent for 3 of the 4 new shapes, but not the 4th.**
    For WS4-6, reading Nuked-OPL3's `OPL3_EnvelopeCalcSin4`-`Sin6` directly
    confirmed the chip's log-sine ROM is purely an implementation trick for
    computing an ordinary sine or square curve cheaply in fixed point — the
    same float-`sinf`-based approximation this module's existing 4 tables
    already use carries over unchanged. WS7 (the logarithmic sawtooth) is
    the exception: there the log-domain-to-linear exponential conversion
    *is* the waveform's actual, audible geometry (a per-half-cycle ~96 dB
    exponential decay with a hard discontinuity at wraparound), not a
    shortcut for something simpler — a naive linear ramp would sound and
    look qualitatively wrong, so its table must use the real exponential
    formula (`amplitude ∝ 2^(-32*p)` per half-cycle) rather than this
    module's usual "geometry over exact ROM values" approximation license.

13. **Patch parameters are OPL3 registers, not musical units.** A Patch's
    values are the chip's own register fields (MULT code, TL in 0.75 dB
    steps, 4-bit rates), so a voice from a real OPL bank file maps onto a
    Patch field for field and an editor shows what the hardware has; the
    labels carry the musical meaning (MULT's ratio, KSL's slope).
14. **Every parameter is note-on.** The voice glue takes all of a patch's
    registers when a note starts; making some live would put a per-block
    re-read on Core 1 for values a player rarely moves mid-note.
15. **Core 1 copies a note's cooked Patch at note-on**, as FM does, rather
    than Core 0 writing it into `VoiceParams`: a cooked OPL Patch is about
    80 bytes, which the generic controller would copy for every voice on
    every MIDI batch.
16. **The factory bank is generated from `patches.h` and checked in.** The
    patches are the project's own, so unlike FM's DX7 banks they can ship;
    `patches.h` stays the single source, shared with the host renderers, and
    the generator's lossless check plus `--check` keep the bank in step.

## Glossary

- **Operator**: one waveform generator with its own envelope and frequency
  multiplier — two per voice on OPL2-class 2-op voices, four per voice on
  OPL3/4-class 4-op voices (see wayfinder map "4-operator OPL4-class voices"),
  versus the DX7 module's six.
- **Operator pair**: two Operators, the building block a 4-op voice's
  Algorithm wires together — the unit real OPL3 hardware pairs two 2-op
  channels from. A 2-op voice is a single Operator pair; a 4-op voice is
  two. Real hardware gives each Operator pair its own Feedback register,
  but see Feedback below: only the first pair's is ever wired to anything.
  _Avoid_: "channel" — already claims a different meaning at the MIDI/voice
  layer in this codebase.
- **Algorithm**: one of a voice's fixed routing topologies. A 2-op voice has
  exactly two — an FM chain (one operator modulates the other, which
  carries) and additive (both operators carry independently). A 4-op voice
  has exactly four — the real OPL3 four-operator connections, each wiring
  two Operator pairs together — versus the DX7 module's 32-plus-free-DAG
  routing.
  _Changed from_: "OPL2 has exactly two" — broadened once 4-op voices
  (wayfinder map "4-operator OPL4-class voices") added a second, larger
  fixed topology set alongside OPL2's original two, matching how both the
  DX7 module and real OPL3 documentation already use "algorithm" for a
  chip's fixed topology set regardless of count.
- **Carrier**: an operator whose output is audible directly.
- **Modulator**: an operator whose output phase-modulates the other
  operator instead of being audible directly.
- **Feedback**: operator 0's output modulating its own phase — one
  `feedback` value per voice, 2-op or 4-op alike. Real OPL3 hardware gives
  the second Operator pair (op2) its own feedback register too, but no real
  four-operator connection ever reads it (verified exhaustively against
  Nuked-OPL3's connection logic — [Real OPL3 four-operator connection topologies](https://github.com/carllom/t00t/issues/138)):
  it's programmable but architecturally a no-op on every one of the 4
  connections, so this module models only op0's, matching what actually
  affects sound. Unlike the DX7 module's per-operator feedback field.
- **EG-type**: whether an operator's envelope holds at its sustain level
  until note-off (sustain) or decays straight through it to silence on its
  own (percussive) — an OPL-native flag with no DX7 equivalent.
- **KSL (key scale level)**: extra output attenuation for higher notes,
  4 settings including off — OPL's analogue of the DX7 module's key level
  scaling curve, in different units.
- **TL (total level)**: an operator's programmed output level, 0-63 OPL
  units in 0.75 dB steps — the DX7 module's equivalent field uses 0-99 DX7
  units on a different (though also log-domain) scale.
