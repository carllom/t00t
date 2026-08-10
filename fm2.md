# T00T — FM Module, Attempt 2: Evaluation & Development Plan

Supersedes the execution order in `fm.md` (branch `fm`), not its design. Written after
reading the `fm` branch source, `fm.md`, `engine.md`, and issues #5, #41–#59, and after
verifying three specific divergences against the real Dexed source (`Source/msfa/`).

**Headline recommendation: do not rewrite the module, and do not keep patching it.
Re-derive the ~600-line numeric core against a Dexed reference harness, inside the
existing scaffolding — and build the harness *first*.**

---

## 1. What actually went wrong

The `fm` branch is not a bad implementation. The architecture is sound, the routing
compiler is correct, the sysex parser is well tested, and the P0 hardware measurements
are real and hard-won. The failure is narrower and more specific than "it sounds wrong".

`fm.md`'s phasing put the Dexed calibration pass at **P6 — dead last** (issue #53, still
open). Every phase before it was validated by ear on hardware. That is the whole problem:
the DX7 is a chain of roughly a dozen nonlinear curves feeding one multiply. When the
composite output is wrong, ear feedback ("too thin", "sine-like") cannot tell you *which*
curve is wrong, so each fix lands as a compensating constant somewhere in the chain.

The git log shows exactly that trajectory:

```
cdc84b1 envelope fix
c3a0f88 feedback fix
8391107 DX7 sound compatibility fixes. Closes #57, #58, #59
```

…and the surviving artifacts of those fixes are `FM_OUT_SHIFT_CARRIER=6`,
`FM_OUT_SHIFT_MODULATOR=0`, `FM_MOD_INPUT_SHIFT=4`, `FM_CARRIER_LEVEL_REF=1<<21`,
`FM_MODULATOR_LEVEL_REF=1400000000`, and per-operator hand-tuned `level` values like
`100000000` / `83000000` / `350000000`. Six free parameters that exist only to cancel each
other out. Note that #58 and #59 were already *correct instincts* — both ported real Dexed
code and both fixed real bugs. They just did it one curve at a time, by ear, with the other
five fudge factors still in the signal path.

### 1.1 Three verified defects still present on `fm`

These were checked against Dexed's actual source, not against the branch's own comments
about Dexed.

**(a) No modulation-index contract — the big one.**

Dexed's invariant, verified from `sin.cc` and `fm_op_kernel.cc`: `Sin::lookup` returns
full-scale ±2²⁴, phase is 24 bits per cycle, and the operator output is `(y * gain) >> 24`.
So a unity-gain operator's output is *exactly one full cycle of phase deviation*. One
anchor; everything else in the DX7 is attenuation in log domain beneath it.

(F2 later measured the rest of it directly rather than deriving it: Dexed's maximum operator
gain is exactly 2.0, so a *max-level* operator peaks at two full cycles — ≈12.6 rad of
modulation index. See §5.6.)

t00t has no such anchor. Carriers and modulators run on two unrelated scales
(`>>6` vs `>>0`), a global `<<4` fudge sits on the modulation input, and the reference
gain is a hand-tuned per-operator magic number. Working the arithmetic through for a
modulator at `FM_MODULATOR_LEVEL_REF = 1400000000` (≈2³⁰·⁴) at full envelope:

```
out  = (2^30.4 * 2^15) >> 16   = 2^29.4
mod  = out << FM_MOD_INPUT_SHIFT = 2^33.4      ← overflows int32 (2^31)
```

At high envelope levels the modulation input wraps the phase accumulator several times per
sample — that is not deep FM, it is aliasing hash. At moderate levels it is fine. So the
error is **per-patch and non-monotonic in level**, which is precisely why the reports
alternated between "too thin" and "sometimes overdriven" and why no single global constant
ever fixed both. This is unfixable by tuning; the contract has to be replaced.

**(b) Wrong envelope attack curve.**

Verified verbatim from `env.cc`'s `Env::getsample()` — rising and falling stages are
*different curve families*:

```cpp
else if (rising_) {
    const int jumptarget = 1716;
    if (level_ < (jumptarget << 16)) level_ = jumptarget << 16;
    level_ += (((17 << 24) - level_) >> 24) * inc_;   // exponential approach
    ...
} else {
    level_ -= inc_;                                    // linear in log domain
    ...
}
```

`env_dx.h`'s `env_dx_step_block()` uses a symmetric linear ramp in both directions. The DX7
attack is a concave exponential approach with a jump-to-floor at 1716 — a characteristic
part of the instrument's sound, and *not* reachable by adjusting a rate table. This is a
strong candidate for the residual "delayed envelope" / attack-shape complaints that
survived #59's rate-curve fix.

