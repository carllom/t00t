# T00T — FM Module (DX7-class): Design & Implementation Plan

A design document for a dedicated **6-operator phase-modulation engine** for the
t00t platform, targeting DX7 feature parity and DX7-class polyphony on the
RP2350. It is a *mode* — a build-time engine variant selected via `T00T_ENGINE=fm`,
following the pattern established by `groovebox` and the layout already sketched
in `architecture.md`.

A secondary, much smaller proposal — adding a 2-operator FM oscillator to the
*generic* subtractive engine — is covered in §10. The two are independent and can
proceed in either order.

Status: **design draft.** No code written yet.

> ~~**Provenance of the numbers in this document.** The per-operator cycle
> figures in §3 are derived from *static instruction counts*... **P0 exists to
> replace these estimates with measurements before any DX7 logic is
> written.**~~ **Struck, #43.** P0's profiling-pin bench pass is done
> (`engine.md` §"FM P0 Measurement (#43)") — the per-operator kernel costs in
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

## 3. The performance question

### 3.1 Budget arithmetic

At 150 MHz and 44100 Hz, Core 1 has:

- **3401 cycles per sample frame**
- **870,748 cycles per 256-sample buffer** (5.805 ms deadline)

Calibrating against the measured figures in `engine.md`:

| Measured (engine.md) | Duty | Cycles/sample |
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

**Measured, #43** (`engine.md` §"FM P0 Measurement (#43)", `breadboard_rp2350`,
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

Measured #43 (`engine.md` §"FM P0 Measurement (#43)"), decisions in §3.4:

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
   re-measured (tests 14/15, `engine.md` §"FM P0 Measurement (#43)"):
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
   resolution.~~ **Measured: BLOCK=8 is 4.9% cheaper, BLOCK=32 is 10.8% more
   expensive — inverted from the amortisation story, because this rig has no
   EG/LFO to amortise. The real driver is GCC's loop-unrolling threshold**
   (confirmed: `audio_engine_run()` compiles to 5,568 bytes at BLOCK=16 vs.
   1,336 at BLOCK=32, and BLOCK=32 alone compiles the per-operator loop as a
   real branch). **Decision: BLOCK=16 stays provisional**, final call
   deferred to P2 as planned, since that's when the EG-resolution side of
   the tradeoff actually exists to measure.
