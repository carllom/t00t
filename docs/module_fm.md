# T00T — FM Module (DX7-class)

A dedicated 6-operator phase-modulation engine targeting DX7 feature parity
and DX7-class polyphony. It is a *mode* — a build-time engine variant
selected via `T00T_ENGINE=fm`. See `engine.md` for the shared dual-core
architecture; `architecture.md` for the cross-engine `VoiceParams`/CMake
pattern; `history_fm.md` for the full development record, including a
later Dexed-conformance evaluation (phases F0–F8) that found and fixed
several real defects in the engine's first build — an uncalibrated
modulation-index scale, a hand-derived envelope curve family instead of
Dexed's real exponential-attack shape, feedback depth 2× too hot, an LFO
phase-origin bug that inverted three of six waveforms, and a converter bug
silently dropping two-thirds of the modulation on 10% of factory patches.
Bank-wide error against Dexed dropped from a mean 18.9 dB harmonic MAE at
the first baseline to 0.53 dB after that work, across all 256 factory
voices on four ROM banks.

A separate, much smaller proposal — adding a 2-operator FM oscillator to
the generic subtractive engine — is independent of everything below; see
Future/TODO.

## Overview

Six independently-enveloped operators per voice, freely routed (any DX7
algorithm, or anything else expressible as a DAG plus self-feedback),
verified against [Dexed](https://asb2m10.github.io/dexed/) (the reference
DX7 emulator) as ground truth rather than by ear alone.

### Specifications

- **Voices**: `MAX_VOICES = 16`, provisional — see Performance below for
  why this is open again rather than settled
- **Routing**: free (not fixed to the 32 DX7 algorithms), resolved once at
  note-on into per-operator bus pointers and a kernel selection; the inner
  loop is identical either way. Algorithms 4 and 6 (the only two DX7
  algorithms with a second, operator-spanning feedback loop) collapse to
  single self-feedback — this matches Dexed exactly, since Dexed's own
  algorithm table never implements that second loop either
- **Envelope**: `EnvDX`, one 4-stage log-domain instance per operator (6
  per voice), a direct port of Dexed's real `Env`, stepped once per
  16-sample control block
- **Pitch EG**: one 4-stage instance per voice, operating in cents, a
  direct port of Dexed's `PitchEnv`
- **LFO**: one per voice (not global — see Decision Record), all six DX7
  waveforms, key sync, PMD/AMD, per-operator AM sensitivity
- **Key/rate scaling, velocity sensitivity, detune, fixed-frequency mode**:
  all resolved once at note-on, zero per-sample cost
- **Multitimbrality**: per-voice, for free — `VoiceParams` carries one
  patch pointer, so different voices can already be playing different
  patches simultaneously
- **Sine table**: 4096-entry, no interpolation, 12-bit phase resolution
  (matching real DX7 hardware) — see Decision Record for why
  interpolation isn't used
- **Patches**: no built-in bank ships in the repository — real DX7 `.syx`
  banks are Yamaha's commercial data (gitignored, generated locally); the
  engine plays a single hardcoded test patch until one is converted.
  `FM_PATCHES` itself lives in RAM, not flash — see Decision Record
- **No PSRAM, no streaming, no dynamic allocation** — ~12.5 KB of SRAM fixed
  working set, plus up to ~26.5 KB for the patch table itself (212
  bytes/patch, up to 128 patches)

### MIDI Mapping (Input Capabilities)

| Message | Function |
|---|---|
| Note On/Off | Standard dynamic allocation (`voice_alloc`), one voice slot per note |
| Pitch Bend | Folded into `phase_inc` by Core 0 before it reaches Core 1 |
| CC1 | Mod wheel — a separate modulation source competing with the patch's own configured LFO depth (`max()`, not a multiplier — see Decision Record) |
| CC10 | Pan |
| CC72 | FX param 1 |
| CC73 | FX wet/dry mix |
| CC74 | FX type select |
| CC75 | FX param 2 |
| Program Change | Configuration: patch select — `FM_PATCHES[value % FM_PATCH_COUNT]`, always at least one entry |