**(c) Feedback is 2× too hot.**

Dexed's `compute_fb`: `scaled_fb = (y0 + y) >> (fb_shift + 1)`.
`op.h`'s `op_render_fb`: `(fb1 + fb2) >> fb_shift`. The `+ 1` is missing.

**(d) Minor, but real:** `env_dx_step_block()`'s `if (step < 1) step = 1;` forces a floor
of 1/256 octave per control block. At BLOCK=16 that is ~40 octaves in 5.8 s, so the slowest
DX7 rates (which should run tens of seconds) are hard-capped at roughly 6 seconds.

### 1.2 What this means for the rewrite question

Every verified defect lives in `op.h`'s scaling, `env_dx.h`, and the `level` field in
`patch.h`/`syx2patch.py` — about 600 lines. None of them live in the routing compiler, the
sysex parser, the MIDI plumbing, the engine skeleton, or the measured kernel *shape*.

A from-scratch rewrite would discard ~2900 lines of correct, tested, measured work to fix
600 lines of wrong arithmetic. Continuing to patch would add fudge factor number seven.
So: **re-derive the numeric core, keep the scaffolding.**

---

## 2. Disposition of the existing `fm` branch

| Tier | Files | Action |
|---|---|---|
| **Keep as-is** | `engines/fm/engine.h`, `audio_engine.cpp`, `midi_controller.cpp`, `display.cpp`, `render.h`, `sine_tab.h`, `rig.h`; `tools/host_render/` CMake scaffolding; `syx2patch.py`'s *parser* half + all 515 lines of `test_syx2patch.py`; the P0/#43/#45 measurements in `engine.md`; CMake/Makefile engine wiring | Untouched. This is the expensive, correct part. |
| **Keep, verify later** | `patch.h`'s `fm_resolve_routing()` + the 32-algorithm table | Correct as far as reviewed; gets an exact conformance test in F4 rather than a rewrite. |
| **Delete and re-derive** | `env_dx.h` (whole file); `op.h`'s gain/shift/feedback scaling (the *loop shape* stays — it is measured); `patch.h`'s `FmOpParams::level` + `FM_TEST_PATCH` constants; `syx2patch.py`'s level-emit half | Do not adjust these. Delete the six fudge constants outright and rebuild against the harness. |
| **Re-verify after the core lands** | `lfo.h`, `pitch_eg.h` | Check against Dexed's `lfo.cc` / `pitchenv.cc`. Probably small fixes, but they are unmeasurable until the core is right. |

Branching: **continue on `fm`.** A second branch buys nothing at PR time — `fm` will never
merge on its own, so an `fm2 → main` PR would carry all of `fm`'s commits anyway, and the
repo's established pattern is one branch per module (`speech` → #40, `tracker` → #26).
Attempt 1's tip is tagged `fm-attempt1` instead, which is what F0's baseline renders against
and what stays flashable for a hardware A/B at F7. Note that F2/F3 will delete work that
closed #45, #57, #58 and #59 — say so in those commit messages and on the issues, or the
history reads as unexplained self-reverts later.

---

## 3. The harness (your proposal — endorsed, with one sharpening)

Your instinct is right and it is the single highest-value item in this plan. The sharpening:

> **Split verification into a control plane (compared *exactly*, numerically, no audio) and
> a signal plane (compared spectrally). Most of the DX7 is control-rate math.**

Everything except the per-sample operator kernel — level curves, rate curves, key scaling,
rate scaling, velocity, detune, LFO, pitch EG, algorithm routing — runs at control rate and
produces numbers you can diff against Dexed's numbers directly. That converts roughly 80% of
"does it sound right?" into deterministic pass/fail unit tests, with no perceptual judgment
and no WAV files. Only the kernel genuinely needs spectral comparison.

This matters because it is what stops attempt 3 from happening: a spectral score can go
green with two errors cancelling. An exact control-plane diff cannot.

### 3.1 Four pieces

**F0-a · `tools/fm_ref/`** — Dexed's `Source/msfa/` synthesis core (24 files), driven by a
`dexed_render` CLI:

```
dexed_render --syx banks/rom1a.syx --voice 10 --note 48 --vel 100 --gate 2.0 --tail 1.5 --out ref.wav
```

*Fetched at a pinned SHA, not vendored* (`fetch_dexed.sh`). Dexed's top-level licence is
GPL-3 while the msfa files themselves carry Apache-2.0 headers (Google Inc. 2012–13) with
later GPL-era modifications — fetching keeps that distinction out of this repo entirely
while staying byte-reproducible, which is all a reference rig needs. Nothing here is ever
linked into the device firmware.

