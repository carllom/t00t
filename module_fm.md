# T00T — FM Module (DX7-class)

A design document for a dedicated **6-operator phase-modulation engine** for the
t00t platform, targeting DX7 feature parity and DX7-class polyphony on the
RP2350. It is a *mode* — a build-time engine variant selected via `T00T_ENGINE=fm`,
following the pattern established by `groovebox` and the layout already sketched
in `architecture.md`.

A secondary, much smaller proposal — adding a 2-operator FM oscillator to the
*generic* subtractive engine — is covered in §10. The two are independent and can
proceed in either order.

> ~~**Provenance of the numbers in this document.** The per-operator cycle
> figures in §3 are derived from *static instruction counts*... **P0 exists to
> replace these estimates with measurements before any DX7 logic is
> written.**~~ **Struck, #43.** P0's profiling-pin bench pass is done
> (`history_fm.md` §"FM P0 Measurement (#43)") — the per-operator kernel costs in
> §3.2/§3.3 are now confirmed by real hardware measurement, matching the
> static analysis closely (unlike the tracker/speech pattern of measured
> running 25–50% *above* static, this landed at or slightly below). One piece
> remains an estimate rather than a measurement: §3.3's ~19 c/f/voice
> EG/LFO/pitch-EG control-rate overhead, which P0's rig deliberately excludes
> (no EG, no LFO — that's P2 scope, per §1's phasing table). Treat the
> per-operator/per-voice kernel numbers as settled; treat the full-voice
> total (kernel + EG/LFO) as a measured-plus-estimated projection until P2.

---

## 1. Scope & phasing

| Phase | Deliverable | Gate |
|-------|-------------|------|
| **P0** | **Measurement gate.** Stripped operator mixer: N voices × 6 ops, fixed increments, fixed gains. No EG, no LFO, no patch logic, no MIDI. Profiling-pin trace. | Decides the voice count for everything downstream |
| **P1** | FM engine skeleton: `engines/fm/`, `FmPatch`/`FmOp` structs, DAG routing resolved at note-on, single hardcoded test patch, MIDI note on/off | Audible 6-op tone with correct ratios |
| **P2** | `EnvDX` — 4-stage log-domain EG at control rate, per operator; output level, velocity sensitivity | Patches evolve; DX-recognisable EP/bell |
| **P3** | Host converter `syx2patch.py`: DX7 32-voice `.syx` → `patches.h`. Key level scaling, rate scaling, detune, fixed-frequency mode | Real DX7 banks load and sound approximately right |
| **P4** | Pitch EG + LFO (rate/delay/waveform/sync, PMD/AMD, per-operator AM sensitivity) | Full DX7 parameter set covered |
| **P5** | Free routing UI/patch model beyond the 32 algorithms; operator-budget voice allocation; per-voice multitimbrality | Beyond-DX7 territory |
| **P6** | Calibration pass against Dexed on the reference banks; kernel hand-tuning (§3.6) | Timbral verification + final voice count |
| **X1** | Multi-operator feedback loops (DX7 algorithms 4 and 6) via a sample-interleaved fallback kernel | See §4.2 |
| **X2** | Operator waveform variants (DX11/TX81Z-style saw/square/half-sine) — one extra table, no extra cycles | Cheap; do it if P0 leaves headroom |

**Start at P0 and do not skip it.** The entire architecture below is sized around
a per-operator cost that has not yet been measured on hardware.

---

## 2. Reuse inventory

FM is unusual among the planned modules in how *little* it needs from the existing
DSP layer — and in how little memory it consumes.

| Existing component | File | Disposition for FM |
|---|---|---|
| ParamExchange double-buffer IPC | `engines/*/engine.h` | **Unchanged mechanism**, new `VoiceParams` payload (§6.3) |
| Dynamic voice allocator | `voice_alloc.*` | **Reused**, with an operator-cost weight added (§6.4) |
| MIDI transports + controller | `midi/` | Unchanged |
| I2S DMA output | `output.*` | Unchanged |
| Delay + Freeverb FX (global, post-mix) | `fx/` | Unchanged — global insert on Core 1 |
| LCD display + telemetry | `wslcd/` | Unchanged; voice dots and CPU load work as-is |
| Sine wavetable | `osc/sine.*` | **Superseded** — FM needs a larger table and a *non*-interpolating lookup (§3.5, §5.1) |
| ADSR envelope | `envelope.*` | **Not reused.** The DX7 EG is a different animal (§5.3) |
| SVF / ladder filter | `filter.h`, `ladder.h` | Not used |
| Sample playback | `osc/sample.h` | Not used |
| PSRAM (QMI CS1) | — | **Not needed.** Total FM working set ≈ 18 KB (§8) |

**Key observation:** unlike the tracker and sample engines, FM is data-light. The
entire module — sine table, a full 32-voice DX7 bank, and all voice state — fits
in ~18 KB of SRAM with no external memory and no streaming. That is an
architectural advantage worth protecting: nothing in this design should introduce
a dependency on PSRAM.

---

## 6. Architecture integration

### 6.1 Build-time engine variant

`src/engines/fm/`, selected by `T00T_ENGINE=fm`, per Option A in `architecture.md`.
The `fm/` directory is already anticipated in the proposed layout there.

```
src/engines/fm/
  engine.h        ← VoiceParams, VoiceParamBlock, ParamExchange
  engine.cpp      ← audio_engine_run(): render pass, voice loop, FX insert
  op.h            ← FmOp + the three kernels
  env_dx.h        ← 4-stage log-domain EG
  pitch_eg.h
  lfo.h
  sine_tab.h
  patch.h         ← FmPatch / FmOpParams (runtime form)
  patches.h       ← GENERATED by tools/syx2patch.py — do not hand-edit
  presets.h       ← VoicePreset + voice_apply_preset() + PresetId
  display.cpp     ← FM-specific LCD panel
```

### 6.2 Prerequisite

None beyond what `groovebox` already required. `engine_base.h` provides
`MAX_VOICES`, `PROFILE_PIN`, `EffectParams`. If the `VoiceNoteBase` refactor
sketched in `architecture.md` lands first, FM should use it; if not, FM declares
the same four fields inline as the other engines currently do.

### 6.3 `VoiceParams` — small, with a patch pointer

```cpp
struct VoiceParams {
    uint32_t phase_inc;      // base pitch (Core 0 computes as today)
    int16_t  amplitude;      // velocity 0..32767
    uint8_t  trigger;        // generation counter
    bool     gate;
    const FmPatch *patch;    // ← the whole timbre, one pointer
    int16_t  bend;           // pitch bend, cents
    int16_t  mod_wheel;      // Q15, → LFO depth
};
```

**Per-voice multitimbrality is that pointer.** It costs nothing per sample, needs
no change to the `ParamExchange` mechanism, and requires no per-sample dispatch —
unlike the tagged-union Option B discussed in `architecture.md` for mixing
*fundamentally different* engines. Within the FM engine, all voices run the same
kernel; only their routing tables and coefficients differ.

Core 0 assigns a patch per MIDI channel and writes the pointer at note-on. 16-part
multitimbral falls out for free.

### 6.4 Voice allocation by operator budget