### Display (Presentation Capabilities)

Same chrome every engine's panel shares (title bar, VOICES dot bar, CPU load
bar, NOTE row), plus FM-specific rows: the current patch (bank index and DX7
voice name, for whichever channel most recently triggered a note or a
Program Change), its algorithm as six operator-role cells
(carrier/modulator/feedback, derived from `FmOpParams` directly — no
algorithm number is stored anywhere at runtime), and a compact per-voice
grid (voice, channel, patch index) covering voices 0–7, which is what makes
multitimbral use visible instead of assumed.

## Technical Overview

### Source Layout

```
src/engines/fm/
  engine.h            VoiceParams, VoiceParamBlock, ParamExchange
  audio_engine.cpp    audio_engine_run(): render pass, voice loop, FX insert
                       (also holds the P0 profiling rig behind T00T_FM_PROFILE)
  fm_scale.h          the fixed-point contract (FM_CYCLE/FM_GAIN_MAX/...) —
                       the single anchor op.h/env_dx.h/lfo.h all derive
                       their scale from
  op.h                FmOp + the three render kernels, note-on/block/voice glue
  env_dx.h            the DX7 envelope, a direct port of Dexed's Env
  pitch_eg.h          voice-wide pitch EG
  lfo.h               voice-wide LFO
  sine_tab.h          the operator sine table
  patch.h             FmPatch/FmOpParams (runtime form) + the note-on
                       routing compiler (fm_resolve_routing())
  patches.h           GENERATED by tools/syx2patch.py — do not hand-edit;
                       gitignored, absent until generated locally
  rig.h               standalone P0 measurement rig (no patch/EG/LFO)
  render.h            fm_render_test_tone(), shared by the device skeleton
                       and the host build
  input_subsystem.cpp note on/off, bend, pan, mod wheel, patch select
  display.cpp         status panel: voices/CPU/note, current patch,
                       algorithm operator-role cells, per-voice multitimbral
                       grid (see Display above)
```

There is no `presets.h`/`VoicePreset` — FM's whole timbre is the single
`FmPatch` pointer in `VoiceParams`, so it never needed the shared
preset-table shape speech/chip use. `input_subsystem.cpp`'s own top-level
`midi_controller_process()` is a one-line call into
`midi_controller_process_generic()` (`src/midi/midi_controller_generic.h`,
built from `src/midi/midi_dispatch.h`'s shared, module-agnostic generic
per-MIDI-message-type dispatch helpers — the same ones `subtractive`
composes) — only the mapping table, Handlers, and per-voice/per-channel
state stay this module's own.

### Build

Build with `make ENGINE=fm`. `FM_PROFILE=1` builds the standalone P0
measurement rig (`rig.h`: N voices × 6 operators, fixed increments/gains,
no EG, no LFO, no patch logic) instead of the real engine. `FM_BLOCK`
overrides the control-block size (confirmed at 16, see Decision Record).
`FM_RIG_*` flags (voice count, table size, interleaving, flash placement,
DSP-extension use) each require a separate build, same reasoning as every
other engine's measurement-rig levers.

### Tools

`tools/syx2patch.py` — converts one to four DX7 32-voice bulk `.syx` dumps
into a single `patches.h`, banks concatenated in the given file order:

```
syx2patch.py convert <in.syx> [in2.syx ...] <out.h>   # writes patches.h
syx2patch.py dump <in.syx> [in2.syx ...]               # per-voice summary, writes nothing
```

Capped at 4 files (128 patches): `FM_PATCH_COUNT` indexes Program Change
directly, a 7-bit MIDI value, so a larger table would leave some patches
permanently unreachable.

Both the `.syx` input and the generated header are gitignored — real DX7
patch data is Yamaha's commercial work, the same policy the tracker's
`xm/` corpus already established for copyrighted third-party content.