Two dependencies had to be shimmed (`shim/`): `tuning.{cc,h}` pulls in JUCE, the Surge
`Tunings` library and libMTSClient for microtuning the DX7 never had, so it is replaced by a
standard-12-TET `TuningState`; `libMTSClient.h` becomes a no-op whose `MTS_HasMaster()`
returns false, so `Dx7Note` always takes the standard path. `env.cc` and `controllers.h`
also `#include "../Dexed.h"` without referencing anything from it — the fetch script strips
that line.

The branch already proved this was feasible — #58 and #59 both built parts of msfa
standalone. The difference is that this time it is a permanent, scripted rig rather than
something used once and thrown away.

**F0-b · `tools/host_render/render_fm_patch.cpp`** — identical CLI, t00t engine, host
build. ~80% of this already exists in `render_fm.cpp`; it mostly needs the CLI and the
gate/release timing.

**F0-c · `tools/fm_compare.py`** — numpy only (no scipy). Metrics chosen to speak the
language of your original ear feedback:

| Metric | Answers |
|---|---|
| **Harmonic-track matrix** `H[k,t]` in dB — STFT (2048/256), then track the magnitude of each harmonic *k·f₀* over time (f₀ is known, the note is fixed) | "too thin", "missing portions of sound" — per-harmonic, per-instant |
| **Spectral centroid over time**, in harmonic number | "too thin" / "too bright" / "sine-like", as one scale-free ratio |
| **Broadband RMS envelope in dB** → time-to-peak, peak dB, sustain level, release T60 | "delayed envelope", "weak sustain" |
| **Log-spectral distance**, one scalar | overall regression tracking |

Alignment: no sample alignment. Note-on is t=0 on both sides, both at 44.1 kHz. Floor
everything at −80 dB.

**One thing the baseline forced, worth keeping in mind for every later phase:** timbre and
envelope must be scored on *separate frame sets*. Scoring timbre wherever the reference
sounds conflates the two — an engine whose envelope dies early then reads as spectrally
wrong for every frame it is silent, and the real timbre difference in the frames it does
sound gets averaged into noise. So spectral metrics run only over frames where **both**
engines are sounding, `coactive_frac` reports how much of the reference that covered, and a
separate attack-window (first 100 ms) timbre number stays valid even when that fraction is
low. Without this split the F0 baseline's spectral numbers were pure envelope artefact.

Output a per-patch scorecard across a whole bank, plus an aggregate.

**F1 · `tools/fm_ctl_diff/`** — the exact control-plane tests. Dexed dumps CSV
trajectories; t00t dumps its equivalents; diff with explicit tolerances (exact where t00t
ports verbatim, ≤1 LSB where a Q-format conversion is involved). Coverage: EG level
trajectory for all 100 rates × representative level sets; `scaleoutlevel`; `ScaleRate` /
`ScaleLevel` / `ScaleCurve`; operator frequency (ratio, detune, fixed mode); LFO output;
pitch EG output; the 32-algorithm routing table.

### 3.2 Declare the intended deviations up front

t00t is not trying to be Dexed, so the comparison thresholds need a principled basis rather
than "whatever passes today". Write these into the doc before F2 starts:

- 12-bit non-interpolated sine table vs Dexed's interpolated lookup
- BLOCK=16 control rate vs Dexed's 64
- 16 voices; no Dexed "Mark I" / "OPL" engine modes (use the plain msfa path as reference)
- float used at control rate where it is free, fixed-point in the kernel

**One of these deserves an early check rather than an assumption.** `fm.md` §3.5 justifies
skipping interpolation with a −72 dBc truncation-spur estimate — but that estimate was made
at *low* modulation index, and the whole point of fixing (a) is that real patches run at
index 3–10. Truncation spurs under heavy phase modulation do not behave like they do on a
bare sine. Put an explicit A/B in F0 (interpolated vs not, on a bright patch like ROM1A
BRASS 1 at real index) so you find out before F2 rather than after F7. If it matters it
changes the kernel budget, and the P0 headroom exists to absorb it.

### 3.3 Sysex banks

Script the download from yamahablackboxes.com into a gitignored `tools/dx7_banks/` rather
than committing the `.syx` files — keeps a redistribution question out of an open-source
repo, and `syx2patch.py` already parses them.

---

## 4. Phasing

Eight issues, each with a machine-checkable gate. The ordering principle is the inverse of
`fm.md`'s: **the calibration pass (#53) moves from last to first.**