`voice_alloc` is reused with one addition: each voice carries an **operator cost
weight** (its patch's active operator count, 1–6). The allocator holds a total
operator budget rather than a voice count.

Consequences:
- A bank of 2-op patches gives far more than 16 voices (capped by `MAX_VOICES`).
- A 6-op patch bank gives exactly the §3.4 number.
- Mixed multitimbral use degrades gracefully rather than by worst case.

Steal priority is unchanged (silent → released → oldest active), driven by Core 1's
active bitmap as today. The only change is that stealing continues until enough
*operators* are free, not until one voice slot is free.

The LCD voice-dot display should be extended to show operator budget utilisation
alongside the existing per-voice dots.

### 6.5 IPC unchanged

`ParamExchange` double-buffering, `commit()` + `__sev()`, Core-1-owned runtime
state in file-scope arrays. No changes. FM's runtime state — six `FmOp`, six
`EnvDX`, pitch EG, LFO per voice — lives in Core 1 file scope exactly as
`voice_phase[]` and `envelope[]` do today.

---

## 4. Free routing vs. fixed algorithms

**Decision: free routing. There is no performance argument for fixing the
algorithms.**

### 4.1 Routing is note-on data, not inner-loop work

In the block-inner form, an operator's routing *is* its `in` and `out` bus
pointers plus its position in the processing order. Both are resolved once at
note-on, from data the host converter already baked. A hardcoded algorithm and a
free 6×6 routing matrix compile to **identical inner loops**.

What fixed algorithms would buy is *fused* kernels — e.g. a 2-op stack that keeps
the modulator's output in a register instead of round-tripping a bus buffer,
saving a store + load (~4 cycles/sample/pair). That is perhaps 10–15% in exchange
for 32 hand-written kernels. Defer indefinitely; revisit only if P6 shows the
module is short of its target and everything in §3.6 is exhausted.

Free routing also delivers **per-voice multitimbrality for free** (§6.3), which a
fixed-algorithm design would not.

### 4.2 The constraint: DAG + self-feedback

Block-inner rendering requires the operator graph to be a directed acyclic graph
plus self-loops. A cycle spanning two or more operators cannot be evaluated in
block order without introducing a block-length (16-sample ≈ 0.36 ms) delay, which
is a comb filter at ~2.7 kHz — not an acceptable approximation.

This covers **30 of the 32 DX7 algorithms**. Only algorithms 4 and 6 use a
multi-operator feedback loop; every other algorithm wraps feedback around a single
operator. (The DX7 hardware permits exactly one feedback loop per algorithm, which
is why no algorithm needs more than this.)

**P1–P6 ship DAG + self-feedback and document algorithms 4 and 6 as unsupported.**
Options for X1, in preference order:

1. **Sample-interleaved fallback kernel.** Voices whose patch contains a
   multi-operator loop are rendered with a sample-outer loop for the operators
   inside the strongly-connected component only. Correct, 1-sample delay in the
   loop (which is what the DX7 hardware does anyway), ~30% more expensive for that
   voice. The patch loader detects the SCC and sets a flag.
2. **Collapse to self-feedback** on the topmost operator of the loop. Cheap,
   wrong, but only audibly wrong at high feedback settings.

The converter should refuse to silently mis-render: if it emits a patch using
algorithm 4 or 6 before X1 lands, it should mark the patch and the engine should
either apply option 2 with a logged warning or skip the patch.

### 4.3 Bus allocation

The DX7's maximum serial modulation depth is 3, so no algorithm needs deep bus
chains. Allocate 6 modulation buses + 1 output bus and let the converter assign
them by liveness.

At BLOCK=16 and `int32`, that is 7 × 16 × 4 = **448 bytes** — and because voices
render sequentially within a pass, this is **one shared scratch for the whole
engine**, not per-voice.

---

## 5. New DSP components

### 5.1 `fm/sine_tab.h` — operator sine table

4096 × `int16_t`, quarter-wave symmetric generation at init, full table stored (no
runtime symmetry unfolding — 8 KB is cheaper than the instructions). Indexed by
`phase >> 20`. Placed in SRAM.

X2 would add saw / square / half-sine / abs-sine tables in the same format,
selected by a per-operator table pointer resolved at note-on. Zero inner-loop cost.

### 5.2 `fm/op.h` — the operator kernel

```cpp
struct FmOp {
    uint32_t phase;
    uint32_t inc;         // Q32 phase increment (ratio × note × detune)
    int32_t  gain;        // Q15 linear, current
    int32_t  gain_step;   // Q15 per-sample delta for this block
    const int32_t *in;    // modulation bus (points at a zero bus for pure carriers)
    int32_t *out;         // output bus (or the shared voice mix bus)
};
```

Three kernel variants, selected per operator at note-on:
`op_render()` (plain), `op_render_fb()` (self-feedback, 2-sample average),
`op_render_first()` (stores rather than accumulates — eliminates bus clearing).

The `in`/`out` pointers and the kernel function pointer, plus the processing
order, constitute the entire routing implementation.

**Modulation-depth ceiling, found and fixed by #57.** #44's original kernel
right-shifted every operator's `gain * sample` product by a single constant
(`FM_OUT_SHIFT = 6`, on top of `fm_mul_gain`'s own implicit `>>16`) whether
that operator was a carrier (needs ±32767 int16-audio-range output) or a
modulator (needs raw magnitude approaching `2^32` for a full radian of phase
deviation, since its `out[]` is added directly into the next operator's
32-bit phase accumulator). One shift serving both scales capped modulator
deviation at ~0.03–0.05 rad even at `gain = INT32_MAX` — barely audible, and
invisible with #44's single hand-tuned `FM_TEST_PATCH`, but #47's real DX7
conversions exposed it hard: real patches sounded like almost-plain sine
tones on real hardware, differing only in envelope/octave, never in timbre.
#57 split the shift by role (`FM_OUT_SHIFT_CARRIER = 6`, unchanged;
`FM_OUT_SHIFT_MODULATOR = 0`, i.e. only `fm_mul_gain`'s own `>>16`), raising
modulator headroom ~64× to ~1.57 rad at `gain = INT32_MAX` — real DX7-range
depth. Passed as a per-call parameter (chosen once by
`fm_voice_render_block()` from the routing, never inside the per-sample
loop), so the loop's instruction shape is unchanged (disassembly-verified:
still 48 `smlawb` instances in the device build, same as #44/#45) and
carrier output/headroom is untouched. The new headroom made a real,
previously-impossible overflow risk real too — multiple modulators can sum
into one shared bus (algorithms 7/8/10/12/13, several used by ROM1A/B) — so
both `tools/syx2patch.py` and `FM_TEST_PATCH` (patch.h) now divide each
modulator's `level` by how many operators share its target bus, the same
fix already applied to multi-carrier algorithms in #47.

**Still not enough — a second, structural gap, found by comparing against
Dexed's actual per-sample code.** Carl's hardware listen after the shift
split: real ROM1A patches changed but stayed "soft," nothing like the bite
and high frequencies real DX7 brass has. Fetching Dexed's own kernel
(`Source/msfa/fm_op_kernel.cc`/`sin.h`, Apache-2.0) directly answered "what
are we missing": `FM_OUT_SHIFT_MODULATOR = 0` already extracts the maximum
magnitude a 32-bit `gain * sample` product can produce — that multiply
cannot yield a wider result, full stop. But Dexed doesn't need a bigger raw
magnitude at all, because its phase representation only needs `2^24` units
per full cycle (`Sin::lookup`'s table read structurally ignores every bit
above bit 23), not `2^32` like this engine's `phase`. The real gap was never
"how much magnitude can the multiply produce" — it's "how much phase
deviation a given magnitude buys," and this engine's wider (`2^32`)
per-cycle convention — chosen for `inc`'s own pitch-increment precision
across the full MIDI range, not for modulation sensitivity — was quietly
taxing every modulator 256× relative to Dexed's own choice.

Closed with `FM_MOD_INPUT_SHIFT = 4` (op.h): incoming modulation (`in[i]`,
and self-feedback's `fb1`/`fb2` average) is pre-scaled left by 4 bits,
using unsigned wraparound, *before* being added to `phase` — mathematically
the same trick a narrower phase representation gets for free, applied only
at the point it's needed. Nothing is stored any wider (`in[]`/`out[]` stay
`int32_t`, so the fan-in/carrier-count overflow fixes above are untouched);
only how aggressively a stored value bends phase changes. Host-measured
across ROM1A's 28 patches: 2nd-harmonic/fundamental ratios now span ~0.0
(patches with no real modulation at that voicing, e.g. VIBE/MARIMBA/TIMPANI
— legitimate) to several times the fundamental (BRASS 1 ~3.9×, ORCHESTRA
~7.0×) — real variety tracking the underlying algorithm/data, not a uniform
near-zero. `FM_TEST_PATCH`'s own modulator levels needed retuning down at
this new headroom (its old near-ceiling constants, picked before this shift
existed, over-drove badly enough at some EG rates to underflow
`eg_to_linear()` mid-attack — a real bug, caught before hardware).

One casualty: `render_fm`'s own routing/ratio spectrum check started
failing at the new depth, and *not* because anything was actually broken.
`FM_TEST_PATCH`'s op0 (ratio 0.5, feeding the carrier two hops upstream
through op2→op4) barely mattered at the old, tiny ceiling; at real depth it
visibly pulls the whole chain's true fundamental down to half the played
note. The check's own "noise floor" probe (`note*4.5`) turned out to be
exactly the 9th harmonic of that real 110 Hz fundamental, not noise at all
— confirmed by sweeping the full spectrum and finding every harmonic of the
*true* fundamental populated, zero everywhere else. Fixed by moving the
probe to a frequency verified off both grids (`note*4.3`), not by detuning
the patch further to satisfy a check that had the wrong reference frequency.

### 5.3 `fm/env_dx.h` — 4-stage DX7 envelope

**Do not reuse `envelope.*`.** The DX7 EG is 4 × (rate, level) pairs operating
linear-in-dB, which is a large part of why its decays sound the way they do. An
ADSR bent into that shape would be both slower and less accurate.

Design:

- EG state and arithmetic live in the **log domain** (linear-in-dB ramps).
- The EG steps **once per control block** (BLOCK=16 → 2756 Hz), not per sample.
- At each block boundary, the log level is converted to linear Q15 via a small
  exp2 table, and the kernel is handed `gain` (start) and `gain_step` (delta) for
  the block.
- Per-sample cost in the kernel: **one add**. Zipper-free.

Block size is chosen by EG time resolution, not by the operator kernel. BLOCK=16
gives 0.36 ms granularity, adequate for the DX7's fastest attacks; BLOCK=32 (0.73 ms)
is likely too coarse for rate-99 attacks. Confirm empirically in P2.

**Level and rate curves, both ported from Dexed after #45's hand-fit
approximations turned out significantly wrong.** #45 shipped both
`DX7_LEVEL_TO_LOG2[]` (TL/EG-level → attenuation) and `DX7_RATE_TO_STEP[]`
(EG rate → octaves/second) as unverified curve-shape guesses — a quadratic-
in-dB level fit and a smooth single exponential rate fit. Both were caught
and replaced only once real DX7 patch data (#47) and real listening (#57)
made the gap audible, and both were fixed the same way: porting and
*running* Dexed's actual `Env`/`Exp2` code (Source/msfa/env.cc/exp2.cc,
Apache-2.0) to get ground-truth numbers, not re-deriving another formula
guess.

- **#58 (level):** the quadratic fit was still considerably flatter than
  real DX7 across most of the 0-99 range (level 90: ours -0.8 dB vs real
  -6.8 dB; level 70: -8.2 dB vs -21.9 dB) — letting anything short of a
  near-maxed operator run hotter than authentic, which is what #57's newly-
  unlocked modulation depth (below) exposed as "sometimes overdriven."
- **#59 (rate):** the exponential fit was up to ~23× too slow at the high
  (fast) end — real DX7 rate isn't smooth, it's piecewise, built from a
  `qrate` value and a `(4+(qrate&3)) << (2+6+(qrate>>2))` bit-shift step
  that accelerates far more steeply than any single exponential can. R1=99
  (an extremely common "instant attack" choice, including this file's own
  `FM_TEST_PATCH`) was landing roughly 20× slower than real hardware —
  audible as "sluggish" envelopes and a perceptible note-on-to-audible
  delay, worst on patches whose rates sit furthest from the R99 extreme.

Both fixes are drop-in table replacements — the underlying "log2 domain,
one add per sample, block-rate EG step" architecture (this section's design
bullets above) is completely unchanged; only the *numbers* in the tables
were wrong. `tools/host_render`'s own EG-shape checkpoints (calibrated to
the old, much slower rate curve) needed updating alongside #59 to still
mean anything at the new, correct speed.

