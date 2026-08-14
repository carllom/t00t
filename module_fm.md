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

> **A second rewrite, after P0–P4 shipped.** Everything below through §9
> describes the design as originally built (issues #41–#59: the P0–P4 phases
> in the table below, plus #57/#58/#59's hand-fit modulation/level/rate
> curve corrections). A later Dexed-conformance evaluation — build a
> reference harness, diff against Dexed's own source control-plane-exact and
> spectrally, phase by phase — found that build still had several real
> defects (an uncalibrated modulation-index scale, a hand-derived envelope
> curve family instead of Dexed's real exponential-attack shape, feedback
> depth 2× too hot, an LFO phase-origin bug that inverted three of six
> waveforms, and a converter bug silently dropping two-thirds of the
> modulation on 10% of factory patches) and replaced the numeric core to fix
> them. That work — phases F0 through F8 — is fully documented in
> `history_fm.md` §§1–7 and is what the engine actually ships today; the
> sections below have been updated to describe the current (post-F2–F7)
> implementation, with pointers to the relevant `history_fm.md` phase where
> the detail matters. Bank-wide error against Dexed dropped from a mean
> 18.9 dB harmonic MAE at the F0 baseline to 0.53 dB at F7, across all 256
> factory voices on four ROM banks — see `history_fm.md` §"The regression
> gate (§6, delivered)" for the committed thresholds.

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
  engine.h            ← VoiceParams, VoiceParamBlock, ParamExchange
  audio_engine.cpp    ← audio_engine_run(): render pass, voice loop, FX insert
                         (also holds the #42 P0 profiling rig behind T00T_FM_PROFILE)
  op.h                ← FmOp + the three kernels, note-on/block/voice glue
  fm_scale.h          ← the F2 fixed-point contract (FM_CYCLE/FM_GAIN_MAX/
                         FM_MOD_SHIFT/FM_VOICE_OUT_SHIFT) — the single anchor
                         op.h/env_dx.h/lfo.h all derive their scale from
  env_dx.h            ← the DX7 envelope, a direct port of Dexed's Env (F3)
  pitch_eg.h          ← voice-wide pitch EG
  lfo.h               ← voice-wide LFO
  sine_tab.h
  patch.h             ← FmPatch / FmOpParams (runtime form) + the note-on
                         routing compiler (fm_resolve_routing())
  patches.h           ← GENERATED by tools/syx2patch.py — do not hand-edit
  rig.h               ← #42's standalone P0 measurement rig (no patch/EG/LFO)
  render.h            ← fm_render_test_tone(), shared by #41's skeleton and
                         the host build
  midi_controller.cpp ← note on/off, bend, pan, mod wheel, patch select
  display.cpp         ← FM-specific LCD panel (stub — deferred, §6.4/open
                         question 8)
```

There is no `presets.h`/`VoicePreset` here — FM's whole timbre is the single
`FmPatch` pointer in `VoiceParams` (§6.3), so it never needed the shared
preset-table shape speech/chip use for their per-voice-type parameter
tweaks; `midi_controller.cpp` is its own file (not the shared
`src/midi/midi_controller.cpp`) for exactly that reason (§6.2).

### 6.2 Prerequisite

None beyond what `groovebox` already required. `engine_base.h` provides
`MAX_VOICES`, `PROFILE_PIN`, `EffectParams`. If the `VoiceNoteBase` refactor
sketched in `architecture.md` lands first, FM should use it; if not, FM declares
the same four fields inline as the other engines currently do.

### 6.3 `VoiceParams` — small, with a patch pointer

```cpp
struct VoiceParams {
    uint32_t phase_inc;      // base pitch, already bend-scaled (Core 0 computes this)
    int16_t  amplitude;      // velocity 0..32767
    uint8_t  trigger;        // generation counter
    bool     gate;
    int16_t  pan;            // Q15 pan, CC10
    const FmPatch *patch;    // ← the whole timbre, one pointer
    uint8_t  note;           // raw MIDI note 0-127 (#48) — key level/rate scaling
                              // need the actual note, which bend-scaled phase_inc
                              // alone can't reconstruct
    int16_t  mod_wheel;      // Q15, CC1 (#49) → LFO PMD/AMD depth
};
```

There is no separate `bend` field: pitch bend is folded into `phase_inc` by Core 0
before it ever reaches Core 1 (`midi_controller.cpp`'s `bend_to_ratio()`/
`apply_channel_bend()`), the same convention the other engines use — `note` exists
alongside it only because key level/rate scaling need the *unbent* note number,
which a bend-scaled frequency can't be inverted back into.

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

This covers **30 of the 32 DX7 algorithms**. Only algorithms 4 and 6 have a
*second* operator whose feedback-out bit is set; every other algorithm wraps
feedback around a single operator.

**Resolved, not just deferred — X1 is not needed.** `tools/syx2patch.py`'s
`decode_algorithm()` detects the potential second loop generically (a tentative
edge from the secondary feedback operator back to the primary, tested for a real
cycle) and — as originally planned — never emits that edge, logging a
`needs_interleaved` note instead (patch.h's `fm_resolve_routing()` never sees a
multi-operator cycle from these two algorithms as a result). What the
Dexed-conformance evaluation added (`history_fm.md` §5.9, "F4 — feedback, and the
algorithms 4/6 question"): dumping Dexed's own algorithm table shows the
secondary operator has `fb_out` set but **not** `fb_in`, and `fm_core.cc` gates
its feedback kernel on both bits together — **Dexed does not implement the
second loop either.** Measured on the seven ROM patches across four banks that
use algorithm 4 or 6, this engine's collapse-to-single-feedback matches Dexed to
0.1–3.9 dB harmonic MAE, in the same range as the rest of the bank. Dropping the
second loop is matching the reference exactly, not approximating it — the
caveat is that this makes the engine match *Dexed*, not necessarily real DX7
silicon, which is out of reach to verify directly. X1 and open question 6 below
are closed as "not needed" on that basis.

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
    uint32_t inc;           // this block's actual increment (base_inc × bend/EG/LFO ratio)
    uint32_t base_inc;      // neutral-pitch increment, resolved once at note-on (F5)
    int32_t  gain;          // linear, current — FM_GAIN_MAX (fm_scale.h) at full scale
    int32_t  gain_step;     // per-sample delta for this block
    const int32_t *in;      // modulation bus (points at a zero bus for pure carriers)
    int32_t *out;            // output bus (or the shared voice mix bus)
    int32_t  fb1, fb2;       // op_render_fb only: last two post-gain outputs
    EnvDX    eg;              // this operator's own 4-stage envelope (§5.3)
};
```

Three kernel variants, selected per operator at note-on:
`op_render()` (plain), `op_render_fb()` (self-feedback, 2-sample average),
`op_render_first()` (stores rather than accumulates — eliminates bus clearing).

The `in`/`out` pointers and the kernel function pointer, plus the processing
order, constitute the entire routing implementation.

**The fixed-point contract (`fm/fm_scale.h`, new file since the F2 rewrite —
`history_fm.md` §2/§5.6).** The kernel originally had no calibrated relationship
between an operator's `gain`/output magnitude and how much phase deviation
that produces on whatever it modulates — carriers and modulators were scaled
by two independently hand-tuned constants (`FM_OUT_SHIFT_CARRIER`/
`FM_OUT_SHIFT_MODULATOR`), plus a further `FM_MOD_INPUT_SHIFT` fudge on the
modulation input, plus a per-operator hand-tuned reference gain
(`FmOpParams::level`, since removed — §7). None of the three constants had a
principled value; they existed to cancel each other out, and a Dexed-diff
harness measured the result as per-patch level errors of −12 to −96 dB and
brightness from 0.30× to 6.31× *on the same build* (`history_fm.md` §5.1). All
of that is gone. There is now one anchor, measured from Dexed rather than
chosen: a unity-gain operator produces exactly one full cycle of phase
deviation, and a max-level one exactly two — Dexed's own real ceiling. In this
engine that is `FM_CYCLE = 2²⁶` (one cycle, in bus units) and
`FM_GAIN_MAX = 2²⁸` (derived from it, not chosen); `fm_mul_gain()`'s output
*is* phase deviation whether the operator is a carrier or a modulator, and a
carrier's `out[]` is simply read as audio (`FM_VOICE_OUT_SHIFT`, applied once
in `fm_render_voice()`) instead of as phase. Nothing outside `fm_scale.h` is
allowed an opinion about absolute level any more — if a patch is too loud or
too dim, the fix is in its DX7 output level, EG, or key scaling, exactly as it
would be on real hardware.