`tools/fm_ref/` — builds [Dexed](https://asb2m10.github.io/dexed/) itself
(fetched at a pinned SHA, never vendored) into a `dexed_render` CLI, the
ground-truth reference for this module, the same role `openmpt123` plays
for the tracker. `tools/fm_compare.py` scores harmonic tracking/spectral
centroid/envelope shape against it. `tools/fm_ctl_diff.py` diffs the
control-rate math (EG, LFO, key/rate scaling, routing) exactly, row for
row. `tools/fm_freq_diff.py` covers detune and fixed-frequency mode by
rendering both engines and comparing spectral peaks in cents (Dexed's
frequency computation is a private method with no table to diff directly).
`tools/fm_regress.py` + `tools/fm_thresholds.json` run all four ROM banks
× 5 note/velocity configurations (1,600 patch renders) on every change,
gating on both the bank mean and the worst single patch. `render_fm`'s own
patch-bank render (one 3-second note per patch) is the quick host-side
sanity pass; the full Dexed diff is `fm_regress.py`.

## Architecture

### Free Routing

There is no performance argument for fixing the algorithms: routing is
note-on data (an operator's bus pointers, processing order, and kernel
selection), not inner-loop work, so a hardcoded algorithm and a free 6×6
routing matrix compile to identical inner loops. Free routing also gives
per-voice multitimbrality for free, which a fixed-algorithm design would
not. The one real constraint block-inner rendering imposes is that the
operator graph must be a DAG plus self-loops — a cycle spanning two or
more operators can't be evaluated in block order without a comb-filtering
block-length delay. This covers 30 of the 32 DX7 algorithms outright;
algorithms 4 and 6 (the only two with a second, operator-spanning feedback
loop) collapse to single self-feedback at conversion time — matching
Dexed exactly, not approximating it, since Dexed's own algorithm table
never implements that second loop either.

Six modulation buses plus one output bus (448 bytes total, `int32` ×
`BLOCK`) are shared scratch for the whole engine, not per-voice, since
voices render sequentially within a pass.

### The Fixed-Point Contract (`fm_scale.h`)

One anchor, from which every other scale factor derives: a unity-gain
operator produces exactly one full cycle of phase deviation, and a
max-level operator produces exactly two — Dexed's own real ceiling,
measured rather than chosen. `fm_mul_gain()`'s output *is* phase deviation
whether the operator is a carrier or a modulator; a carrier's output is
simply read as audio (one shift, applied once per voice) instead of as
phase. `FM_CYCLE` leaves headroom above a single operator's maximum
specifically so summing several modulators targeting the same bus is safe
without attenuating anything. Nothing outside this file is allowed an
opinion about absolute level — if a patch is too loud or too dim, the fix
is in its DX7 output level, EG, or key scaling, exactly as it would be on
real hardware.

### The Operator Kernel

```cpp
struct FmOp {
    uint32_t phase;
    uint32_t inc;           // this block's actual increment (base_inc × bend/EG/LFO ratio)
    uint32_t base_inc;      // neutral-pitch increment, resolved once at note-on
    int32_t  gain;          // linear, current
    int32_t  gain_step;     // per-sample delta for this block
    const int32_t *in;      // modulation bus (points at a zero bus for pure carriers)
    int32_t *out;            // output bus (or the shared voice mix bus)
    int32_t  fb1, fb2;       // op_render_fb only: last two post-gain outputs
    EnvDX    eg;              // this operator's own 4-stage envelope
};
```

Three kernel variants, selected per operator at note-on: plain, self-feedback
(a 2-sample average, real DX7 depth range across levels 1–7 via
`fb_shift = 8 - feedback_level`, matching Dexed's own `compute_fb` shift
exactly), and a first-writer variant that stores rather than accumulates —
eliminating the need to clear a bus before its first writer touches it.
The `in`/`out` pointers, the kernel selection, and the processing order
are the entire routing implementation; nothing else changes between
algorithms.

A modulator can drive more than one carrier at once (DX7 algorithms
19–25): a bus is not treated as emptied by its first reader, so every
operator that reads a given bus number receives the same modulation.

### `EnvDX` — the DX7 Envelope

Not a reuse of the shared ADSR (`envelope.*`) — the DX7 EG is 4 ×
(rate, level) pairs operating linear-in-dB, a direct port of Dexed's own
`Env` (`Source/msfa/env.cc`, Apache-2.0) in its log-domain (Q24-octave)
convention, not a re-derivation of its shape.

- Steps once per control block (`FM_BLOCK` = 16, confirmed by measurement
  — see Decision Record), not per sample. At each block boundary the log
  level converts to linear via a small exp2 table, and the kernel is
  handed a start gain and a per-sample delta — one add per sample in the
  hot path.
- Rising and falling stages are different curve families, matching real
  hardware: a rising stage jumps to a fixed floor then approaches its
  target exponentially; a falling stage ramps linearly in the same log
  domain. A single symmetric ramp cannot produce a genuinely instant
  attack, since it has to traverse the whole log-domain floor first.
- Output level is folded into the envelope's own target when each stage's
  level is resolved, not added afterward — the two compose non-linearly,
  so adding them separately understates every sustain stage.
- Both the level curve (`dx7_scaleoutlevel()`) and the rate curve
  (the piecewise `qrate`-based derivation in `env_dx_advance()`) are
  ported verbatim from Dexed's own tables, verified byte-identical by a
  committed control-plane conformance suite — not hand-fit shape guesses.
- A same-level stage still takes real time on hardware rather than
  completing instantly (matching Dexed's own dwell-table behavior), and
  the slowest rates are not artificially floored.

### Pitch EG and LFO

`pitch_eg.h`: one 4-stage instance per voice (not per operator), operating
on pitch in cents, applied by scaling all six operator increments at each
block boundary — zero per-sample cost. Rate and level tables ported
verbatim from Dexed's `PitchEnv`. Starts from the release level (L4), not
silence, on trigger — a zero-initialized config is a real pitch drop, not
"off"; every hand-written patch literal sets this explicitly, and the
converter never hits the pitfall since it always copies real patch bytes.

`lfo.h`: one per voice (not global — see Decision Record), all six DX7
waveforms, key sync, PMD/AMD, per-operator AM sensitivity, evaluated once
per control block — zero per-sample cost. Waveform math is re-expressed as
a plain float function of this file's own phase convention rather than
replicating Dexed's internal bit-for-bit table format, since control-rate
cost makes float math free either way; the rate table itself is ported
verbatim (real hardware-calibrated data). Mod wheel is a separate
modulation source that competes with the patch's own configured PMD/AMD
via `max()`, matching Dexed's real rule — a patch's configured
vibrato/tremolo always plays at its own depth; moving the wheel increases
it further from there.

### Note-On-Time Computation

Resolved once per note-on, never touched again in the render loop — this
is where the DX7's nonlinearity lives:

- **Key level scaling**: composed with output level and velocity before
  the log2 conversion, clamped to the DX7's own valid range — matching
  Dexed's own composition order, not added as a separate unclamped offset.
- **Rate scaling**: added to the base rate's own control value at every
  stage transition, matching Dexed's order of operations exactly.
- **Detune**: applied via the phase-increment path. Ratio-mode detune is
  genuinely note-dependent on real hardware (a fixed cents-per-unit
  approximation measured up to ~12 cents of error at the low end); the
  real note-dependent multiplier is resolved once per note-on from the
  actual MIDI note. Fixed-frequency-mode detune is a flat constant, since
  that one genuinely is note-independent on real hardware.
- **Fixed-frequency mode**: a closed-form Hz formula derived from Dexed's
  own frequency computation, applied via the same phase-increment path.

`VoiceParams` carries a raw MIDI `note` field specifically because key
level/rate scaling and note-dependent detune all need the actual played
note, which a bend-scaled `phase_inc` alone can't be inverted back into.

### Per-Voice Multitimbrality

`VoiceParams` carries one `const FmPatch *` — the whole timbre, one
pointer. It costs nothing per sample, needs no `ParamExchange` mechanism
change, and requires no per-sample dispatch, since every voice runs the
same kernel regardless of which patch it points at; only the routing
tables and coefficients differ. Core 0 assigns a patch per MIDI channel
and writes the pointer at note-on — 16-part multitimbral falls out for
free.

## Status and Plan

### Performance

**Re-confirmed, `MAX_VOICES = 16` kept.** Clean idle baseline: 24.8 c/f
(the earlier 71.4 c/f figure was a stale reading with the power-on-default
delay silently running, not a real idle cost — see Decision Record).
Typical per-voice cost: ~182 c/f/voice, confirmed independently twice.
Reverb + delay costs measured directly against the real engine for the
first time and land close to the previously-reused subtractive-engine
figures. Measured directly, not projected: 16 voices + reverb on a typical
patch runs high-80s to 91% duty. Patch data (`FM_PATCHES`) lives in SRAM,
not flash — see Decision Record #18.

Coverage caveat: real per-patch cycle numbers exist for only 6 of 256
factory patches, spanning 176.9–234.7 c/f/voice. The full 256-patch
by-ear pass (all banks, done) confirmed audible correctness bank-wide, but
isn't a cycle-accurate cost for each one. Full numbers: `history_fm.md`,
"F8 — hardware voice-count sweep" and "Multi-Bank syx2patch, Full ROM
By-Ear Pass, and the ORCH-CHIME RAM Fix".

### Future / TODO

- **Patch bank source** — currently a curated local conversion only; open
  question whether `.syx` loading should become a runtime feature over
  MIDI SysEx instead.
- **Free-routing UI / patch model beyond the 32 DX7 algorithms** — not
  started; the engine can already render arbitrary DAG+feedback routings,
  but nothing exposes that beyond what a converted `.syx` patch specifies.
- **X2 — operator waveform variants** (saw/square/half-sine, DX11/TX81Z
  style) — one extra table, zero extra inner-loop cost; not built, low
  priority pending headroom.
- **Core0/Core1 XIP cache contention** — the "keep the kernel in flash"
  measurement (see Decision Record) was taken with Core 0 doing
  essentially no flash-side work. RP2350's XIP cache is shared by both
  cores, so this margin isn't guaranteed once Core 0 has real LCD/MIDI
  traffic to contend with. Untested — there's no real Core 0 workload yet
  to bench against. Mitigation identified if it turns out to matter:
  `xip_cache_pin_range()` (RP2350-only) reserves the kernel's flash range
  against eviction.
- **FM oscillator in the subtractive engine** (`WAVE_FM2`) — a separate,
  much smaller, independent proposal: one extra phase accumulator plus a
  decaying modulation-index envelope per voice, added to the shared
  oscillator set rather than as part of this module. Estimated cost is
  roughly +20% per subtractive voice. Gets DX-flavoured tones through the
  existing ladder filter and effects chain; does not get real DX7
  character (six independently-enveloped operators vs. two operators
  sharing one index envelope — closer to a Casio CZ). Not built; shares no
  code with this module beyond possibly the sine table.

## Decision Record

1. **Free routing, not fixed algorithms** — no performance argument favors
   fixing them (routing is note-on data either way), and free routing
   gives per-voice multitimbrality for free.
2. **Algorithms 4 and 6 collapse to single self-feedback** rather than
   building an interleaved fallback kernel for the rare second loop — this
   turned out to match Dexed exactly (Dexed's own algorithm table never
   implements that second loop either), not to be an approximation.
3. **One fixed-point anchor** (a unity-gain operator = one cycle of phase
   deviation, max-level = two, both measured from Dexed's real ceiling)
   replaces what was originally several independently hand-tuned scale
   constants that existed only to cancel each other out.
4. **Self-feedback depth is the DX7's real 0–7 range**, not a bool —
   the fixed point contract makes the correct shift derivable
   (`8 - feedback_level`) rather than needing a second guessed constant.
5. **A bus is not emptied by its first reader** — one modulator can
   legitimately drive more than one carrier (DX7 algorithms 19–25); an
   earlier design that treated a bus as consumed on first read silently
   dropped modulation on a meaningful fraction of real factory patches.
6. **`EnvDX`'s tables are ported verbatim from Dexed**, not hand-fit —
   an early hand-fit level curve and rate curve were both measurably wrong
   against real DX7 patch data once a direct comparison harness existed.
7. **Output level is folded into the envelope's own target**, not added
   as a separate offset afterward — the two compose non-linearly.
8. **No shared control-rate clock abstraction with the speech module.**
   `EnvDX` (one instance per operator, log2 fixed-point, exact per-sample
   linear interpolation via a precomputed gain/gain_step pair) and
   speech's segment sequencer (one instance per voice, plain float,
   one-pole IIR smoothing) share only the shape "check a countdown at a
   block boundary" — a shared abstraction would cost FM cycles it doesn't
   pay today and give speech an interpolation mechanism it doesn't use.
9. **LFO is per-voice, not global** — per-voice multitimbrality (patch
   pointer per voice) leaves a literal shared LFO with no principled
   behavior once two simultaneously-active voices with *different*
   patches both request global-phase mode, a case that can't arise on
   real single-timbral DX7 hardware. Per-voice-with-key-sync already
   covers the common "notes struck together share phase" fidelity case.
10. **Mod wheel competes with the patch's own LFO depth via `max()`**,
    not as a multiplier on it — the multiplier design meant a patch with
    real configured vibrato played completely flat until the wheel moved,
    which measured as a real defect against every factory patch, not
    expected behavior.
11. **No sine-table interpolation** — a 4096-entry table indexed by the
    top 12 phase bits costs identically to a 1024-entry non-interpolated
    table, matches real DX7 hardware's own resolution, and the resulting
    truncation spur floor is inaudible under FM's own harmonic density.
12. **`FM_BLOCK` = 16**, not raised or lowered — BLOCK=8 measures slightly
    cheaper per kernel but doubles how often the (real, now-larger)
    per-block envelope step runs; BLOCK=32 is both more expensive and
    gives a coarser, less accurate envelope transient. 16 was kept as the
    already-characterised middle point rather than trading a measured
    kernel win for an unmeasured control-rate loss.
13. **The operator kernel stays in flash, not SRAM** — counter to the
    original assumption. Measured SRAM placement forces linker veneer
    stubs (a flash-resident render loop calling SRAM-placed code crosses a
    range a direct branch can't reach), a real cost with nothing to offset
    it once RP2350's XIP cache is accounted for.
14. **Two-operator loop interleaving was not adopted** — measured a small
    loss, not the expected largest win; the compiler already exploits
    most of the same instruction-level slack once both calls inline.
15. **The M33 DSP extension (`smulwb`) is used where convenient** —
    measured a real, no-cost win fusing a multiply and shift pair.
16. **The converter emits no per-operator reference gain or level
    constant** — the fixed-point contract's shared headroom makes the
    fan-in division an earlier design used (to avoid overflow on
    multi-carrier/multi-modulator algorithms) both unnecessary and
    actively wrong, since it would quietly make a multi-carrier patch
    quieter than its patch data specifies.
17. **A patch's real carrier release level (L4) passes straight through**,
    not forced to zero — an earlier design zeroed it on the theory that a
    nonzero L4 would leave the voice allocator unable to reclaim the
    voice. That reasoning didn't hold (the allocator can reclaim a
    released-but-decaying voice at lower steal priority), and the
    override silently discarded a real, deliberate part of some patches'
    design (sustained ring-out after key-off).
18. **`FM_PATCHES` lives in RAM, not flash** — op.h rereads a patch's fields
    every control block (`am_sensitivity`, `pitch_eg`, `lfo`), not just once
    at note-on, so unlike the kernel itself (Decision Record above), this
    table's cost is sensitive to where it sits, and flash placement measured
    a real, reproducible cost swing for one patch depending on unrelated
    changes to the rest of the table (a flash XIP-cache placement effect,
    not a converter or data bug). RAM access has no such sensitivity. This
    doesn't reopen the kernel-placement question: that one was rejected for
    a code-only branch-range cost (linker veneers) that plain data never
    pays.
19. **No operator-budget voice allocation.** Every real DX7 patch uses all
    6 operator slots — none of this engine's factory or converted content
    can produce a voice cheaper than 6 operators, so a cost-weighted
    allocator would behave identically to the existing flat, per-slot
    `voice_alloc` until patch content that isn't 6-op exists. A future
    synthesis flavor with a genuinely different operator cost (a fixed
    2-operator architecture, for instance) is expected to become its own
    engine with its own flat `MAX_VOICES` pool, not a second patch shape
    sharing this one's allocator — the render kernel, envelope, and
    routing model here are DX7-specific, not generic across FM chip
    families. Revisit only if a concrete need for a shared, cost-weighted
    pool arises, with a fresh design pass at that point.
20. **CC30 (the encoder-CC patch-select alternative) was dropped** once
    Program Change moved onto the shared Router's `CONFIGURATION` category
    — the two were fully duplicate logic, and standardizing every module's
    preset selection on Program Change alone (rather than each module
    picking its own CC/PC split) keeps that category's meaning consistent
    across engines.
21. **`FM_PATCHES`/`FM_PATCH_COUNT`/`FM_PATCH_NAMES` are always defined**
    (`patch.h`), never conditionally absent — without a locally generated
    DX7 bank they fall back to a single entry wrapping `FM_TEST_PATCH`,
    so Program Change and the display never need to branch on whether a
    real bank exists. `T00T_FM_HAS_PATCHES` still gates the host-render
    tools' own real-bank-only tests (`render_fm`/`render_fm_patch`),
    unrelated to this fallback.
22. **`midi_controller_process()` is built from shared, module-agnostic
    generic helpers** (`src/midi/midi_dispatch.h`), not written from
    scratch — comparing this module's freshly-migrated controller against
    `subtractive`'s found the two nearly identical beyond Handler-local and
    generic-plumbing differences, so the per-message-type dispatch shape
    (CC, pitch bend, Program Change, NOTE) moved into shared functions
    every module composes, while the mapping table and Handlers stay fully
    this module's own.
23. **Voice allocation lives inside `set_note()`, not the dispatch loop**
    — `voice_alloc_allocate()`/`release()` are the Voice Allocation
    Interface (CONTEXT.md), reached from a Handler; `set_note()` resolves
    its own voice via its own `note_voice[]` lookup (steal-on-retrigger
    included), so `midi_controller_process_generic()` needs no per-voice
    tracking arrays and dispatches NOTE the same uniform way as every
    other category.

## Glossary

- **Operator**: one sine-wave generator with its own envelope and
  frequency ratio — the DX7's basic building block, 6 per voice.
- **Algorithm**: a fixed routing topology (which operators modulate which,
  which are carriers) — one of 32 on real DX7 hardware; this engine
  supports any DAG-plus-self-feedback topology, not just those 32.
- **Carrier**: an operator whose output is audible directly, not just used
  to modulate another operator.
- **Modulator**: an operator whose output phase-modulates another operator
  instead of (or in addition to) being audible directly.
- **Feedback**: an operator (or, on two DX7 algorithms, a pair) whose
  output modulates itself.
- **EG (envelope generator)**: the DX7's 4-stage (rate, level) contour,
  one instance per operator (amplitude) or per voice (pitch).
- **Block**: the control-rate step size — 16 samples here — at which the
  EG/LFO/pitch EG are recomputed, distinct from a DMA buffer or a sample.
- **Ratio / fixed-frequency mode**: an operator's frequency is either a
  ratio of the played note (default) or a fixed Hz value independent of
  pitch (used for inharmonic bell/percussion partials).
- **TL (output level)**: an operator's programmed output level, 0–99 DX7
  units, composed with key/velocity scaling before conversion to gain.
- **PMD / AMD**: pitch/amplitude modulation depth — how strongly the LFO
  affects pitch or amplitude.