| Phase | Deliverable | Gate |
|---|---|---|
| **F0** | Reference rigs. Build `dexed_render`, `render_fm_patch`, `fm_compare.py`, bank fetch script. **No engine changes.** Plus the §3.2 interpolation A/B. | **Met, §5.** Scorecard runs end-to-end on ROM1A #11 (E.PIANO 1) C3, Dexed vs the **current `fm` branch**. That baseline number is the thing everything after is measured against — and it retroactively quantifies how far off the current build is, which no amount of listening ever did. |
| **F1** | Control-plane conformance harness (`fm_ctl_diff`). | **Met, §5.4.** It runs, and **fails loudly** on the current `env_dx.h` — i.e. it independently rediscovers §1.1(b) and (d) without being told. If it passes, the harness is wrong. |
| **F2** | **Met (level/overflow), §5.6.** **Fixed-point contract.** Delete `level`, `FM_OUT_SHIFT_CARRIER`, `FM_OUT_SHIFT_MODULATOR`, `FM_MOD_INPUT_SHIFT`, and both `*_LEVEL_REF`s. Re-derive `op.h` + `patch.h` + `syx2patch.py`'s emit half around *unity gain = one full cycle of phase deviation*. Carriers and modulators share one scale; the carrier attenuation happens once, at the voice mix. | Level gap collapses to a few dB across the bank (met: -0.4..-6.2 dB on 27/32, the rest slow-attack patches F3 owns). No int32 overflow at max level on any ROM1A patch, proved by bound not sample (met: 1.42 bits headroom). **The spectral-centroid criterion originally written here was moved to F3** -- brightness is modulator level over time, so it cannot be judged while the envelopes are broken. |
| **F3** | **Met, §5.7-5.8.** **`EnvDX` rewrite.** Exponential-approach rising stage with the 1716 jump target, linear falling, `advance()`'s real level+TL composition (`(scaleoutlevel(L)>>1)<<6 + outlevel − 4256`, min-16 clamp), the real rate derivation, `staticcount` handling for slow rates, and remove the `step<1→1` clamp. | F1's six `eg/*` cases within tolerance. F0 envelope metrics (time-to-peak, T60) within 10% on E.PIANO 1 and TUB BELLS. **Spectral centroid** (moved from F2) approaches 1.0× on the bright patches. The four slow-attack level outliers (§5.6) close at a 2 s gate. |
| **F4** | Feedback `>>(fb_shift+1)` fix; exact conformance test of all 32 algorithms against Dexed's table. Decide algorithms 4 and 6 (two-op loops, old X1/#54): implement the interleaved kernel, or document the approximation and its measured cost. | F1 routing table exact. Scorecard green on the feedback-heavy patches. |
| **F5** | Key level scaling, rate scaling, velocity, detune, fixed-frequency — re-verified against F1 rather than re-derived by ear. | F1 exact for all four. |
| **F6** | LFO + pitch EG re-verification against `lfo.cc` / `pitchenv.cc`. | F1 exact; scorecard green on vibrato-heavy patches. |
| **F7** | **Full-bank regression + first hardware checkpoint.** All four ROM banks × several notes and velocities through the scorecard. Commit the resulting thresholds as a checked-in file. *Then* flash and listen — once. | Aggregate score under threshold; your listening test; re-measure c/f/voice (F3 changes the control-rate cost materially). |
| **F8** | Performance retune → final `MAX_VOICES`. | Measured, per `engine.md` convention. |

Open issues #50 (patch selection over MIDI), #52 (display panel), #51 (P5 operator-budget
allocation), #55 (X2 waveforms) and #56 (WAVE_FM2 in the subtractive engine) are unaffected
by any of this and can proceed independently at any point.

---

## 5. F0 and F1 results (measured)

The rig is built and the gate is met: `tools/fm_ref/` (see its README for setup),
`tools/fm_compare.py`, `tools/host_render/render_fm_patch.cpp`. Baseline scorecard
committed at `tools/fm_ref/out/baseline_rom1a_c3.json` — ROM1A, all 32 voices, C3
(MIDI 48), velocity 100, 2 s gate + 1.5 s tail, Dexed @ `2e182b3d` vs `fm` @ `cdc84b1`.

### 5.1 Baseline, ROM1A × 32 at C3

| | mean | range |
|---|---|---|
| Harmonic MAE | 18.9 dB | 0.6 (E.ORGAN 1) → 55.7 (CLAV 1) |
| Attack timbre MAE (first 100 ms) | 26.8 dB | 2.2 → 73.3 |
| Envelope MAE | 33.6 dB | 1.0 → 66.2 |
| Spectral centroid ratio | 1.87× | 0.30× (GUITAR 2) → 6.31× (VOICE 1) |
| Level gap | — | −12.1 dB → −95.9 dB |