**Self-feedback depth was 2× too hot, found and fixed by comparing against
Dexed's real `compute_fb`.** Two bugs, both in `op_render_fb()`: (1)
self-feedback used to be no-op-or-full (a bool) rather than the DX7's real
64× depth range across levels 1–7 — fixed by `FmRouting::fb_shift`
(`8 - feedback_level`, resolved at note-on from `FmOpParams::feedback_level`,
now 0–7 DX7 units, not a bool); (2) the feedback history (`fb1`/`fb2`) stored
the *raw* table lookup instead of the value after the gain multiply, so a
decaying operator's feedback buzz never faded with its own envelope — fixed
by storing the post-gain value, matching Dexed's `fb_buf`. Once both were in
place, a control-plane diff against Dexed's `compute_fb` found the shift
itself was still exactly 2× too deep at every level (`>> fb_shift` where
Dexed uses `>> (fb_shift + 1)`) — an octave of extra feedback on every
patch's edge, which reads as generalized excess brightness rather than a
feedback bug, exactly why it went unnoticed by ear. Fixed; five of the seven
brightest patches in the reference bank turned out to be feedback-heavy and
moved substantially once corrected (SYN-LEAD 1: 24.3 dB harmonic error →
0.6 dB). See `history_fm.md` §5.9 ("F4 — feedback, and the algorithms 4/6
question") for the full measurement.

**Modulator fan-out (F7, `history_fm.md` §5.20) — the largest remaining defect
found in the whole evaluation.** `FmOpParams` gained `extra_target_mask`: a
bitmask of *additional* operators a modulator also feeds, beyond its primary
`mod_target`. DX7 algorithms 19–25 have one modulator driving two or three
carriers at once; the routing resolver used to treat a bus as emptied by its
first reader and silently dropped every subsequent one — on 25 of the 256
factory ROM voices (10%), leaving two of three carriers on some patches
completely unmodulated. `fm_voice_render_block()` itself needed no change
(`in_bus`/`out_bus` were already independent per operator); only
`patch.h`'s `fm_resolve_routing()` and `tools/syx2patch.py`'s
`decode_algorithm()` needed to stop clearing a bus on read. Fixed the way it
was found: a new control-plane test (`table/routing`) that reconstructs the
real modulation *graph* rather than just diffing algorithm bytes — the old
`table/algorithms` case had compared byte-identical (and therefore
"passing") input to a decoder that silently discarded most of it.

### 5.3 `fm/env_dx.h` — 4-stage DX7 envelope

**Do not reuse `envelope.*`.** The DX7 EG is 4 × (rate, level) pairs operating
linear-in-dB, which is a large part of why its decays sound the way they do. An
ADSR bent into that shape would be both slower and less accurate.

Design (current, since the F3 rewrite — `history_fm.md` §5.7):

- EG state and arithmetic live in the **log domain** (Dexed's own Q24-octave
  `level_` convention, 15-octave usable range), a direct port of Dexed's
  `Env` (`Source/msfa/env.cc`, Apache-2.0) rather than a re-derivation of its
  shape.
- The EG steps **once per control block** (`FM_BLOCK`=16 → 2756 Hz), not per
  sample.
- At each block boundary, the log level is converted to linear via a small
  256-entry exp2 table (no interpolation needed — the 8 fractional bits it
  resolves line up exactly with the table), and the kernel is handed `gain`
  (start) and `gain_step` (delta) for the block.
- Per-sample cost in the kernel: **one add**. Zipper-free.
- **Rising and falling stages are different curve families**, matching real
  DX7 hardware: a rising stage jumps to a fixed floor (`jumptarget = 1716`)
  and then approaches its target *exponentially*; a falling stage ramps
  *linearly* in the same log domain. A single symmetric ramp — this
  section's original design — cannot produce a genuinely instant attack: it
  has to traverse the whole log-domain floor first, which a control-plane
  diff against Dexed measured at 16.3 ms for a rate-99 "instant" attack
  against Dexed's 0.0 ms (`history_fm.md` §5.4).
- **Output level (TL) is folded into the envelope's own target, not added
  afterwards.** `env_dx_advance()` composes it with a bias and a floor
  clamp when it resolves each stage's target level — carrying it alongside
  as a separate offset (the original design) put every sustain stage about
  60 dB lower than intended, since the two compose non-linearly.
- A same-level stage still takes real time on hardware (Dexed's
  `staticcount`/`ACCURATE_ENVELOPE` dwell table) rather than completing
  instantly, and the slowest rates are not capped by an artificial floor —
  the smallest possible increment spans the full 15-octave range in about
  six minutes, which the current formulation reaches without a special case.

Block size is chosen by EG time resolution, not by the operator kernel.
`FM_BLOCK`=16 (0.36 ms/block) is confirmed, not raised or lowered — see §3.6
item 3 and `history_fm.md` §"FM Engine — EnvDX + BLOCK Confirmation (#45)".

**Both curve tables (level and rate) are ported from Dexed, not hand-fit.**
The original design (#45) shipped both a level curve and a rate curve as
unverified shape guesses — a quadratic-in-dB level fit and a smooth
single-exponential rate fit — and both were measurably wrong once real DX7
patch data and a control-plane diff against Dexed existed to check them
against: the level fit ran 6–15 dB hotter than real DX7 across most of the
0–99 range, and the rate fit was up to ~23× too slow at the fast end (a
"rate 99 instant attack" landing roughly 20× slower than real hardware).
Both are now ported wholesale from Dexed's real code rather than re-fit —
`dx7_scaleoutlevel()` (Dexed's `Env::scaleoutlevel()`, a measured lookup
table for levels 0–19 and `28 + level` above that) and the piecewise
`qrate`-based rate derivation in `env_dx_advance()`
(`(4+(qrate&3)) << (2+6+(qrate>>2))`, not a single exponential — real DX7
rate accelerates far more steeply toward the fast end than any smooth curve
can express). Both are verified byte-identical to Dexed's own tables by a
committed control-plane conformance suite (`tools/fm_ctl_diff.py`), not
merely "close." `tools/host_render`'s own EG-shape checkpoints were
recalibrated to the new, correct speed alongside the rate-table replacement.

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

**Confirmed correct, not just implemented.** Unlike `env_dx.h` and `lfo.h`
(§5.3/§5.5), a control-plane diff against Dexed's real `PitchEnv` found this
file's tables and combination logic already exact on first measurement
(`history_fm.md` §5.5) — no rewrite was needed here.

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
50.9 Hz.

**Three of six waveforms were half a cycle out of phase, and the delay ramp
was wrong in shape — both found by a control-plane diff against Dexed, both
fixed (`history_fm.md` §5.14).** Dexed's key-sync point is the *middle* of
the cycle (`phase_ = (1<<31) - 1`), not the start, and its two sawtooth
cases carry a compensating half-cycle rotation to match. The original
`lfo.h` synced to phase 0 and wrote the sawtooths unrotated — the two
omissions cancel exactly for the sawtooths (which is why they measured
correct) and for nothing else, so triangle, square and sine each came out
precisely inverted. Both halves of the pair are now restored
(`fm_lfo_trigger()`'s sync point and each saw case's rotation), and all six
waveforms are conformance-exact against Dexed. Separately, the delay ramp
(`dx7_lfo_delay_incs()`) used to collapse Dexed's real two-stage accumulator
into a single ramp spanning only the accumulator's *silent* first stage —
documented at the time as "a deliberate simplification," but the real first
stage is not a ramp at all (`getdelay()` returns exactly 0 throughout it),
so the old code started opening while the reference was still fully closed.
Now ported as the real two-stage integer accumulator: delay 0 → instant,
delay 50 → 0.31 s closed + 0.08 s opening, delay 99 → 2.66 s closed + 0.67 s
opening.

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
mod-matrix feature) is **not** replicated — the simpler, extremely common
convention `lfo.h` implements instead is what stayed: mod wheel is a
separate modulation source, hardwired to both pitch and amp, matching
speech's own CC1 "mod wheel → vibrato depth" precedent (#36).

**The mod-wheel/patch-depth relationship was wrong, and is now reversed to
match Dexed.** The original design made the wheel a 0..1 *multiplier* on the
patch's own configured PMD/AMD, with the consequence flagged honestly at the
time: "a patch with real vibrato/tremolo configured will sound completely
flat until the mod wheel is actually moved." Measured against the
reference, that consequence is a real bug, not expected behavior — every
factory patch with configured vibrato played with no vibrato at the wheel's
resting position (0), which is silent by default on every MIDI controller.
It was invisible to every scorecard run before this was checked, because
the render harness defaults to wheel 0 and both sides looked equally quiet.
Fixed to follow Dexed's real rule instead: the wheel is a **separate**
modulation source that competes with the patch's own depth via `max()`, not
a multiplier on it — a patch's configured vibrato/tremolo always plays at
its own depth, and pushing the wheel up increases it further from there.
"Mod wheel scales LFO depth" still holds (pushing the wheel up still
audibly increases the effect), just as a superset of the old behavior
rather than the only source of it.

`VoiceParams` has `mod_wheel` (Q15, `engine.h`) — §6.3's struct listing.
Host-verified (`render_fm`'s `run_pitch_eg_check()`/`run_lfo_check()`):
pitch EG's L4-start/attack-blip/settle/release-swoop shape and its
per-operator increment scaling (moves a ratio operator, exempts a
fixed-frequency one — matching Dexed's own `osc_freq()` fixed-mode branch,
which only ever receives pitch bend, never pitch/LFO mod); all six LFO
waveform shapes (now conformance-exact); rate/delay calibration anchors; a
real block-rate pitch/amp swing near the calibrated max; `mod_wheel=0`
leaving each patch's own configured depth in effect (not silencing it, per
the `max()` fix above); and AM reaching `fm_voice_step_envelopes()`'s real
gain output, weighted by each operator's own `am_sensitivity` and leaving
`am_sensitivity=0` operators untouched. Kernel disassembly unaffected (still
48 `smlawb` instances, device build) — everything here is note-on/block-rate,
never inside the per-sample loop, confirming the "zero per-sample cost"
constraint this section opens with. `tools/syx2patch.py` bakes the
voice-wide pitch EG (bulk offset 102-109) and LFO (offset 112-116) bytes
straight through, same "no host-side DSP math" reasoning as every other
field — offsets and the LKS/LFW/LPMS bit-packing in byte 116 cross-checked
against Dexed's own `Cartridge::unpackProgram` (`Source/PluginData.cpp`,
Apache-2.0).

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
  resolved at note-on into `dx7_note_outlevel()`'s composed `outlevel`,
  combined with TL and velocity *before* the log2 conversion and clamped to
  [0,127] exactly like Dexed's own `outlevel = min(127, outlevel)` — not
  added as a separate, unclamped log2 offset. A boosting curve (DX7 curve
  2/3, "+EXP"/"+LIN") at high depth and an extreme note can otherwise push
  the combined value well past what a reference level was ever meant to
  represent, and `eg_to_gain()`'s shift has no defined behavior for a large
  positive log2 offset — this clamp is what keeps it inside the range that
  function already guarantees is safe. (Also verified byte-identical to
  Dexed's own table across all 55,040 rows by a committed control-plane
  suite, `tools/fm_ctl_diff.py`.)