4. **M33 DSP extension.** `smulwb` fuses the `mul` + `asr` pair. **Measured:
   −3.0%, as predicted. Adopted where convenient** — real, no correctness
   cost (host + device verified in #42).
5. **SIO interpolators.** Not tried — `fm.md`'s own prediction ("probably not
   worth it with a non-interpolating lookup") held up well enough by the
   other levers' small margins that this wasn't worth #43's bench time.

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

> **Cross-module note.** This is structurally the same problem as the speech
> module's per-voice segment clocks: *N independent control-rate clocks per voice*,
> stepped at block boundaries, driving per-sample interpolated values. The
> tracker's single ordered `TickBlock` ring does not serve either case. If a
> shared `BlockClock` abstraction is worth extracting, FM and speech are the two
> customers — decide once, at P2, rather than twice.

### 5.4 `fm/pitch_eg.h` — pitch envelope

One per voice, same 4-stage shape as `EnvDX` but operating on pitch (in cents)
rather than amplitude. Applied by scaling all six operator increments at each
block boundary. Per-sample cost: **zero**.

### 5.5 `fm/lfo.h` — voice LFO

Rate, delay, waveform (tri/saw up/saw down/square/sine/S&H), key sync, PMD, AMD.
Evaluated once per control block. Pitch mod folds into the increment scaling
alongside the pitch EG; amplitude mod folds into each operator's `gain`/`gain_step`
computation according to its AM sensitivity. Per-sample cost: **zero**.

The DX7 has one global LFO; a per-voice LFO with key sync is strictly better for
polyphony and costs nothing extra here. Keep a patch flag for global-phase mode if
DX7 fidelity on specific patches matters.

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

## 7. Host-side tooling: `tools/syx2patch.py`

Following the principle established by the tracker's converter: **relocate all
awkward, nonlinear, one-time work to the host, so the runtime sees only increments
and pointers.**

Input: a DX7 32-voice bulk dump `.syx` (4096-byte payload, 128 packed bytes per
voice).

Output: `src/engines/fm/patches.h` — a `const FmPatch patches[]` array in flash.

The converter resolves:

| DX7 source data | Baked output |
|---|---|
| Algorithm number (0–31) | `order[6]`, `in_bus[6]`, `out_bus[6]`, first-writer flags, kernel selection, feedback operator index |
| EG rates/levels (0–99) | Per-stage log-domain increments at the 2756 Hz control rate, plus target levels |
| Operator output level (0–99) | Log-domain attenuation, via the DX7's nonlinear level table |
| Key level scaling (breakpoint, curves, depths) | Per-note offset table or curve coefficients |
| Rate scaling (0–7) | EG rate offset coefficients |
| Coarse/fine ratio, detune, osc mode | Q16 ratio multiplier, or absolute Hz for fixed mode |
| Velocity sensitivity (0–7) | Level offset coefficients |
| LFO rate/delay/waveform/PMS/AMS | Control-rate increments and depth scalars |
| SCC detection | `needs_interleaved` flag for algorithms 4 and 6 |

The converter should **fail loudly** on anything it cannot represent, rather than
approximating silently.

**Verification asset:** because real DX7 banks load, [Dexed](https://asb2m10.github.io/dexed/)
becomes the ground-truth reference renderer for this module — the same role
`openmpt123` plays for the tracker and `say -v Fred` plays for speech. P6 is a
calibration pass comparing per-patch output against Dexed on a fixed set of notes.

---

## 8. Memory budget

| Item | Size | Location |
|---|---|---|
| Operator sine table (4096 × int16) | 8 KB | SRAM |
| Exp2 table for EG (256 × int16) | 0.5 KB | SRAM |
| Patch bank (32 × ~200 B, runtime form) | ~6.4 KB | Flash (read-only) |
| Per-voice state (16 × ~200 B) | ~3.2 KB | SRAM |
| Shared bus scratch (7 × 16 × int32) | 448 B | SRAM |
| Mix scratch | reuses existing `scratch[]` | SRAM |
| **Total SRAM** | **~12 KB** | of 520 KB |

**No PSRAM. No streaming. No dynamic allocation.** This should be treated as a
design invariant of the module.

---

## 9. CPU budget summary

| Item | Cycles/sample | % of 3401 |
|---|---|---|
| Idle / DMA / IPC (measured) | ~15 | 0.44% |
| Global FX insert (reverb — measured, reused from the subtractive engine's identical shared code, `engine.md` §"FM P0 Measurement (#43)") | 268.7 | 7.9% |
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
targeted — see `engine.md` §"FM P0 Measurement (#43)" for the full
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

## 11. Recommended build order

1. **P0 — measure.** ~150 lines. 16 voices × 6 ops, fixed increments and gains,
   no EG, no patch logic. Scope GPIO 22. In the same session, measure:
   flash vs. `__not_in_flash_func`; 1024 vs. 4096 table; interleaved vs. plain
   kernel; BLOCK = 8/16/32; and the FX insert in isolation.
2. ~~Record the results in `engine.md`'s performance table before writing P1.
   Update §3.4 of this document with measured figures and strike the
   provenance caveat.~~ **Done, #43.**
3. **P1** — engine skeleton with one hardcoded patch. Verify ratios and routing by
   ear against Dexed on the same algorithm.
4. **P2** — `EnvDX`. Confirm BLOCK choice against fastest-attack patches.
5. **P3** — the converter. This is the largest single piece of work and the one
   that makes everything else verifiable.
6. **P4–P6** — remaining parameters, free routing, calibration.

`WAVE_FM2` (§10) can be slotted in anywhere; it does not gate anything.

---

## 12. Open questions / decisions to make

| # | Question | When |
|---|---|---|
| 1 | ~~Measured cycles/operator, and therefore the real voice count~~ **Closed, #43: 100.05 c/f/voice measured (kernel only), `MAX_VOICES=16` confirmed** — `engine.md` §"FM P0 Measurement (#43)". Raising past 16 deferred to a P2 bench pass once EG/LFO exist to measure. | **P0 — done** |
| 2 | ~~FX insert cost in isolation; is Freeverb worth ~4% in an FM context?~~ **Closed, #43: 268.7 c/f / 7.9% (reused from the subtractive engine's identical shared FX code), not ~4%. Freeverb stays** — the 16-voice budget clears with ~27% margin even at the corrected cost. | **P0 — done** |
| 3 | BLOCK size — 16 assumed; confirm against rate-99 attacks | P2 |
| 4 | Extract a shared `BlockClock` for FM and speech, or keep them separate? | P2 |
| 5 | Global vs. per-voice LFO default (DX7 fidelity vs. better polyphonic behaviour) | P4 |
| 6 | Algorithms 4 and 6: interleaved fallback (X1) or documented limitation? | P4 |
| 7 | Patch bank source — ship a curated set, or make `.syx` loading a runtime feature over MIDI SysEx? | P3 |
| 8 | Does per-voice multitimbrality warrant a MIDI channel→patch mapping UI on the LCD? | P5 |
| 9 | X2 operator waveforms — only if P0 leaves headroom | P6 |
| 10 | **Core0/Core1 XIP cache contention.** #43's "keep the kernel in flash" decision (§3.6 item 2) was measured with Core0 doing essentially no flash-side work — MIDI/LCD/control are still stubs (#41/#42). RP2350 has one 16 KB XIP cache shared by *both* cores (`hardware_xip_cache.h`); once Core0 does real LCD/MIDI/control work, its flash traffic can evict the FM kernel's cache lines, right when Core0 is busiest — a risk #43 didn't test and can't yet, since there's no real Core0 workload to contend against. Mitigation available if it turns out to matter: `xip_cache_pin_range()` (RP2350-only) permanently reserves the kernel's flash range against eviction by anything else, keeping flash's speed without the exposure. If pinning doesn't pan out, SRAM's #43 "measured worse" verdict was itself measured in isolation — SRAM sidesteps this specific shared-cache problem entirely (its own contention risk is per-bank and controllable), so it's a fallback, not dead. | **P1+, once Core0 has a real workload to bench against** |

---

## 13. Summary — the new-code list

**New files:**
`engines/fm/{engine.h, engine.cpp, op.h, env_dx.h, pitch_eg.h, lfo.h, sine_tab.h, patch.h, patches.h, presets.h, display.cpp}`, `tools/syx2patch.py`.

**Modified:**
`voice_alloc.*` (operator-cost weight), `CMakeLists.txt` (engine selection),
`engine.md` (P0 measurements), `wslcd/display.cpp` (operator budget readout).

**Unchanged:** IPC, MIDI, output, effects, LCD driver.

**Headline expectation:** 16 voices of full 6-operator, freely-routed,
per-voice-multitimbral FM, in ~12 KB of SRAM, with real DX7 banks loading via a
host converter — pending P0.