> **Cross-module note, closed #46: reject, no shared code.** This looked
> structurally like the speech module's per-voice segment clocks: *N
> independent control-rate clocks per voice*, stepped at block boundaries,
> driving per-sample interpolated values. Compared against the real trees
> (`env_dx.h`/`op.h` here vs. `sequencer.h`/`tract.h` in speech) once both
> existed, the shared shape turned out to be only "check a countdown at a
> block boundary, then update state something downstream reads." Everything
> below that diverges: `EnvDX` is one instance *per operator* (6 independent
> fixed-4-stage machines per voice) in log2 fixed-point, handing the kernel
> an exact `gain`/`gain_step` pair for true per-sample linear interpolation;
> speech's sequencer is one clock *per voice* driving a variable-length
> phoneme segment list in plain float, and its "step" is a one-pole IIR
> smooth toward target held constant for the whole sub-block — no per-sample
> interpolation at all. A shared `BlockClock` would force a lowest-common-
> denominator template that costs FM cycles it doesn't pay today and gives
> speech an interpolation mechanism it doesn't use. Common pattern, no common
> code — see `architecture.md` "Settled Decisions" for the full writeup.
> Speech's sequencer (#34/#36/#37, hardware-verified) is untouched.

### 5.4 `fm/pitch_eg.h` — pitch envelope

One per voice, same 4-stage shape as `EnvDX` but operating on pitch (in cents)
rather than amplitude. Applied by scaling all six operator increments at each
block boundary. Per-sample cost: **zero**.

**Implemented, #49.** Rate and level tables (`DX7_PITCHENV_RATE`/
`DX7_PITCHENV_LEVEL`) ported verbatim from Dexed's `PitchEnv` (Source/msfa/
pitchenv.{h,cc}, Apache-2.0), same rigor #58/#59 already established for
`env_dx.h`'s own tables — cross-checked by porting and running Dexed's real
formula in a standalone calibration harness before committing to the
cents-domain re-expression: level 0/50/99 → -4800/0/+4762.5 cents (the
table's own ±128 raw span *is* the DX7's real ±4-octave pitch EG range, no
rescaling surprises), rate 0/99 → 0.047/11.97 octaves/sec (a rate-99 pitch
EG sweeps its full ~8-octave span in ~0.67s — a real, audible swoop, not
instant like the amplitude EG's own rate 99, which is correct DX7 character:
the "attack blip"/"brass scoop" this issue names is a fast *relative* move
over tens/hundreds of cents, well under that full-span figure). The
4-stage state machine is exactly isomorphic to `EnvDX`'s (reasoned through
against Dexed's real `PitchEnv::getsample()`/`keydown()` logic, not assumed):
stages 1-2 auto-advance on reaching target, stage 3 holds forever while a
note is held, stage 4 (release) is entered only by `fm_pitch_eg_release()`,
jumping from wherever the EG currently is — same convention
`env_dx_release()` already uses. One real footgun documented and guarded
against: `fm_pitch_eg_trigger()` starts from L4 (release level), not
silence — a zero-initialized `FmPitchEgParams` (level all 0) is a real
~4-octave pitch drop on every note, not "off"; "off" is level `{50,50,50,50}`
(DX7 hardware center). Every hand-written `FmPatch` literal (`FM_TEST_PATCH`
and anything copy-constructed from it) sets this explicitly — see `patch.h`'s
own comment. `tools/syx2patch.py` never hits this pitfall since it always
copies real patch bytes straight through.

### 5.5 `fm/lfo.h` — voice LFO

Rate, delay, waveform (tri/saw up/saw down/square/sine/S&H), key sync, PMD, AMD.
Evaluated once per control block. Pitch mod folds into the increment scaling
alongside the pitch EG; amplitude mod folds into each operator's `gain`/`gain_step`
computation according to its AM sensitivity. Per-sample cost: **zero**.

**Implemented, #49 — closes open question 5: per-voice, global-phase
mode dropped (not deferred).** This document's own recommendation (per-voice with
key sync, "strictly better for polyphony") is what shipped; the "patch flag
for global-phase mode" alternative was considered and rejected, not left
unbuilt. Reason: #48 already made this engine genuinely multitimbral (one
patch pointer per voice), and a literal single shared LFO has no principled
behavior once two simultaneously-active voices request global phase with two
*different* patches' rates — a case that structurally cannot arise on real
single-timbral DX7 hardware, so there's no "real DX7 behavior" to fall back
on for it. Per-voice-with-key-sync already gives every note struck at the
same instant identical LFO phase (the common "block chord" case DX7 fidelity
actually cares about); the residual gap — notes of the *same* patch struck at
different times drifting slightly out of phase — is small and patch-
dependent, not worth the architectural ambiguity. Revisit only if #53's real
Dexed-diff work finds this audible on a reference bank.