- **Rate scaling** (`env_dx.h`'s `dx7_scale_rate`): resolved at note-on into
  a qrate *delta* (`EnvDX::rate_scaling`), added to the base rate's own
  qrate at every stage transition (`env_dx_advance()`) — matching Dexed's
  `qrate += rate_scaling_` order exactly, not applied as a post-hoc scale on
  the already-converted octaves/second value (a different, less faithful
  order of operations). Zero-delta is the exact same table-lookup fast path
  the rate table's own conformance check already verified, so this is
  behavior-neutral for every patch that doesn't use rate scaling.
- **Detune**: applied via the Q32 increment path (`op.h`'s
  `fm_op_base_inc()`, from `FmOpParams::detune_offset` — the raw DX7 byte
  0–14 minus 7, i.e. −7..+7, not a baked cents value). Ratio-mode detune is
  genuinely note-dependent on real hardware (Dexed's `detuneRatio`, from
  2.46 cents/unit at C1 down to 0.68 at C6) — a fixed cents-per-unit
  approximation, tried first, measured up to 11.9 cents of error at C1
  against the real formula (a quarter-semitone, audible as the wrong beating
  rate between two operators sharing a ratio, which is the entire point of
  detune). `op.h`'s `dx7_detune_cents_per_unit()` now resolves the real
  note-dependent multiplier once per note-on from the played MIDI note, not
  from the converter — a note-independent, note-on-baking converter
  structurally cannot resolve a note-dependent parameter, which was the
  actual defect. Fixed-frequency-mode detune uses a different, genuinely
  note-independent sharpen-only rule (`DX7_FIXED_DETUNE_CENTS_PER_UNIT`,
  ~0.96 cents/unit) — that one *is* a flat constant on real hardware, so it
  is baked as one.
- **Fixed-frequency mode**: also the Q32 increment path (`FmOpParams::
  fixed_hz`/`fixed_freq`). `tools/syx2patch.py`'s `op_fixed_hz()` reduces
  Dexed's `osc_freq()` fixed-mode branch to a closed form,
  `Hz = 10^(((coarse&3)*100+fine)/100)` — Dexed's own constant `4458616` is
  exactly `(2^24 * log2(10) * 0.01) << 3`, so no need to replicate its
  Q24-log-frequency/`Freqlut` machinery. All 32 of ROM1A convert (the 4
  patches that needed this — "TUB BELLS"/"STEEL DRUM"/"REFS WHISL"/"TRAIN" —
  all use fixed-frequency operators for inharmonic bell/percussion partials,
  exactly the DX7 use case this unblocks).

`VoiceParams` carries a raw MIDI `note` field (0-127, §6.3) — key level/rate
scaling need the actual note, which `phase_inc` alone (already bend-scaled
into a frequency) can't reconstruct. `fm_voice_note_on()`'s signature carries
a `midinote` parameter accordingly. Verification for key/rate scaling and
detune now runs against Dexed directly rather than through a hand-written
host check: `tools/fm_ctl_diff.py`'s table diffs cover key/rate scaling and
velocity exactly (55,040/1,024/1,024 rows respectively), and a dedicated
spectral tool (`tools/fm_freq_diff.py`, since Dexed's `osc_freq()` is a
private method with no table to diff against) covers detune and
fixed-frequency mode by rendering both engines and comparing the spectral
peak in cents — worst-case 0.18 cents after the note-dependent detune fix,
against 11.90 cents before it (§5.6 above). The original hand-written check
this replaced (`render_fm`'s `run_key_rate_scaling_check()`) was removed
once it started asserting internals the envelope rewrite (§5.3) deleted.
Kernel disassembly unaffected (still 48 `smlawb` instances) — everything
here is note-on/block-rate, never inside the per-sample loop, exactly as
this section's own opening claim requires.

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

The converter now resolves, across v1 (#47) + v2 (#48) + v3 (#49), all
implemented and all still current — same v1/v2/v3 split `speechgen.py` used
(#32/#35):

| DX7 source data | Handling |
|---|---|
| Algorithm number (0–31) | `mod_target`/`extra_target_mask` per operator, via one generic bus-flag decode (Dexed's own algorithm table, reconstructed into this engine's shape) applied uniformly to all 32 rows — `order`/`in_bus`/`out_bus`/kernel selection itself is *already* resolved at note-on by `fm_resolve_routing()`, so the converter's only job is picking the right target(s). `extra_target_mask` (F7) exists because a bus is not emptied on read — one modulator can drive two or three carriers at once (algorithms 19–25), and treating a bus as consumed by its first reader silently dropped the rest on 25 of 256 factory voices |
| EG rates/levels (0–99) | Copied straight through as raw bytes — `env_dx.h` already converts these at block-rate, so there is no host-side log-domain math to do |
| Operator output level (0–99), velocity sensitivity (0–7) | Copied straight through, same reasoning. No per-operator reference-gain constant is emitted any more (F2) — see below |
| Coarse/fine ratio | `ratio = (coarse==0 ? 0.5 : coarse) * (1 + fine/100)`, verified against Dexed's `osc_freq()`/`coarsemul[]` |
| Feedback level (0–7) | The real 0–7 depth, not collapsed to a bool — `op.h`'s `op_render_fb()` reproduces DX7's actual 64× depth range across levels 1–7; level 0 is exact, real silence, not an approximation |
| Algorithms 4 and 6 | `needs_interleaved` detected via a real cycle check (not hardcoded algorithm numbers) on a test graph augmenting the ordinary routing with the secondary FB_OUT operator's tentative edge back to the primary — collapsed to single self-feedback, logged, never silent. Resolved as **matching Dexed exactly, not approximating it**: Dexed's own algorithm table shows the second loop was never implemented there either (§4.2) |
| Key level scaling, rate scaling | Baked as per-operator parameters (breakpoint/depths/curves, RS 0-7) rather than resolved numbers — both need the played MIDI note, which patch data alone never has (§5.6) |
| Detune | `op_detune_offset()` passes the raw DX7 byte (0–14, offset to −7..+7) straight through for `op.h` to resolve at note-on against the actual note — a note-independent converter cannot resolve real DX7 detune's note-dependent ratio-mode formula itself (§5.6) |
| Fixed-frequency mode | `op_fixed_hz()`, a closed-form Hz formula derived from Dexed's `osc_freq()` (§5.6) |
| Pitch EG, LFO | Voice-wide `pitch_eg`/`lfo` blocks (bulk offset 102-109/112-116) copied straight through as raw bytes, same reasoning as the EG fields — DX7's own LFO waveform numbering matches `FmLfoParams::waveform` exactly, no remapping needed |
| Voice name | Retained, becomes `FM_PATCH_NAMES[]` and the sanitized `FmPatchId` enum value |

Transpose remains deliberately unwired — it's a Core 0/MIDI-controller-layer
concern, not something a patch-data-only converter can apply.

**The converter emits no gain or level constant at all.** v1 originally gave
each operator a reference gain (`FmOpParams::level`) from one of two
hand-tuned constants (`FM_CARRIER_LEVEL_REF`/`FM_MODULATOR_LEVEL_REF`),
divided by the carrier count or modulator fan-in to avoid `int16`/`int32`
overflow on multi-carrier and multi-modulator algorithms. All of that is
gone with the F2 fixed-point contract (§5.2): `op.h`'s `FM_CYCLE` leaves 5
bits of headroom above a single operator's maximum precisely so summing
several is safe without attenuating anything, so dividing by fan-in here
would quietly make a multi-carrier patch quieter than it asks to be — the
class of error F2 exists to remove. The converter's job is now purely
translation: DX7 bytes in, DX7 parameters out, with no opinion anywhere
about absolute level.

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
and the tool logs a warning. As §4.2 above covers, that fallback turned out
to be the correct, permanent behavior rather than a stopgap: Dexed's own
algorithm table shows it never implements the second loop either.

A bus is **not** emptied when it is read (F7, `history_fm.md` §5.20) — the
decoder used to clear a bus's contents on its first reader, which silently
kept only the first of two or three operators that legitimately share one
modulator on algorithms 19–25. Fixed by tracking bus contents explicitly and
recording every reader beyond the first as an `extra_targets` edge
(`FmOpParams::extra_target_mask` at runtime). Caught by a control-plane test
that reconstructs the real modulation *graph* rather than diffing algorithm
bytes — the byte-level check had passed since v1, because identical input
decoded wrongly still compares identical to identical input decoded wrongly
the same way.

### Packed-voice unpacking

`unpack_voice()` is a bit-for-bit port of Dexed's
`Cartridge::unpackProgram` (Source/PluginData.cpp, Apache-2.0), cross-checked
against the DX7 MIDI Data Format Sheet's own published "Bulk Dump Packed
Format" table. Every field is range-checked against its documented range
(0-99, 0-31, 0-7, …) and raises rather than silently clamping — unlike
Dexed's own `normparm`, which clamps corrupted bytes defensively since it
has to keep running against whatever's already loaded (four out-of-range
factory bytes across all 256 ROM voices — three EG fields and one detune —
are the one place this is relaxed: both engines consume them through
functions that are *total* over the wider byte range and agree on the
result, so passing them through is measurably more accurate than clamping,
not less strict; a warning still records that the patch is off-panel).
`parse_syx_bulk()` validates the full `F0 43 0n 09 20 00 … checksum F7`
envelope, including the masked 2's-complement checksum.

No per-operator reference gain is computed here any more (see above) — v1
originally scaled multi-carrier patches down by `1/carrier_count` to avoid
clipping under a flat per-role reference, which F2 made unnecessary. v1 also
forced a carrier's release level (L4) to 0 on the theory that a nonzero
carrier L4 would leave the voice allocator unable to ever reclaim the voice
(the tracker's #21 "voice never frees" bug shape). That reasoning didn't
hold — `voice_alloc.cpp`'s allocator can reclaim a released-but-still-decaying
voice at a lower steal priority, so nothing was ever actually stuck — and the
override deleted a real, deliberate part of some patches' design (a nonzero
carrier L4 means "keep sounding after key-off," e.g. ROM1A's TRAIN, a train
whistle). Removed (F6, `history_fm.md` §5.16); real carrier L4 data now
passes straight through.

Patch select is wired into `midi_controller.cpp` behind the same
`T00T_FM_HAS_PATCHES` gate: Program Change and CC30 (the BeatStep Pro can't
reliably send real Program Change, same reasoning #36 gave speech's
phrase-bank CCs) both pick `FM_PATCHES[value % FM_PATCH_COUNT]`.

**Verification asset:** because real DX7 banks load, [Dexed](https://asb2m10.github.io/dexed/)
is the ground-truth reference renderer for this module — the same role
`openmpt123` plays for the tracker and `say -v Fred` plays for speech. The P6
calibration pass this section originally deferred to "compare per-patch
output against Dexed on a fixed set of notes" has effectively already
happened, ahead of schedule: `tools/fm_ref/` builds Dexed itself (fetched at
a pinned SHA, never vendored) into a `dexed_render` CLI, `tools/fm_compare.py`
scores harmonic tracking/spectral centroid/envelope shape against it, and
`tools/fm_ctl_diff.py` diffs the DX7 control-rate math (EG, LFO, key/rate
scaling, routing) exactly, row for row. `tools/fm_regress.py` +
`tools/fm_thresholds.json` run all four ROM banks × 5 note/velocity
configurations (1,600 patch renders) on every change and gate on both the
bank mean and the worst single patch — the committed thresholds (0.53 dB
harmonic, 1.53 dB attack, 0.34 dB envelope MAE) are what caught every defect
this document's §3–§7 now describe as fixed. `render_fm`'s own patch-bank
render (`fm_patches/*.wav`, one 3-second note per patch — long enough to
catch a deliberately slow-swelling patch like ROM1A's "TAKE OFF") remains
the quick host-side sanity pass; the full Dexed diff is `tools/fm_regress.py`.
See `history_fm.md` §§1–6 for the harness's own design and §5 for the full
phase-by-phase results.

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

**F8 (`breadboard_rp2350`, real DX7 patches from `patches.h`, held notes, no
FX) measured markedly higher than either projection above, and the
`MAX_VOICES`/budget picture is open again as a result.** 16 voices:
**87.9% duty**, a clean linear fit `cycles ≈ 71.4 + 182.4 × N` (exact at
N=8/16). That is **182.4 c/f/voice above idle** — 1.8× #43's kernel-only
baseline and 1.2–1.7× #45's EnvDX-only checkpoint, and single-patch numbers
range 177–235 c/f depending on the patch's own algorithm/feedback/LFO
content (unlike #43's uniform synthetic rig, a real patch's actual DX7 data
now drives the real per-block cost). Re-solving §9's budget with today's
idle (71.4 c/f, itself 4.7× #43's 15 c/f — not yet confirmed to be the same
measurement convention) and the old #43 reverb figure (268.7 c/f) gives
**~11–14 voices** as the reverb-safe count, not 16. `MAX_VOICES` has **not**
been changed on this reading: the idle-measurement discrepancy needs
confirming and there is no FX-on reading yet against the current (F7)
engine, and per #53's own acceptance criteria this is meant to be a HITL
gate, not a number set from a cycle count alone — the by-ear pass is 5 of 32
ROM1A patches in, reported as sounding "much more authentic" than the
original (Attempt 1) build. Mild distortion reported at 16 voices/no-FX was
tracked to mix-stage summing headroom, not a Core 1 deadline overrun (its
intensity tracks note *volume*, which a deadline miss would not). See
`history_fm.md` §5.22 ("F8 — hardware voice-count sweep") for the full sweep and
the open items.

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
| Patch bank (32 × ~245 B, runtime form) | ~7.6 KB | Flash (read-only) — **measured, #47**: ROM1A's 28 converted patches were 5488 B (196 B/patch) pre-#48; **#48**: all 32/32 convert (fixed-frequency support), 220 B/patch; **#49**: `FmOpParams` gained `am_sensitivity` (+1 B/op) and `FmPatch` gained voice-wide `lfo`/`pitch_eg` structs (+~16 B/patch) — 7744 B (242 B/patch); **F7** (`history_fm.md` §5.20) added `extra_target_mask` (+1 B/op) for modulator fan-out — device flash measured 54,588 B total at F7 (+784 B over the pre-F7 build), estimate per `tools/syx2patch.py`'s own printed flash-cost line, not a compiled `sizeof()` |
| Per-voice state (16 × ~200 B, `FmOp[6]`) + pitch EG/LFO (16 × ~24 B, one each per voice) | ~3.6 KB | SRAM |
| Shared bus scratch (7 × 16 × int32) | 448 B | SRAM |
| Mix scratch | reuses existing `scratch[]` | SRAM |
| **Total SRAM** | **~12.5 KB** | of 520 KB |

**No PSRAM. No streaming. No dynamic allocation.** This should be treated as a
design invariant of the module.

---

## 9. CPU budget summary

This table has two layers: the original #43/#45 projection (kernel + EG,
before the Dexed-conformance rewrite existed to bench against), and F8's
real hardware reading of the complete, current (F7) engine.

| Item | Cycles/sample | % of 3401 |
|---|---|---|
| Idle / DMA / IPC (#43/#45-era measurement) | ~15 | 0.44% |
| Idle / DMA / IPC (**F8 measurement, current engine**) | 71.4 | 2.10% |
| Global FX insert (reverb — measured on the subtractive engine's identical shared code, not yet re-confirmed on the current FM build with FX linked) | 268.7 | 7.9% |
| 16 × 6-op voice, projected (#43 kernel + §3.3's unmeasured ~19 c/f EG/LFO) | ~1920 | 56.5% |
| 16 × 6-op voice, **F8 measured** (real DX7 patches, no FX) | ~2918 | 85.8% |
| **Total, original projection** | **~2204** | **~65%** |
| **Total, F8 measured (no FX; reverb not yet re-confirmed on this build)** | **~2990** | **~88%** |

The original projection (65%, comfortably under budget) is what this table
said before the Attempt-2 rewrite's own hardware pass existed. F8's own
measurement of the *current* engine — real DX7 patches from `patches.h`,
not the P0 rig's synthetic fixed topology — came back markedly higher:
**87.9% duty at 16 voices with no FX at all**, before reverb's 268.7 c/f
(7.9%) is even added. Re-solving with today's measured idle and the old
(not yet re-confirmed on this engine) reverb figure gives roughly
**11–14 voices** as the reverb-safe count, not 16 — see §3.4's own F8
paragraph for the qualifications (the idle jump itself needs confirming as
the same measurement convention, and there is no FX-on reading yet against
the current engine). `MAX_VOICES` has not been changed pending both, plus
the still-in-progress by-ear pass — see `history_fm.md` §"F8 — hardware
voice-count sweep" for the full numbers and the open items.

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
| 1 | ~~Measured cycles/operator, and therefore the real voice count~~ **Closed at P0, #43: 100.05 c/f/voice measured (kernel only), `MAX_VOICES=16` confirmed.** **Reopened by F8** (`history_fm.md` §5.22, "F8 — hardware voice-count sweep"): the complete, current engine measures 182.4 c/f/voice above idle on real DX7 patches — the reverb-safe count re-solves to ~11–14 voices, not 16. `MAX_VOICES` is unchanged pending an idle-measurement-convention check, an FX-on reading against the current engine, and the rest of the by-ear pass — see §3.4/§9. | **P0 done; F8 reopened it, unresolved** |
| 2 | ~~FX insert cost in isolation; is Freeverb worth ~4% in an FM context?~~ **Closed, #43: 268.7 c/f / 7.9% (reused from the subtractive engine's identical shared FX code), not ~4%. Freeverb stays** — the 16-voice budget clears with ~27% margin even at the corrected cost. | **P0 — done** |
| 3 | ~~BLOCK size — 16 assumed; confirm against rate-99 attacks~~ **Closed, #45: BLOCK=16 confirmed, not raised to 32 or lowered to 8** — `history_fm.md` §"FM Engine — EnvDX + BLOCK Confirmation (#45)". | **P2 — done** |
| 4 | ~~Extract a shared `BlockClock` for FM and speech, or keep them separate?~~ **Closed, #46: reject — common pattern, no common code.** Compared the real `EnvDX` (§5.3) against speech's segment sequencer: per-operator vs. per-voice instancing, fixed 4-stage log2 vs. variable-length float segments, exact per-sample `gain_step` interpolation vs. per-sub-block-constant IIR smoothing. See §5.3's cross-module note and `architecture.md` "Settled Decisions". Speech's sequencer (#34/#36/#37) untouched. | **P2 — done** |
| 5 | ~~Global vs. per-voice LFO default~~ **Closed, #49: per-voice, global-phase mode dropped (not built), by design.** #48's multitimbrality (one patch pointer per voice) leaves a literal shared LFO with no principled behavior once two active voices with *different* patches both request global phase — a case that can't arise on real single-timbral hardware, so there's no real DX7 behavior to match. Per-voice-with-key-sync already covers the common "block chord" fidelity case. See §5.5's own writeup. | **P4 — done** |
| 6 | ~~Algorithms 4 and 6: interleaved fallback (X1) or documented limitation?~~ **Closed: not needed, not deferred.** §4.2/§7's F4 finding: Dexed's own algorithm table shows it never implements the second, operator-spanning loop for these two algorithms either (`fb_out` without `fb_in` on the secondary operator, gated out by `fm_core.cc`). This engine's collapse-to-single-feedback fallback matches the reference exactly, not approximately — measured 0.1–3.9 dB harmonic MAE on the seven ROM patches using either algorithm, in line with the rest of the bank. X1 and `#54` are closed as not needed. | **Closed, F4** |
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
   #43's kernel-only baseline). The by-ear-against-Dexed check this step
   originally asked for was later superseded by a quantitative one: F0–F7
   (`history_fm.md` §§1–6) built a harness that renders both engines and
   diffs them directly, control-plane-exact and spectrally, rather than
   relying on ear alone. The hardware listen itself is a separate, still
   partially open item — see step 6 below.
4. ~~**P2** — `EnvDX`. Confirm BLOCK choice against fastest-attack patches.~~
   **Implemented, #45**, then **replaced wholesale by F3** (§5.3): the
   original design (4-stage log-domain EG, hand-fit DX7 level/rate tables)
   is gone, superseded by a direct port of Dexed's real `Env`. BLOCK=16
   confirmed, not changed — `history_fm.md` §"FM Engine — EnvDX + BLOCK Confirmation
   (#45)".
5. ~~**P3** — the converter.~~ **Implemented, #47 (v1) + #48 (v2)**
   (`tools/syx2patch.py`: 32-voice .syx unpack, all 32 algorithms mapped to
   `mod_target`/`feedback` via one generic bus-simulation decode, algorithm
   4/6 interleaved-feedback detected via cycle analysis and collapsed to
   single self-feedback with a logged warning, EG rates/levels/output
   level/velocity sensitivity copied straight through since env_dx.h
   already converts them at runtime, coarse/fine ratio computed, per-
   carrier/per-modulator level scaled down by fan-in to avoid int32
   overflow on multi-carrier/multi-modulator algorithms — real bugs caught
   by host-rendering the actual bank, not assumed; the fan-in scaling was
   itself later removed by F2 once the engine had a single shared headroom
   ceiling that made it unnecessary, §7). #48 added key level
   scaling and rate scaling (both ported from Dexed's real `ScaleLevel`/
   `ScaleRate`, resolved at note-on from a new `VoiceParams::note` field),
   detune (a fixed ±7-cents approximation, later replaced by F5's real
   note-dependent formula, §5.6), and fixed-frequency mode (a
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
   `PitchEnv`/`Lfo`; `tools/syx2patch.py` v3 bakes both from the `.syx`'s
   bulk offset 102-109/112-116). Closes open question 5: per-voice LFO,
   global-phase mode considered and dropped (see §5.5's own writeup for
   why). Zero per-sample cost confirmed against device disassembly (48
   `smlawb` instances, unchanged). `pitch_eg.h` measured exact against Dexed
   on first check (§5.4); `lfo.h`'s waveform phase-origin bug and mod-wheel
   convention were both later found and fixed by F6 (§5.5). Hardware listen
   is F8's own item now, partially done — 5 of 32 ROM1A patches heard on
   `breadboard_rp2350` as of F8, reported as sounding "much more authentic"
   than the original build; the rest of the bank, and a check with the mod
   wheel actually moved (see §5.5's note), remain.
7. **P5–P6** — free routing UI, calibration against Dexed. P6's calibration
   half is substantially done ahead of this build-order slot: `history_fm.md`
   §§1–7 (F0–F8) is exactly that pass, run as a dedicated evaluation rather
   than waiting for P6 — see §7's "Verification asset" paragraph. What
   remains here is P5's free-routing UI/patch-model work and any residual
   F8 hardware items (§3.4/§9).

`WAVE_FM2` (§10) can be slotted in anywhere; it does not gate anything.

---

## 13. Summary — the new-code list

**New files:**
`engines/fm/{engine.h, audio_engine.cpp, op.h, fm_scale.h, env_dx.h, pitch_eg.h, lfo.h, sine_tab.h, patch.h, patches.h, rig.h, render.h, midi_controller.cpp, display.cpp}`,
`tools/syx2patch.py`. There is no `presets.h` — FM's whole timbre is the
single `FmPatch` pointer (§6.3), never the shared per-voice-type preset
shape. The Dexed-conformance evaluation added a second tooling tree not
originally planned here: `tools/fm_ref/` (Dexed itself, fetched at a pinned
SHA), `tools/fm_compare.py`, `tools/fm_ctl_diff.py`, `tools/fm_freq_diff.py`,
`tools/fm_regress.py` + `tools/fm_thresholds.json`, and
`tools/host_render/render_fm_patch.cpp` — see §7's "Verification asset"
paragraph and `history_fm.md` §3.

**Modified:**
`voice_alloc.*` (operator-cost weight), `CMakeLists.txt` (engine selection),
`history_fm.md` (P0 through F8 measurements and the Dexed-conformance
rewrite), `wslcd/display.cpp` (operator budget readout, not yet built —
§6.4/open question 8).

**Unchanged:** IPC, MIDI, output, effects, LCD driver.

**Headline expectation:** full 6-operator, freely-routed, per-voice-
multitimbral FM, in ~12-13 KB of SRAM, with real DX7 banks loading via a
host converter that now matches Dexed to a mean 0.53 dB harmonic error
across all 256 factory voices. `MAX_VOICES=16` is provisional again as of
F8's hardware pass (§3.4/§9) — the reverb-safe count may land closer to
11–14 once the open items there are resolved.