Three things this says that no listening session did:

**The level error is per-patch and non-monotonic, exactly as §1.1(a) predicts.**
Every patch is quieter than the reference, but by anywhere from 12 dB to 96 dB.
STRINGS 1/2/3 and VOICE 1 (−88 to −96 dB) are effectively silent. A single global
gain constant cannot fix a spread like that — which is why none of attempt 1's ever did.

**Brightness misses in both directions at once.** Centroid ratios run from 0.30×
(far too dull) to 6.31× (far too bright) *within the same build*. That is the
"too thin" / "sometimes overdriven" contradiction in the original feedback, and it
is what an uncontrolled modulation index looks like when it lands differently per patch.

**E.ORGAN 1 scores 0.6 dB harmonic / 2.2 dB attack / 2.4 dB envelope / 1.08×
centroid.** A near-perfect match on one patch is the most useful number in the
table: it proves the sysex parser, the algorithm routing compiler, the operator
kernel's loop shape and the whole harness are sound. The failures are confined to
the level/EG/modulation chain, which is precisely the Tier-B scope in §2. That is
the measured version of §1.2's argument against a from-scratch rewrite.

### 5.2 The E.PIANO 1 envelope, measured

RMS envelope, dB relative to each render's own peak:

| | 20 ms | 200 ms | 400 ms | 800 ms | 2000 ms (key-up) | 2500 ms |
|---|---|---|---|---|---|---|
| Dexed | −1.5 | −2.8 | −2.4 | −0.3 | −9.9 | −63.4 |
| t00t | **0.0** | −6.8 | −21.3 | **−59.0** | −65.3 | −223 (exact zero) |

t00t peaks at 20 ms and is inaudible by 800 ms, then sits on a −65 dB plateau for
the rest of a 2-second held note. Dexed sustains within 3 dB of its peak for the
whole gate and releases over ~500 ms.

Two distinct defects, not one. The fast collapse is the rate/curve problem of
§1.1(b)/(d). The −65 dB plateau is a *level* problem: that is the EG sitting on its
stage-3 sustain target, mapped ~60 dB lower than the reference puts it. #58 ported
`scaleoutlevel()` correctly, but the surrounding composition — the hand-tuned
per-operator `level` reference standing in for the DX7's real `<<5` / `−14<<24`
bias chain — still lands the result in the wrong place. More evidence that these
constants have to go rather than be re-tuned.

### 5.3 The interpolation check (§3.2) — assumption was wrong, decision stands

`tools/fm_ref/sine_table_ab.py`, 2-op 1:1 pair, t00t's exact integer arithmetic
(uint32 phase, 4096-entry table, `phase >> 20`) against a float64 reference:

| β | 0.5 | 2 | 3 | 5 | 8 | 12 |
|---|---|---|---|---|---|---|
| Worst non-harmonic spur, no interp | −77 dBc | −68 | −67 | **−60** | **−55** | **−53** |
| With linear interpolation | −117 dBc | −111 | −111 | −102 | −97 | −96 |

fm.md §3.5's −72 dBc estimate is right at β ≤ 0.5 and **wrong by 10–20 dB in the
β = 3–10 range real patches use** — the spur floor rises with modulation index,
because the lookup index sweeps the table at a rate set by the modulated phase.
Interpolation buys a flat +37–38 dB at every index, not just low ones.

Also measured: a 24-bit table value scores *identically* to 16-bit at every β. The
entire error is phase truncation, none of it amplitude quantisation — so
`sine_tab.h` staying `int16_t` (8 KB) is free of consequence, and widening it
would buy nothing.

**Decision: keep the non-interpolated table.** −55 dBc under heavy modulation is
acceptable on a platform whose stated remit is lo-fi, and the real DX7's own
12-bit phase resolution is where this number comes from in the first place. But
the cost is now known rather than assumed, and the trade is concrete: interpolation
costs ~+45 c/f/operator on #43's measured 100 c/f/voice kernel, which with #45's
measured EG overhead puts a voice at ~182–199 c/f against 2607 available — i.e.
**16 voices without interpolation, or 13 with**. Revisit at F7 only if bright
patches audibly grit; do not spend three voices on it speculatively.

### 5.4 F1 — control-plane conformance

`tools/fm_ref/dexed_dump`, `tools/host_render/t00t_ctl_dump`, `tools/fm_ctl_diff.py`.
**Gate met: 12/24 pass, and every failure traces to a specific engine defect** — the
harness independently rediscovered §1.1(b) and (d) without being told what to look for.