Waveform math is reasoned-through against Dexed's real per-waveform bit
tricks (triangle/saw-down/saw-up/square/sine, `Source/msfa/lfo.cc`,
Apache-2.0) and cross-checked shape-by-shape, but re-expressed as a plain
float function of this file's own Q32 phase convention rather than
replicating Dexed's own phase/table Q-format bit-for-bit — that format is
tuned to Dexed's internal table size, not a property of the DX7's real
waveform shapes, and control-rate cost (~2756 Hz at BLOCK=16) makes float
math free either way. Rate table (`DX7_LFO_RATE_SOURCE`) *is* ported
verbatim (real hardware-calibrated data) — real Hz cross-checked against
commonly cited DX7 figures: rate 0 ≈ 0.065 Hz (~15.5s period), rate 99 ≈
50.9 Hz. Delay ramp reduces Dexed's own two-stage Q32 accumulator to a
single dominant-stage real-seconds formula (delay 0 → instant, delay 99 →
~2.66s, delay 50 → ~0.31s, all cross-checked) — a documented simplification,
not a fidelity claim; the two-stage curve exists to serve Dexed's own
fixed-point arithmetic, not because the perceptual fade-in needs two pieces.

PMD/PMS/AMD combination *is* numerically equivalent to Dexed's real
`Dx7Note::compute()` LFO-driven pitch/amp-mod terms — the real formula is
exactly four independent multiplicative factors (depth × sensitivity × delay
× LFO value) with no cross-term, so re-deriving it as plain fractions times
one calibration constant (`FM_LFO_PMD_MAX_CENTS`, cross-checked at ≈1190.6
cents — just under an octave — at PMD=99/PMS=7/LFO-extreme against a
standalone harness running Dexed's real integer formula) is exact, not
approximate, just without the Q24/Q32/`>>39` bit-shift plumbing that only
ever existed to keep Dexed itself integer-only. Dexed's *separate*
non-LFO controller-driven pitch-mod path (a JUCE-plugin-configurable
mod-matrix feature) is **not** replicated — this issue's own acceptance
criterion ("mod wheel scales LFO depth") asks for the simpler, extremely
common convention `lfo.h` implements instead: mod wheel is a 0..1 multiplier
on the LFO's own configured PMD/AMD depth, matching speech's own CC1 "mod
wheel → vibrato depth" precedent (#36) — **not** real DX7 hardware's own
convention, where a patch's configured PMD/PMS plays at full depth
regardless of wheel position. Flagged explicitly for the first hardware
listen: a patch with real vibrato/tremolo configured will sound completely
flat until the mod wheel is actually moved (`VoiceParams::mod_wheel`,
CC1) — expected, not a bug.

`VoiceParams` gained `mod_wheel` (Q15, `engine.h`) — §6.3's original sketch
already anticipated this field name. Host-verified (`render_fm`'s
`run_pitch_eg_check()`/`run_lfo_check()`): pitch EG's L4-start/attack-blip/
settle/release-swoop shape and its per-operator increment scaling (moves a
ratio operator, exempts a fixed-frequency one — matching Dexed's own
`osc_freq()` fixed-mode branch, which only ever receives pitch bend, never
pitch/LFO mod); all six LFO waveform shapes; rate/delay calibration anchors;
a real block-rate pitch/amp swing near the calibrated max; `mod_wheel=0`
silencing both outputs; the delay ramp actually fading in; and AM reaching
`fm_voice_step_envelopes()`'s real gain output, weighted by each operator's
own `am_sensitivity` and leaving `am_sensitivity=0` operators untouched.
Kernel disassembly unaffected (still 48 `smlawb` instances, device build) —
everything here is note-on/block-rate, never inside the per-sample loop,
confirming the "zero per-sample cost" constraint this section opens with.
`tools/syx2patch.py` bakes the voice-wide pitch EG (bulk offset 102-109) and
LFO (offset 112-116) bytes straight through, same "no host-side DSP math"
reasoning as every other field — offsets and the LKS/LFW/LPMS bit-packing in
byte 116 cross-checked against Dexed's own `Cartridge::unpackProgram`
(`Source/PluginData.cpp`, Apache-2.0).

### 5.6 Note-on-time computation (no runtime cost)

All of the following are resolved once per note-on and never touched again:

- **Key level scaling** — breakpoint, left/right curve and depth → an output-level
  offset per operator.
- **Rate scaling** — → an EG rate offset per operator.
- **Velocity sensitivity** — → an output-level offset per operator.
- **Coarse/fine ratio, detune, fixed-frequency mode** → the Q32 increment.
- **Routing** → order, `in`/`out` pointers, kernel selection, first-writer flags.

This is where the DX7's nonlinearity lives, and none of it belongs in the render
loop.

**Implemented, #48**, same session as #57/#58/#59, all four ported directly
from Dexed (`dx7note.cc`'s `ScaleRate`/`ScaleLevel`/`ScaleCurve`,
`osc_freq()`'s fixed-frequency branch), not re-derived approximations:

- **Key level scaling** (`env_dx.h`'s `dx7_scale_level`/`dx7_scale_curve`):
  resolved at note-on, combined with TL *before* the log2 conversion and
  clamped to [0,127] exactly like Dexed's own `outlevel = min(127,
  outlevel)` — not added as a separate, unclamped log2 offset. A boosting
  curve (DX7 curve 2/3, "+EXP"/"+LIN") at high depth and an extreme note can
  otherwise push the combined value well past what a reference `level` was
  ever meant to represent, and `eg_to_linear()`'s shift has no defined
  behavior for a large positive log2 offset — this clamp is what keeps it
  inside the range that function already guarantees is safe.
- **Rate scaling** (`env_dx.h`'s `dx7_scale_rate`): resolved at note-on into
  a qrate *delta* (`FmOp::rate_scale_qrate`), added to the base rate's own
  qrate at every stage transition — matching Dexed's `qrate +=
  rate_scaling_` order exactly, not applied as a post-hoc scale on the
  already-converted octaves/second value (a different, less faithful order
  of operations). Zero-delta is the exact same table-lookup fast path #59
  already verified, so this is behavior-neutral for every existing patch.
- **Detune**: applied via the Q32 increment path that already existed since
  #44 (`FmOpParams::detune_cents`, previously always 0 from the converter).
  Approximated as a fixed ±7 cents at the DX7 detune extremes rather than
  Dexed's real note-frequency-dependent formula — replicating that exactly
  needs the full pitch pipeline's absolute log-frequency value, which
  `tools/syx2patch.py` (a note-independent, note-on-baking converter)
  doesn't have; small enough that "audible as beating between two operators
  at the same ratio" (this issue's own acceptance bar) holds regardless.
- **Fixed-frequency mode**: also the Q32 increment path (`FmOpParams::
  fixed_hz`/`fixed_freq`, previously unused since the converter skipped
  every fixed-frequency patch outright). `tools/syx2patch.py`'s
  `op_fixed_hz()` reduces Dexed's `osc_freq()` fixed-mode branch to a
  closed form, `Hz = 10^(((coarse&3)*100+fine)/100)` — Dexed's own constant
  `4458616` is exactly `(2^24 * log2(10) * 0.01) << 3`, so no need to
  replicate its Q24-log-frequency/`Freqlut` machinery. All 32 of ROM1A now
  convert (was 28/32 — the 4 previously-skipped patches, "TUB BELLS"/"STEEL
  DRUM"/"REFS WHISL"/"TRAIN", all use fixed-frequency operators for
  inharmonic bell/percussion partials, exactly the DX7 use case this
  unblocks).

`VoiceParams` gained a raw MIDI `note` field (0-127) — key level/rate
scaling need the actual note, which `phase_inc` alone (already bend-scaled
into a frequency) can't reconstruct. `fm_voice_note_on()`'s signature grew
a `midinote` parameter accordingly; every existing caller (device and host)
updated. Host-verified with a dedicated check (`render_fm`'s
`run_key_rate_scaling_check()`) that builds a patch with deliberately
nonzero scaling and confirms both the resolved note-on values and the
actual per-sample speed/level differ in the expected direction between a
low and a high note — nothing else in the test suite would have exercised
these code paths at all, since `FM_TEST_PATCH`'s own scaling fields default
to zero (no scaling, preserving its exact existing behavior). Kernel
disassembly unaffected (still 48 `smlawb` instances) — everything here is
note-on/block-rate, never inside the per-sample loop, exactly as this
section's own opening claim requires.

---

## 7. Host-side tooling: `tools/syx2patch.py`

Following the principle established by the tracker's converter: **relocate all
awkward, nonlinear, one-time work to the host, so the runtime sees only increments
and pointers.**

**Implemented (v1), #47.** Input: a DX7 32-voice bulk dump `.syx` (4096-byte
payload, 128 packed bytes per voice, unpacked bit-for-bit against Dexed's own
`Cartridge::unpackProgram` — cross-checked against the published DX7 MIDI
Data Format Sheet too, and verified byte-for-byte via a synthetic-fixture
round-trip test, `tools/test_syx2patch.py`).

Output: `src/engines/fm/patches.h` — a `const FmPatch FM_PATCHES[]` array in
flash, plus `enum FmPatchId` and `FM_PATCH_NAMES[]`. Both the `.syx` input
and the generated header are gitignored, never committed — a real DX7 bank
is Yamaha's own commercial patch data, the same reasoning `xm2t00t`'s `xm/`
already established for copyrighted third-party `.xm` songs. The device and
`tools/host_render`'s `render_fm` both compile against `patches.h`'s absence
gracefully (`T00T_FM_HAS_PATCHES`, gated in both `CMakeLists.txt`s and
detected at configure time via `if(EXISTS …)` — the same pattern the
top-level `CMakeLists.txt` already uses for other optional generated files)
— every voice just plays `FM_TEST_PATCH` until someone runs the converter
locally.

```
syx2patch.py convert <in.syx> <out.h>   # writes patches.h (enum FmPatchId,
                                          # FM_PATCH_COUNT, const FmPatch
                                          # FM_PATCHES[], FM_PATCH_NAMES[])
syx2patch.py dump <in.syx>               # prints a per-voice summary
                                          # (algorithm, name, warnings), writes nothing
```

v1 actually resolves:

| DX7 source data | v1 handling |
|---|---|
| Algorithm number (0–31) | `mod_target`/`feedback` per operator, via one generic bus-flag decode (Dexed's own `FmAlgorithm` table, reconstructed into this engine's shape) applied uniformly to all 32 rows — `order`/`in_bus`/`out_bus`/kernel selection itself is *already* resolved at note-on by `fm_resolve_routing()` (#44), so the converter's only job is picking the right `mod_target` |
| EG rates/levels (0–99) | Copied straight through as raw bytes — `env_dx.h` (#45) already converts these at block-rate, so there is no host-side log-domain math to do |
| Operator output level (0–99), velocity sensitivity (0–7) | Copied straight through, same reasoning |
| Coarse/fine ratio | `ratio = (coarse==0 ? 0.5 : coarse) * (1 + fine/100)`, verified against Dexed's `osc_freq()`/`coarsemul[]` |
| Feedback level (0–7) | `feedback` bool on the algorithm's primary operator; level 0 is exact, not approximated (real hardware silence too) — the kernel has no depth control, so any nonzero level becomes "on" |
| Algorithms 4 and 6 | `needs_interleaved` detected via a real cycle check (not hardcoded algorithm numbers) on a test graph augmenting the ordinary routing with the secondary FB_OUT operator's tentative edge back to the primary — collapsed to single self-feedback, logged, never silent |
| Voice name | Retained, becomes `FM_PATCH_NAMES[]` and the sanitized `FmPatchId` enum value |

Deferred to v2 (#48)/v3 (#49), same v1/v2/v3 split `speechgen.py` used
(#32/#35): key level scaling, rate scaling, detune, fixed-frequency mode
(v2, #48); pitch EG and LFO (v3, #49, `pitch_eg`/`lfo` bulk offset 102-109/
112-116, cross-checked against Dexed's own `Cartridge::unpackProgram`). All
are parsed and range-checked from the `.syx` (so `.syx` corruption still
fails loudly) but not wired into `FmOpParams`/`FmPatch` until their runtime
consumer exists — v1's fixed-frequency operators were skipped outright
rather than approximated, since v1 had no way to represent them without
silently changing the patch's character; v2 fixed that. Transpose remains
deliberately unwired (#49) — it's a Core 0/MIDI-controller-layer concern, not
something a patch-data-only converter can apply.

### Algorithm decode

`DX7_ALGORITHMS[32]` is Dexed's own bus-flag table (Source/msfa/fm_core.cc,
Apache-2.0), reused as data — not because Dexed's 2-bus render scheme is
copied, but because it's a compact, already-correct encoding of all 32
algorithms' topology. `decode_algorithm()` simulates DX7's fixed OP6→OP1
processing order once, generically, to reconstruct each operator's real
`mod_target`: a bus (1 or 2) is a stack, written by a contiguous run of
operators, and the next operator that reads that bus number is,
unambiguously, every writer in that run's actual downstream target. One
function handles all 32 rows — the acceptance criterion "generated or
table-driven, not 32 hand-written cases" is met by the data being a table
and the decode being one generic simulation, not per-algorithm branches.
Hand-verified against algorithm 1 (two independent chains,
`6→5→4→3→OUT`/`2→1→OUT`, feedback on OP6) and algorithm 32 (all six
operators as independent carriers, feedback on OP6) — both match every
published DX7 algorithm chart.

Algorithms 4 and 6 each have a second operator with the FB_OUT bit set
alone (not paired with FB_IN, unlike the algorithm's primary feedback
operator) — real DX7 hardware routes that operator's output into the same
shared feedback register the primary operator reads, a genuine
operator-spanning loop closed only across block-processing order. Detected
generically (not by hardcoding "algorithm == 4 or 6"): a test graph adds a
tentative edge from every such secondary operator to the primary, and a real
cycle check runs over it. `needs_interleaved` is set when a cycle is found;
the fallback is simply that the secondary edge was never added to the real
emitted routing in the first place — `patches.h` gets a per-patch comment
and the tool logs a warning, `#54` is the eventual real fix.

### Packed-voice unpacking

`unpack_voice()` is a bit-for-bit port of Dexed's
`Cartridge::unpackProgram` (Source/PluginData.cpp, Apache-2.0), cross-checked
against the DX7 MIDI Data Format Sheet's own published "Bulk Dump Packed
Format" table. Every field is range-checked against its documented range
(0-99, 0-31, 0-7, …) and raises rather than silently clamping — unlike
Dexed's own `normparm`, which clamps corrupted bytes defensively since it
has to keep running against whatever's already loaded. `parse_syx_bulk()`
validates the full `F0 43 0n 09 20 00 … checksum F7` envelope, including the
masked 2's-complement checksum.

Two real, unanticipated issues surfaced only by actually host-rendering a
real bank (ROM1A), not by design review alone: multi-carrier algorithms
(19–32, 3–6 carriers summed into one voice) clipped int16 range under a flat
per-role reference gain — fixed by scaling each carrier's reference by
`1/carrier_count`; and a nonzero carrier release level (L4) would leave
`env_dx.h` unable to ever reach `EG_IDLE`, exactly the tracker's #21 "voice
never frees" bug shape — fixed by forcing carrier L4 to 0 with a logged
warning. `FmOpParams::level` itself (the reference-gain ceiling) is *not*
DX7 data on either side of that fix — see `patch.h`'s own comment and
`FM_TEST_PATCH`'s precedent, which v1 reuses directly (`FM_CARRIER_LEVEL_REF
= FM_TEST_PATCH`'s carrier constant, `FM_MODULATOR_LEVEL_REF` its highest
modulator constant).

Patch select is wired into `midi_controller.cpp` behind the same
`T00T_FM_HAS_PATCHES` gate: Program Change and CC30 (the BeatStep Pro can't
reliably send real Program Change, same reasoning #36 gave speech's
phrase-bank CCs) both pick `FM_PATCHES[value % FM_PATCH_COUNT]`.

**Verification asset:** because real DX7 banks load, [Dexed](https://asb2m10.github.io/dexed/)
becomes the ground-truth reference renderer for this module — the same role
`openmpt123` plays for the tracker and `say -v Fred` plays for speech. P6 is a
calibration pass comparing per-patch output against Dexed on a fixed set of notes.
`render_fm`'s patch-bank render (`fm_patches/*.wav`, one 3-second note per
patch — long enough to catch a deliberately slow-swelling patch like ROM1A's
"TAKE OFF") is what #53 will diff against Dexed.

---

## 3. The performance question

### 3.1 Budget arithmetic

At 150 MHz and 44100 Hz, Core 1 has:

- **3401 cycles per sample frame**
- **870,748 cycles per 256-sample buffer** (5.805 ms deadline)

Calibrating against the measured figures in `history_subtractive.md`:

| Measured (`history_subtractive.md`) | Duty | Cycles/sample |
|---|---|---|
| Idle | 0.44% | ~15 |
| Subtractive voice (osc + ADSR + SVF + LFO) | ~5.9% | ~201 |
| 16 voices, max | ~90% | ~3060 |

16 × 5.9% = 94%, against a measured ~90%. The percent↔cycle conversion holds well
enough to plan in cycles and translate back.

### 3.2 Operator kernel instruction counts

Four candidate kernels were compiled and their loop bodies counted. The kernel is
**block-inner** (outer loop over operators, inner loop over a block of samples),
which keeps phase, increment, gain and gain-step resident in registers.

| Kernel | Instructions in loop | Est. cycles/op/sample |
|---|---|---|
| Plain operator, no interpolation | 13 (2 = loop overhead) | ~16–18 |
| Self-feedback (DX7 2-sample average) | 18 | ~21–23 |
| Linear interpolation | 19 | ~23–25 |

The plain kernel, as emitted:

```asm
.L2:  ldr   r3, [r1, #4]!        @ modulation bus in
      add   ip, ip, r5           @ phase += inc
      add   r3, r3, ip           @ phase + modulation
      lsrs  r3, r3, #22          @ table index — wrap is free (10 bits out of a u32)
      ldrsh r10, [r6, r3, lsl #1]
      asr   r3, lr, #8
      mul   r10, r3, r10         @ × envelope gain
      ldr   r3, [r2, #4]!        @ output bus accumulate
      cmp   r0, r1
      add   r3, r3, r10, asr #14
      add   lr, lr, r4           @ gain += gain_step
      str   r3, [r2]
      bne   .L2
```

Two properties worth noting:

- **Phase wrap is free.** With a 32-bit phase accumulator and a table indexed by
  the top bits, the right shift produces exactly the index width. No mask, no
  modulo, and phase modulation wraps correctly by construction.
- **The gain ramp costs one instruction.** `add lr, lr, r4` is the entire
  per-sample cost of a full 4-stage DX7 envelope, because the EG runs at control
  rate and hands the kernel a linear start value and a per-sample delta (§5.3).

### 3.3 Voice cost model

Per 6-operator voice, per sample:

| Component | Cycles |
|---|---|
| 5 plain operators @ ~17 | 85 |
| 1 self-feedback operator @ ~22 | 22 |
| Per-block overhead amortised (6 × EG step + exp2, LFO, pitch EG, bus setup) at BLOCK=16 | ~19 |
| **Total** | **~126** |

Modulation buses do not need clearing: the first writer to each bus uses `=`
instead of `+=`, resolved at note-on when the routing is compiled.

### 3.4 Headline number

**Measured, #43** (`history_fm.md` §"FM P0 Measurement (#43)", `breadboard_rp2350`,
2026-08-08): raw 6-operator kernel cost is **100.05 c/f/voice** (linear fit,
flat 2–24 voices, 7.79 c/f fixed overhead), against a corrected available
budget of **2607 c/f** (85% × 3401 − 15 idle − 268.7 measured FX reserve, §9).
P0's rig excludes EG/LFO/pitch-EG by design (§1) — the ~19 c/f/voice
control-rate line item below is still §3.3's estimate, not measured, pending
a P2 bench pass once `EnvDX`/LFO exist:

| | Cycles/sample/voice | Voices |
|---|---|---|
| Available (85% utilisation − idle − measured FX reserve) | 2607 | — |
| **Measured, kernel only (P0)** | **100.5** | **25** |
| Projected total (measured kernel + §3.3's unmeasured ~19 c/f EG/LFO) | ~120 | **21** |

**Decision: `MAX_VOICES = 16`, confirmed** (not raised, despite the
projection clearing the ≤130 "20+" tier below) — the projection leans on an
unverified P2 estimate, not a bench reading, so this document doesn't spend
the apparent surplus yet. 16 voices costs 1,608 c/f of the 2607 available, a
comfortable ~27% margin under even the conservative 120 c/f/voice
projection. Revisit raising `MAX_VOICES` once P2 measures the real EG/LFO
control-rate cost — the surplus P0 came back with is real, it's just not
spent on a projection.

**Measured, #45** (`breadboard_rp2350`, real `EnvDX`, 16 voices, no FX):
**65% Core 1 duty cycle steady-state, 73% under intensive re-triggering.**
Working back through the same arithmetic as above (65%/73% × 3401 − 15
idle, ÷16 voices) gives **~137 c/f/voice steady, ~154 c/f/voice under
bursty re-triggering** — this §3.3 estimate's "~19 c/f/voice" control-rate
line item was low: the real EG overhead (full voice minus #43/#44's
100.05 c/f kernel-only baseline) is **~37-54 c/f/voice**, roughly 2-3x the
original guess, not the LFO/pitch-EG that don't exist yet (P4) but purely
`EnvDX`'s per-block stepping (env_dx_step_block() plus the exp2 conversion,
6 operators/voice). Still comfortably inside the ≤130→"20+" / ~160→"16,
proceed as planned" sensitivity tiers below (137 sits just above the ≤130
cutoff, confirming 16 was the right call, not a conservative one) — no
change to `MAX_VOICES`. See `history_fm.md` §"FM Engine — EnvDX + BLOCK
Confirmation (#45)" for the full number and what it means for the
BLOCK=8-vs-16 tradeoff (it strengthens the case for keeping 16, since
BLOCK=8 would double this now-larger real overhead, not the original
~19 c/f guess).

Sensitivity (as originally written, kept for the record):

- **≤130 cycles/voice measured** → 20+ voices; consider 6-op as the unconditional
  default and revisit X2 operator waveforms.
- **~160 cycles/voice** → 16 voices. Proceed as planned.
- **≥200 cycles/voice** → 12 voices. Make 4-op the default patch shape with 6-op
  as an opt-in, and prioritise §3.6 tuning before P2.

P0's measured 100.5 c/f/voice is so far under all three tiers (even the
projected ~120 c/f total clears the ≤130 tier) that the sensitivity table's
main job — deciding whether to retreat to 12 or 4-op-default — turned out
not to be needed. The tiers stay as-written for P2's use, since that's when
the number they were meant to gate (the full voice cost, EG/LFO included)
actually gets measured.

This is Core 1 only. Splitting voices across both cores would roughly double the
count, but it conflicts with the established Core 0 = I/O / Core 1 = DSP split and
would put audio rendering in contention with the LCD DMA and MIDI polling. Not
proposed.

### 3.5 Why there is no interpolation

Linear interpolation costs ~45% per operator (19 instructions vs 13). Instead:

**4096-entry table, `phase >> 20`, no interpolation.** Identical instruction
count to the 1024-entry no-interp version. 12-bit phase resolution is exactly what
the DX7 hardware used, giving a truncation spur floor around −72 dBc — below
anything audible under FM's own harmonic density, and well inside the platform's
stated lo-fi remit. Cost: 8 KB of SRAM.

The existing `osc/sine.*` 1024-entry interpolating lookup stays as-is for the
other engines; FM gets its own table. (If it proves convenient, the shared table
could be widened to 4096 and the interpolating path would get free extra accuracy,
but that is not a dependency.)

### 3.6 Tuning levers, in expected order of value

Measured #43 (`history_fm.md` §"FM P0 Measurement (#43)"), decisions in §3.4:

1. **Two operators interleaved in one loop body.** ~~Likely the single
   largest win.~~ **Measured: −1.5%, not the largest win.** Plausible cause:
   GCC's own scheduler already exploits most of the independent-load-use
   slack once both calls inline into the render loop, ahead of the explicit
   software pairing. **Decision: not adopted** — too small a win for the
   added kernel-selection complexity.
2. **`__not_in_flash_func` on the kernel and the sine table in SRAM.**
   ~~Non-negotiable.~~ First pass **measured 0% — the lever didn't actually
   engage** (the attribute targets functions that fully inline away before
   linking, so there's no separate symbol left to place; confirmed via
   `objdump`, identical `.text` either way — genuinely always flash). Fixed
   in `rig.h` using the pico-sdk's `__no_inline_not_in_flash_func`, and
   re-measured (tests 14/15, `history_fm.md` §"FM P0 Measurement (#43)"):
   **SRAM is +4.9 c/f/voice *worse* than flash**, isolated from the noinline
   call overhead via a flash-only control — backwards from this item's
   "non-negotiable" assumption. Confirmed by evidence, not asserted: the
   SRAM build has three linker-generated veneer stubs the flash build
   doesn't, because a call from the flash-resident render loop into
   SRAM-placed code crosses a ~256 MB gap outside a Thumb `BL`'s range.
   RP2350's XIP cache already erases most of flash's latency disadvantage
   for a small loop reused every sub-block, leaving the veneer indirection
   as a pure cost with nothing to offset it. **Decision: keep the kernel
   inlined in flash** — beats both noinline variants outright regardless of
   placement, so this isn't even close. **Caveat (open question 10):**
   measured with Core0 doing essentially no flash-side work; RP2350's XIP
   cache is shared by both cores, so this margin isn't guaranteed once Core0
   has real LCD/MIDI/control traffic competing for the same 16 KB.
3. **Block size.** ~~BLOCK=16 balances per-block overhead against EG time
   resolution.~~ **Measured (#43, kernel-only): BLOCK=8 is 4.9% cheaper,
   BLOCK=32 is 10.8% more expensive — inverted from the amortisation story,
   because that rig has no EG/LFO to amortise. The real driver is GCC's
   loop-unrolling threshold** (confirmed: `audio_engine_run()` compiles to
   5,568 bytes at BLOCK=16 vs. 1,336 at BLOCK=32, and BLOCK=32 alone
   compiles the per-operator loop as a real branch). **Closed, #45:
   BLOCK=16 confirmed (not changed)** — see `history_fm.md` §"FM Engine —
   EnvDX + BLOCK Confirmation (#45)" for the host-rendered rate-99 attack-transient
   comparison. BLOCK=32 loses on both axes now measured (10.8% more
   expensive kernel *and* the coarsest, least accurate attack transient: 9
   steps and an 8% timing overshoot vs. BLOCK=8's 34 steps and BLOCK=16's
   17); BLOCK=8 wins narrowly on both individually, but doubles how often
   the new per-block EG step runs (env_dx.h: a handful of table lookups
   plus one 64-bit divide per operator, absent from #43's kernel-only rig),
   an unmeasured real cost with no hardware bench to weigh it against
   BLOCK=8's kernel savings. Kept the already-characterised BLOCK=16 rather
   than trade a measured kernel win for an unmeasured control-rate loss —
   revisit with a real profiling-pin reading if BLOCK=8's EG overhead
   turns out smaller than projected.
4. **M33 DSP extension.** `smulwb` fuses the `mul` + `asr` pair. **Measured:
   −3.0%, as predicted. Adopted where convenient** — real, no correctness
   cost (host + device verified in #42).
5. **SIO interpolators.** Not tried — this document's own prediction ("probably not
   worth it with a non-interpolating lookup") held up well enough by the
   other levers' small margins that this wasn't worth #43's bench time.

---

## 8. Memory budget

| Item | Size | Location |
|---|---|---|
| Operator sine table (4096 × int16) | 8 KB | SRAM |
| Exp2 table for EG (256 × int16) | 0.5 KB | SRAM |
| Patch bank (32 × ~240 B, runtime form) | ~7.5 KB | Flash (read-only) — **measured, #47**: ROM1A's 28 converted patches were 5488 B (196 B/patch) pre-#48; **#48**: all 32/32 convert (fixed-frequency support), 220 B/patch; **updated, #49**: FmOpParams gained `am_sensitivity` (+1 B/op) and FmPatch gained voice-wide `lfo`/`pitch_eg` structs (+~16 B/patch, once per patch not per op) — 7744 B total (242 B/patch), estimate per `tools/syx2patch.py`'s own printed flash-cost line, not a compiled `sizeof()` |
| Per-voice state (16 × ~200 B, `FmOp[6]`) + pitch EG/LFO (16 × ~24 B, one each per voice) | ~3.6 KB | SRAM |
| Shared bus scratch (7 × 16 × int32) | 448 B | SRAM |
| Mix scratch | reuses existing `scratch[]` | SRAM |
| **Total SRAM** | **~12.5 KB** | of 520 KB |

**No PSRAM. No streaming. No dynamic allocation.** This should be treated as a
design invariant of the module.

---

## 9. CPU budget summary

| Item | Cycles/sample | % of 3401 |
|---|---|---|
| Idle / DMA / IPC (measured) | ~15 | 0.44% |
| Global FX insert (reverb — measured, reused from the subtractive engine's identical shared code, `history_fm.md` §"FM P0 Measurement (#43)") | 268.7 | 7.9% |
| 16 × 6-op voice @ 100.5 measured kernel + ~19 unmeasured EG/LFO (P2) ≈ 120 | ~1920 | 56.5% |
| **Total** | **~2204** | **~65%** |

Both line items are now real measurements, not reservations: the reverb
figure reuses `fx/delay.h`/`fx/reverb.h`'s already-measured subtractive-engine
deltas (unchanged, engine-agnostic, global post-mix code — the reservation
was low by about 1.8×), and the voice figure is #43's bench-measured
100.5 c/f/voice kernel cost, still carrying §3.3's original ~19 c/f/voice
EG/LFO estimate forward unmeasured (P0's rig has no EG/LFO by design; that
piece is P2's to measure). Total Core 1 load at 16 voices + reverb, using
the best current numbers, is well under the budget this table originally
targeted — see `history_fm.md` §"FM P0 Measurement (#43)" for the full
derivation and the decision not to raise `MAX_VOICES` on this projection
alone.

---

## 10. The other option: FM in the generic subtractive engine

Independent of everything above, and worth doing on its own merits.

Add `WAVE_FM2` (and optionally `WAVE_FM4`) to the shared oscillator set:

- **State:** one extra phase accumulator per voice, mirroring the existing
  `voice_phase[]` array in `engines/subtractive/audio_engine.cpp`.
- **Params:** ratio (modulator = carrier × ratio) and index (modulation depth),
  plus an index envelope.
- **Cost:** ~17 cycles for the modulator + ~20 for a decay envelope on the index,
  on top of the existing ~201 → roughly **+20%**, dropping 16 voices to ~13.

**What it gets you:** DX-flavoured bells, electric pianos and basses running
through the ladder filter and the existing effects chain — a sound neither current
engine makes, and one that combines well with subtractive processing.

**What it does not get you:** DX7 character, which comes from six *independently
enveloped* operators. Two operators sharing one index envelope is closer to a
Casio CZ than a DX7. This is a complementary sound source, not a substitute for §1–9.

**Implementation note:** the index envelope is the crux. A static index gives a
flat, lifeless timbre; FM's characteristic evolving brightness requires the index
to decay. The `aux_env` slot in the groovebox `VoiceParams` is the pattern to
follow — the subtractive engine needs an equivalent second envelope, which is also
independently useful for filter modulation.

This can be built at any time and shares no code with the FM module beyond
possibly the 4096-entry table.

---

## 12. Open questions / decisions to make

| # | Question | When |
|---|---|---|
| 1 | ~~Measured cycles/operator, and therefore the real voice count~~ **Closed, #43: 100.05 c/f/voice measured (kernel only), `MAX_VOICES=16` confirmed** — `history_fm.md` §"FM P0 Measurement (#43)". Raising past 16 deferred to a P2 bench pass once EG/LFO exist to measure. | **P0 — done** |
| 2 | ~~FX insert cost in isolation; is Freeverb worth ~4% in an FM context?~~ **Closed, #43: 268.7 c/f / 7.9% (reused from the subtractive engine's identical shared FX code), not ~4%. Freeverb stays** — the 16-voice budget clears with ~27% margin even at the corrected cost. | **P0 — done** |
| 3 | ~~BLOCK size — 16 assumed; confirm against rate-99 attacks~~ **Closed, #45: BLOCK=16 confirmed, not raised to 32 or lowered to 8** — `history_fm.md` §"FM Engine — EnvDX + BLOCK Confirmation (#45)". | **P2 — done** |
| 4 | ~~Extract a shared `BlockClock` for FM and speech, or keep them separate?~~ **Closed, #46: reject — common pattern, no common code.** Compared the real `EnvDX` (§5.3) against speech's segment sequencer: per-operator vs. per-voice instancing, fixed 4-stage log2 vs. variable-length float segments, exact per-sample `gain_step` interpolation vs. per-sub-block-constant IIR smoothing. See §5.3's cross-module note and `architecture.md` "Settled Decisions". Speech's sequencer (#34/#36/#37) untouched. | **P2 — done** |
| 5 | ~~Global vs. per-voice LFO default~~ **Closed, #49: per-voice, global-phase mode dropped (not built), by design.** #48's multitimbrality (one patch pointer per voice) leaves a literal shared LFO with no principled behavior once two active voices with *different* patches both request global phase — a case that can't arise on real single-timbral hardware, so there's no real DX7 behavior to match. Per-voice-with-key-sync already covers the common "block chord" fidelity case. See §5.5's own writeup. | **P4 — done** |
| 6 | Algorithms 4 and 6: interleaved fallback (X1) or documented limitation? **Partially closed, #47**: v1 documents the limitation and applies a collapse-to-single-self-feedback fallback (detected generically via cycle analysis, logged, never silent) — real X1 interleaved rendering (the actual two-operator loop) is `#54`, still open. | P4 |
| 7 | Patch bank source — ship a curated set, or make `.syx` loading a runtime feature over MIDI SysEx? | P3 |
| 8 | Does per-voice multitimbrality warrant a MIDI channel→patch mapping UI on the LCD? | P5 |
| 9 | X2 operator waveforms — only if P0 leaves headroom | P6 |
| 10 | **Core0/Core1 XIP cache contention.** #43's "keep the kernel in flash" decision (§3.6 item 2) was measured with Core0 doing essentially no flash-side work — MIDI/LCD/control are still stubs (#41/#42). RP2350 has one 16 KB XIP cache shared by *both* cores (`hardware_xip_cache.h`); once Core0 does real LCD/MIDI/control work, its flash traffic can evict the FM kernel's cache lines, right when Core0 is busiest — a risk #43 didn't test and can't yet, since there's no real Core0 workload to contend against. Mitigation available if it turns out to matter: `xip_cache_pin_range()` (RP2350-only) permanently reserves the kernel's flash range against eviction by anything else, keeping flash's speed without the exposure. If pinning doesn't pan out, SRAM's #43 "measured worse" verdict was itself measured in isolation — SRAM sidesteps this specific shared-cache problem entirely (its own contention risk is per-bank and controllable), so it's a fallback, not dead. | **P1+, once Core0 has a real workload to bench against** |

---

## 11. Recommended build order

1. **P0 — measure.** ~150 lines. 16 voices × 6 ops, fixed increments and gains,
   no EG, no patch logic. Scope GPIO 22. In the same session, measure:
   flash vs. `__not_in_flash_func`; 1024 vs. 4096 table; interleaved vs. plain
   kernel; BLOCK = 8/16/32; and the FX insert in isolation.
2. ~~Record the results in `history_fm.md`'s performance table before writing P1.
   Update §3.4 of this document with measured figures and strike the
   provenance caveat.~~ **Done, #43.**
3. ~~**P1** — engine skeleton with one hardcoded patch. Verify ratios and routing
   by ear against Dexed on the same algorithm.~~ **Implemented, #44** (routing
   compiler, one hardcoded patch, MIDI note on/off, host-verified;
   `breadboard_rp2350`-measured ~93-95 c/f/voice at 16 voices, at or below
   #43's kernel-only baseline). The by-ear-against-Dexed half is still
   Carl's to do.
4. ~~**P2** — `EnvDX`. Confirm BLOCK choice against fastest-attack patches.~~
   **Implemented, #45** (4-stage log-domain EG, DX7 level table, velocity
   sensitivity, BLOCK=16 confirmed — `history_fm.md` §"FM Engine — EnvDX + BLOCK Confirmation
   (#45)"). By-ear EP/bell check on real hardware still Carl's to do.
5. ~~**P3** — the converter.~~ **Implemented, #47 (v1) + #48 (v2)**
   (`tools/syx2patch.py`: 32-voice .syx unpack, all 32 algorithms mapped to
   `mod_target`/`feedback` via one generic bus-simulation decode, algorithm
   4/6 interleaved-feedback detected via cycle analysis and collapsed to
   single self-feedback with a logged warning, EG rates/levels/output
   level/velocity sensitivity copied straight through since env_dx.h
   already converts them at runtime, coarse/fine ratio computed, per-
   carrier/per-modulator level scaled down by fan-in to avoid int32
   overflow on multi-carrier/multi-modulator algorithms — real bugs caught
   by host-rendering the actual bank, not assumed). #48 added key level
   scaling and rate scaling (both ported from Dexed's real `ScaleLevel`/
   `ScaleRate`, resolved at note-on from a new `VoiceParams::note` field),
   detune (a fixed ±7-cents approximation), and fixed-frequency mode (a
   closed-form Hz formula also derived from Dexed's `osc_freq()`) — see
   §5.6's own writeup. All 32/32 of ROM1A now convert and host-render clean
   (was 28/32 before #48; the 4 previously-skipped fixed-frequency patches,
   "TUB BELLS"/"STEEL DRUM"/"REFS WHISL"/"TRAIN", all convert now). Pitch
   EG, LFO, and transpose remain unwired — no `pitch_eg.h`/`lfo.h` exists
   yet at all, that's P4. The `.syx` input and generated `patches.h` are
   both gitignored (Yamaha's own patch data, same policy `xm2t00t`'s `xm/`
   already established for copyrighted third-party content) — see §7 above
   for the full writeup and usage, and `history_fm.md`'s "syx2patch Host
   Converter (#47)" section for the bug-discovery narrative and validation
   results.
6. ~~**P4** — pitch EG + LFO.~~ **Implemented, #49** (`pitch_eg.h`/`lfo.h`:
   4-stage pitch EG in cents, LFO with all six waveforms/rate/delay/PMD/AMD/
   key-sync/PMS/per-op AM sensitivity, both ported from Dexed's real
   `PitchEnv`/`Lfo` and cross-checked via a calibration harness the same way
   #58/#59 verified `env_dx.h`'s tables; `tools/syx2patch.py` v3 bakes both
   from the `.syx`'s bulk offset 102-109/112-116). Closes open question 5:
   per-voice LFO, global-phase mode considered and dropped (see §5.5's own
   writeup for why). Zero per-sample cost confirmed against device
   disassembly (48 `smlawb` instances, unchanged). Hardware listen still
   Carl's to do — see §5.5's note on `mod_wheel` defaulting to 0 (no
   vibrato/tremolo until the wheel moves, by design, not a bug).
7. **P5–P6** — free routing UI, calibration against Dexed.

`WAVE_FM2` (§10) can be slotted in anywhere; it does not gate anything.

---

## 13. Summary — the new-code list

**New files:**
`engines/fm/{engine.h, engine.cpp, op.h, env_dx.h, pitch_eg.h, lfo.h, sine_tab.h, patch.h, patches.h, presets.h, display.cpp}`, `tools/syx2patch.py`.

**Modified:**
`voice_alloc.*` (operator-cost weight), `CMakeLists.txt` (engine selection),
`history_fm.md` (P0 measurements), `wslcd/display.cpp` (operator budget readout).

**Unchanged:** IPC, MIDI, output, effects, LCD driver.

**Headline expectation:** 16 voices of full 6-operator, freely-routed,
per-voice-multitimbral FM, in ~12 KB of SRAM, with real DX7 banks loading via a
host converter — pending P0.