**What passes** — and this is as valuable as what fails, because it bounds the work:

| | |
|---|---|
| `table/scaleoutlevel` | 100 rows identical — #58's port was correct |
| `table/scale-rate`, `table/scale-level` | 1024 / 55040 rows identical — #48's ports were correct |
| `table/algorithms` | all 192 (algorithm, operator) entries identical to `FmCore::algorithms`. Confirms §2's Tier-1 assumption that the routing table and `syx2patch.py`'s decode are sound |
| `pitcheg/*` | all three within 0.6 cents mean |
| `lfo/rate` | all 34 sampled rate values within 5% |
| `lfo/shape-w1`, `w2` | sawtooth up and down, 0.014 mean error |
| `lfo/sample-hold` | correct step rate and range |

**What fails**

**The EG, all six cases.** The sharpest single number in this whole document:

| rates 99,99,99,99 / levels 99,99,99,99 | time to reach −1 dB |
|---|---|
| Dexed | **0.0 ms** (first control block) |
| t00t | **16.3 ms** |

That is §1.1(b) measured. `env_dx_trigger()` starts at `EG_LOG2_FLOOR` (−40 octaves) and
`env_dx_step_block()` ramps *linearly in the log domain*, so an "instant" attack has to
traverse 40 octaves of inaudibility first — about 16 ms at rate 99. Dexed never traverses
it: its rising branch jumps straight to `jumptarget = 1716` and then uses an exponential
approach. The 1716 jump **is** the DX7's instant attack. This also accounts for most of the
20 ms time-to-peak measured in §5.2.

The other EG cases: `eg/very-slow-decay` 54 dB mean error (§1.1(d)'s `step<1` clamp),
`eg/slow-attack` 62 dB with its worst point at the 2 s release boundary, `eg/low-outlevel`
5.3 dB (the TL composition — Dexed folds output level into the EG's own target via
`(scaleoutlevel(L)>>1)<<6 + outlevel_ − 4256` with a min-16 clamp; t00t adds it separately
as `static_log2` with no bias term and no clamp).

**Velocity sensitivity is ~3× too shallow.** At sensitivity 7, velocity 0: Dexed −3344
units (−78.6 dB), t00t −1024 (−24.0 dB). `eg_vel_sensitivity_log2()`'s hand-rolled
`EG_VEL_SENS_MAX_OCTAVES = 4.0` linear-in-velocity model is not the DX7 curve. 894/1024
rows differ.

**Three LFO waveforms are half a cycle out of phase.** Triangle, square and sine each score
0.50–0.98 mean error as-is but **0.015 when rolled half a cycle** — i.e. they are exactly
inverted. The sawtooths are correct, which is what proves this is not a global phase-origin
error (a roll would break those instead): Dexed's own saw formulas carry a `^ (1<<31)` that
t00t's plain `t` / `1−t` omit, and that omission is what cancels for the saws and doubles
for the rest. `lfo.h`'s comments say each waveform was "reasoned through against Dexed's
real per-waveform bit tricks" — they were reasoned, not measured, and three of six came out
inverted. That is the whole thesis of this document in miniature.

**The LFO delay ramp is wrong** (`lfo/delay-60` 0.14, `lfo/delay-99` 0.42 mean error);
`dx7_lfo_delay_seconds()` collapses Dexed's two-stage accumulator into one, which its own
comment flags as a deliberate simplification. Now measurable.

### 5.5 What F1 changes about the plan

Nothing structural, but it sharpens the scope. `pitch_eg.h` is **correct** and needs no F6
work beyond keeping the test green — F6 shrinks to the LFO. The algorithm table is
confirmed, so F4 reduces to the feedback fix plus the algorithms 4/6 decision. And the three
integer tables #48/#58 ported are all exact, which is worth stating plainly: those two
issues did real, correct work, and the reason the engine still sounded wrong is entirely in
the pieces around them.

Deferred from F1 to F5 as planned: operator frequency conformance (ratio, detune, fixed
mode). Dexed's `osc_freq()` is a private method with no public accessor, so that test needs
an isolated single-operator render measured spectrally in cents rather than a table diff —
it belongs with the phase that actually changes those parameters.

### 5.6 F2 — the fixed-point contract

Deleted: `FM_OUT_SHIFT_CARRIER`, `FM_OUT_SHIFT_MODULATOR`, `FM_MOD_INPUT_SHIFT`,
`FmOpParams::level`, `FM_CARRIER_LEVEL_REF`, `FM_MODULATOR_LEVEL_REF`, and
`syx2patch.py`'s carrier-count / modulator-fan-in division. Six free parameters and one
attenuation real hardware never applied.

Added: one anchor, measured from Dexed rather than assumed (`tools/fm_ref`, probe against
the pinned source) — `Sin::lookup` is full-scale 2²⁴, Dexed's maximum operator gain is
exactly 2.0, and 24 bits is one cycle of its phase, so **a max-level operator produces
exactly two full cycles of phase deviation** and a unity-gain one exactly one. In t00t that
is `FM_CYCLE = 2²⁶` (one cycle, in bus units), `FM_GAIN_MAX = 2²⁸` (derived from it and the
sine table's 2¹⁵ full scale, not chosen), `FM_MOD_SHIFT = 6` (bus units → the 32-bit phase
accumulator) and `FM_VOICE_OUT_SHIFT = 14` (the one master headroom choice, applied once).
Carriers and modulators are the same number on the same scale; a carrier's output is simply
read as audio instead of as phase.

**Level gap, ROM1A × 32 at C3** — the metric F2 exists to move:

| | before (F0 baseline) | after F2 |
|---|---|---|
| Best | −12.1 dB | **−0.4 dB** |
| Typical | −13 to −30 dB | **−0.4 to −6.2 dB** (27 of 32 patches) |
| Worst | −95.9 dB | −81.4 dB (4 patches, see below) |

The four remaining outliers — STRINGS 1/2/3 and VOICE 1, plus TAKE OFF — are **not** a
scaling problem. Re-rendering them with an 8 s gate instead of 2 s closes them from
−77.8/−76.0/−77.7/−45.7 dB to **−3.1/−1.5/−2.3/+6.4 dB**. They are slow-attack patches whose
envelope cannot get off the floor inside a 2 s note, which is §5.4's 16 ms-to-open attack
defect showing up as a level error. F3 owns them.

So the honest statement of F2's result: **every ROM1A patch now lands within a few dB of the
reference once its envelope is given time to open.** No per-patch tuning, no compensating
constants — one anchor.

**Overflow, proven rather than sampled.** `render_fm_patch --check` bounds every bus
exactly: each operator's contribution is at most `2 × FM_CYCLE` (the gain ceiling times the
table's full scale, through `fm_mul_gain`'s fixed shift), so a bus's worst case is its
writer count times that, countable from the resolved routing with no dependence on what any
note does. Worst case across all 32 patches is E.ORGAN 1's six carriers on one bus:
2²⁹·⁵⁸, **1.42 bits of int32 headroom**. Compare §1.1(a)'s old behaviour, where a
max-level modulator reached 2³³·⁴ and wrapped the phase accumulator several times per sample.

**Where the F2 gate was not met, and why it was mis-specified.** §4 asked for spectral
centroid error to "drop materially on the bright patches". It did not — the mean centroid
ratio moved 1.87× → 1.99×, mixed per patch (ORCHESTRA 0.93→1.14 and HARPSICH 1.45→1.17
improved; BRASS 3 1.55→3.17 and E.PIANO 1 2.12→2.72 got worse). That criterion was wrong to
put here. Brightness in FM *is* modulator level over time, so it cannot be right until the
envelopes are, and §5.4 measured those as badly broken in both rate and landing level. The
centroid criterion belongs to F3, and is restated there. Harmonic MAE (18.9 → 18.5) and
envelope MAE (33.6 → 29.5) likewise barely moved, for the same reason and as expected.

Regression checks: F1 unchanged at 12/24 (F2 touches no curve F1 measures), and
`tools/test_syx2patch.py` 20/20, with the two fan-in-division tests replaced by tests that
the converter now emits no gain constant at all.

Left deliberately alone: the feedback `>> fb_shift` vs Dexed's `>> (fb_shift + 1)`
(§1.1(c)), so that this scorecard delta attributes cleanly to the scaling contract and
nothing else. It is F4's one-character fix.

### 5.7 F3 — the envelope

`env_dx.h` replaced wholesale with a direct port of Dexed's `Env` (`Source/msfa/env.cc`),
in Dexed's own Q24-octave level domain, with two documented deviations: the control-block
size (this engine steps every `FM_BLOCK`=16 samples, Dexed every 64 — `inc` is stored in
Dexed's units and scaled to the real block length, so a 64-sample block reproduces Dexed
exactly) and the output gain anchor (`eg_to_gain()` peaks at F2's `FM_GAIN_MAX` instead of
Dexed's 2.0). Everything else is the same arithmetic.

What that fixed, in order of size:

- **The rising stage is a different curve family.** Dexed jumps to `jumptarget` (1716) and
  then approaches the target exponentially; the old file ramped linearly in both directions
  and had to cross a 40-octave floor first. Attack to −1 dB at rate 99: **16.3 ms → 0.0 ms**,
  matching Dexed's first control block.
- **Output level belongs inside the envelope.** `advance()` folds it into each stage target
  (`(scaleoutlevel(L)>>1)<<6 + outlevel − 4256`, floored at 16); the old engine carried it
  alongside as `static_log2` and added it afterwards, which is what put E.PIANO 1's sustain
  ~60 dB low. `FmOp::static_log2` and `FmOp::rate_scale_qrate` are both gone.
- **Velocity was a hand-rolled model.** Replaced by a port of `ScaleVelocity`. F1's
  `table/velocity` went from 894/1024 rows differing to **1024/1024 identical**.
- **`staticcount`** — Dexed's measured dwell table for stages with nowhere to go — was
  missing entirely, so same-level stages completed instantly.
- **The `step < 1` clamp is gone**, and the formulation no longer needs it: the smallest
  possible `inc` spans the full 15-octave range in about six minutes.
- **`EG_LEVEL_THRESH`** (Dexed's `kLevelThresh`) added. A DX7 envelope floors around −90 dB
  and never reaches zero; Dexed reaches real silence by skipping operators under the
  threshold. Without it "the voice is finished" would be true of the stage counter but not
  of the audio.

### 5.8 Where the numbers landed

ROM1A × 32 at C3, means across the bank:

| | F0 baseline | after F2 | after F3 |
|---|---|---|---|
| Harmonic MAE | 18.9 dB | 18.5 dB | **5.5 dB** |
| Attack timbre MAE | 26.8 dB | 25.8 dB | **6.3 dB** |
| Envelope MAE | 33.6 dB | 29.5 dB | **2.0 dB** |
| Spectral centroid ratio | 1.87× | 1.99× | **1.43×** |
| Level gap (range) | −95.9 … −12.1 dB | −81.4 … −0.4 dB | **−3.9 … +0.6 dB** |

Gate, item by item:

- **F1's six `eg/*` cases within tolerance** — met. All six pass at ≤0.02 dB mean error;
  F1 overall went 12/24 → **19/24**, with every remaining failure in the LFO (F6).
- **The four slow-attack level outliers close at a 2 s gate** — met. STRINGS 1/2/3 and
  VOICE 1 went from −77.8/−76.0/−81.4/−77.7 dB to −0.4/−0.0/−0.6/−0.1 dB.
- **Spectral centroid approaches 1.0×** (moved here from F2) — substantially met: 1.99× →
  1.43× mean, and 20 of 32 patches now sit within 1.0–1.5×. Not clean: MARIMBA 3.31×,
  TIMPANI 4.12×, SYN-LEAD 1 2.16× remain bright. Of the seven brightest, **five have
  feedback level 7 and one has 6** — consistent with §1.1(c)'s 2× feedback error, which is
  still deliberately in place. F4 should move these; if it does not, they need their own
  investigation rather than another pass at the envelope.
- **Envelope metrics within 10% on E.PIANO 1 and TUB BELLS** — met on attack, sustain and
  release T60 (E.PIANO 1: attack 0.0/0.0 ms, sustain −7.4/−8.5 dB, T60 557/522 ms; TUB BELLS
  within 3 dB on sustain), but **`peak_time_ms` reads 754 ms vs 41 ms on E.PIANO 1 and that
  is a bad statistic, not a defect.** Both envelopes are flat to within a couple of dB
  across the whole gate (dexed −1.5/−2.8/−0.3/−9.9 dB at 20/200/800/2000 ms; t00t
  −0.0/−0.6/−1.7/−9.3), so "time of the maximum" is decided by a ~1 dB ripple and moves
  freely. Envelope MAE of 1.9 dB on that patch is the number to trust. `peak_time_ms` should
  be reported but not gated on.

Regressions: `tools/test_syx2patch.py` 20/20, and `tools/host_render/render_fm.cpp` (the
older cmake host suite) back to ALL PASS — its `run_level_table_checks` and
`run_key_rate_scaling_check` were removed, since both asserted internals of the model F3
deleted and `fm_ctl_diff.py` now covers exactly that ground against Dexed instead.

---

## 6. The recommendation that matters most

Commit the F7 scorecard thresholds to the repo.

The original problem was never the DSP — it was that the feedback loop ran through your ears
and your memory, so nothing was ever pinned down and every fix could silently regress an
earlier one. A checked-in threshold file makes timbral regression a failing command instead
of a listening session. That is the durable fix, and it is worth more than any individual
curve in §1.1.

Your hardware listening tests then become what they should be — confirmation at a
checkpoint, not the debugging instrument.
