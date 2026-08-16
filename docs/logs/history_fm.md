# T00T — FM Module, Attempt 2: Evaluation & Development Plan

Development history: the "Attempt 2" rewrite evaluation for the FM module. It
superseded the execution order in what is now `module_fm.md` (then `fm.md`, branch
`fm`) at the time, not its design; kept here as the record of how the module's
numeric core was re-derived against a Dexed reference harness. See `module_fm.md`
for the current spec. Written after reading the `fm` branch source, `module_fm.md`,
`engine.md` (whose #41–#59 build/measurement sections were later moved into §7 of
this document by a docs reorg), and issues #5, #41–#59, and after verifying three
specific divergences against the real Dexed source (`Source/msfa/`).

**Headline recommendation: do not rewrite the module, and do not keep patching it.
Re-derive the ~600-line numeric core against a Dexed reference harness, inside the
existing scaffolding — and build the harness *first*.**

---

## 1. What actually went wrong

The `fm` branch is not a bad implementation. The architecture is sound, the routing
compiler is correct, the sysex parser is well tested, and the P0 hardware measurements
are real and hard-won. The failure is narrower and more specific than "it sounds wrong".

`module_fm.md`'s phasing put the Dexed calibration pass at **P6 — dead last** (issue #53, still
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
| **Keep as-is** | `engines/fm/engine.h`, `audio_engine.cpp`, `midi_controller.cpp`, `display.cpp`, `render.h`, `sine_tab.h`, `rig.h`; `tools/host_render/` CMake scaffolding; `syx2patch.py`'s *parser* half + all 515 lines of `test_syx2patch.py`; the P0/#43/#45 measurements in §7 below; CMake/Makefile engine wiring | Untouched. This is the expensive, correct part. |
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

**F1 · `tools/fm_ctl_diff.py`** — the exact control-plane tests. Dexed dumps CSV
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

**One of these deserves an early check rather than an assumption.** `module_fm.md` §3.5 justifies
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
`module_fm.md`'s: **the calibration pass (#53) moves from last to first.**

| Phase | Deliverable | Gate |
|---|---|---|
| **F0** | Reference rigs. Build `dexed_render`, `render_fm_patch`, `fm_compare.py`, bank fetch script. **No engine changes.** Plus the §3.2 interpolation A/B. | **Met, §5.** Scorecard runs end-to-end on ROM1A #11 (E.PIANO 1) C3, Dexed vs the **current `fm` branch**. That baseline number is the thing everything after is measured against — and it retroactively quantifies how far off the current build is, which no amount of listening ever did. |
| **F1** | Control-plane conformance harness (`fm_ctl_diff`). | **Met, §5.4.** It runs, and **fails loudly** on the current `env_dx.h` — i.e. it independently rediscovers §1.1(b) and (d) without being told. If it passes, the harness is wrong. |
| **F2** | **Met (level/overflow), §5.6.** **Fixed-point contract.** Delete `level`, `FM_OUT_SHIFT_CARRIER`, `FM_OUT_SHIFT_MODULATOR`, `FM_MOD_INPUT_SHIFT`, and both `*_LEVEL_REF`s. Re-derive `op.h` + `patch.h` + `syx2patch.py`'s emit half around *unity gain = one full cycle of phase deviation*. Carriers and modulators share one scale; the carrier attenuation happens once, at the voice mix. | Level gap collapses to a few dB across the bank (met: -0.4..-6.2 dB on 27/32, the rest slow-attack patches F3 owns). No int32 overflow at max level on any ROM1A patch, proved by bound not sample (met: 1.42 bits headroom). **The spectral-centroid criterion originally written here was moved to F3** -- brightness is modulator level over time, so it cannot be judged while the envelopes are broken. |
| **F3** | **Met, §5.7-5.8.** **`EnvDX` rewrite.** Exponential-approach rising stage with the 1716 jump target, linear falling, `advance()`'s real level+TL composition (`(scaleoutlevel(L)>>1)<<6 + outlevel − 4256`, min-16 clamp), the real rate derivation, `staticcount` handling for slow rates, and remove the `step<1→1` clamp. | F1's six `eg/*` cases within tolerance. F0 envelope metrics (time-to-peak, T60) within 10% on E.PIANO 1 and TUB BELLS. **Spectral centroid** (moved from F2) approaches 1.0× on the bright patches. The four slow-attack level outliers (§5.6) close at a 2 s gate. |
| **F4** | **Met, §5.9-5.10.** Feedback `>>(fb_shift+1)` fix; exact conformance test of all 32 algorithms against Dexed's table. Decide algorithms 4 and 6 (old X1/#54). | F1 routing table exact (met since F1: 192/192). Scorecard green on the feedback-heavy patches (met: SYN-LEAD 1 24.3 → 0.6 dB, ORCH-CHIME 18.2 → 1.3 dB). Algorithms 4/6 resolved as **not needed** — Dexed does not implement the second loop either. |
| **F5** | **Met, §5.11-5.13.** Key level scaling, rate scaling, velocity, detune, fixed-frequency. | F1 exact for key/rate/velocity (already met). New `tools/fm_freq_diff.py` covers detune + fixed-frequency, which cannot be table-diffed: **6/6, worst 0.18 cents** (was 11.90). Key scaling verified across five octaves. |
| **F6** | **Met, §5.14-5.17.** LFO re-verification against `lfo.cc` (F3 had already settled `pitchenv.cc`). | **F1 exact — 24/24, the first time it has been.** Sync phase + saw rotation, the two-stage delay accumulator, amp mod moved to the log domain, and #49's mod-wheel convention reversed to Dexed's `max()` rule. Scorecard green on the vibrato/tremolo patches (TRAIN harmonic 14.7 → 3.4). Two EG paths that no test reached are now covered (§5.15), and the largest single remaining defect turned out to be a converter override, not DSP (§5.16). |
| **F7** | **Host half met, §5.18-5.21.** Full-bank regression + committed thresholds. Hardware half (listen, c/f/voice) pending a board. | 1600 patch renders per run across all four ROM banks × 5 note/velocity configs, gated by `tools/fm_thresholds.json`: **harmonic 0.53 dB, attack 1.53 dB, envelope 0.34 dB** (from 18.9 / 26.8 / 33.6 at F0). Four out-of-range factory bytes resolved as pass-through, measured not assumed (§5.18). Feedback depth conformance added (§5.19). **Modulator fan-out for algorithms 19-25 fixed — 10% of factory voices had two thirds of their modulation silently dropped** (§5.20). |
| **F8** | Performance retune → final `MAX_VOICES`. | Measured, in cycles/frame/voice — the convention §7's device measurements use. |

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

module_fm.md §3.5's −72 dBc estimate is right at β ≤ 0.5 and **wrong by 10–20 dB in the
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

### 5.9 F4 — feedback, and the algorithms 4/6 question

**The feedback fix is one character**, and F2's shared scale is what made it provable rather
than a judgement call. Dexed's `compute_fb` uses `(y0 + y) >> (fb_shift + 1)`; `op_render_fb`
used `>> fb_shift`. Working both sides in cycles-of-phase-deviation:

| feedback level | Dexed | t00t before | t00t after |
|---|---|---|---|
| 7 | 1.000 cycles | 2.000 | **1.000** |
| 4 | 0.125 | 0.250 | **0.125** |
| 1 | 0.016 | 0.031 | **0.016** |

Exactly 2× at every level — an octave too much feedback everywhere. Feedback is what a DX7
patch's edge is voiced around, so being 2× deep on it reads as a general excess of
brightness rather than as a feedback problem, which is why ears never located it.

F3 predicted this: of the seven brightest patches, five had feedback 7 and one had 6. All of
them moved. SYN-LEAD 1 went from 24.3 dB harmonic error and 2.16× centroid to **0.6 dB and
1.07×**; ORCH-CHIME 18.2 → 1.3 dB; E.PIANO 1's attack timbre 24.2 → 0.9 dB.

**Algorithms 4 and 6 (old X1 / #54): nothing to implement.** The plan said "implement the
interleaved kernel, or document the approximation and its measured cost". It turns out
there is no approximation. Dumping Dexed's own algorithm table shows that in both
algorithms the *secondary* feedback operator has `fb_out` set but **not** `fb_in`:

```
alg 4 op 0: in 0 out 1 fb_in 1 fb_out 1      <- primary, takes compute_fb
alg 4 op 2: in 1 out 0 fb_in 0 fb_out 1      <- secondary, fb_in is 0
alg 6 op 0: in 0 out 1 fb_in 1 fb_out 1
alg 6 op 1: in 1 out 0 fb_in 0 fb_out 1
```

and `fm_core.cc` gates the feedback kernel on `(flags & 0xc0) == 0xc0` — both bits — with
`// todo: more than one op in a feedback loop` on the line above it. **Dexed does not
implement the second loop either.** t00t dropping it is matching the reference exactly, not
approximating it.

Measured, on the seven patches across all eight ROM banks that use these algorithms:

| patch | bank | | harmonic MAE | centroid | level |
|---|---|---|---|---|---|
| CLAV 2 | rom1b | alg 4, fb 5 | 3.9 dB | 0.97× | +0.2 dB |
| CLAV 3 | rom1b | alg 4, fb 0 | 3.9 dB | 1.00× | −0.2 dB |
| PIPES 4 | rom1b | alg 6, fb 0 | 1.2 dB | 1.15× | −0.2 dB |
| RECORDER | rom2a | alg 6, fb 5 | 1.0 dB | 1.02× | −0.3 dB |
| CHIMES | rom2a | alg 6, fb 7 | 1.2 dB | 1.01× | +0.5 dB |
| COW BELL | rom2a | alg 6, fb 0 | 0.1 dB | 2.01× | −0.4 dB |

(rom1b bank mean 1.0 dB, rom2a 1.3 dB.) CHIMES at *maximum* feedback sits at the bank mean,
and CLAV 3 at feedback 0 scores identically to CLAV 2 at feedback 5 — so whatever the
3.9 dB on the CLAVs is, it is not the missing loop. **#54 should close as "not needed"
rather than stay deferred.** The caveat worth stating: this makes t00t match Dexed, not
necessarily real DX7 hardware. If the hardware does implement the second loop, both are
wrong together — which is in scope, since this project's stated target is Dexed.

### 5.10 Scorecard after F4

ROM1A × 32 at C3:

| | F0 | F2 | F3 | F4 |
|---|---|---|---|---|
| Harmonic MAE | 18.9 dB | 18.5 dB | 5.5 dB | **1.9 dB** |
| Attack timbre MAE | 26.8 dB | 25.8 dB | 6.3 dB | **2.3 dB** |
| Envelope MAE | 33.6 dB | 29.5 dB | 2.0 dB | **1.9 dB** |
| Centroid ratio | 1.87× | 1.99× | 1.43× | **1.27×** (1.16× over the patches where it applies) |
| Worst level gap | −95.9 dB | −81.4 dB | −3.9 dB | **−3.9 dB** |

Two harness fixes landed with this phase, both of which would otherwise corrupt later
measurements:

- **`Makefile.fm` did not list the engine headers as dependencies.** The engine is
  header-only, so `make` saw nothing to rebuild and F4's first scorecard came back
  byte-identical to F3's. It now depends on a wildcard over `src/engines/fm/*.h` so a new
  header cannot be forgotten. Every measurement in §5.6–5.8 was re-verified after the fix.
- **`harmonic_coverage`** added to `fm_compare.py`. Several DX7 patches are deliberately
  inharmonic — MARIMBA's 4.52 ratio, TIMPANI's 0.78, STEEL DRUM's fixed 398 Hz operator —
  so the harmonic tracker samples mostly noise floor and the centroid swings wildly while
  the tracked content matches almost exactly (TIMPANI: 3.80× centroid, **0.1 dB** harmonic
  MAE). Coverage is now reported, the summary marks those rows with `*`, and the aggregate
  centroid is taken only over the 18 of 32 patches where it means anything.

Remaining outliers, for F5 to look at: **TRAIN** (14.6 dB harmonic, 16.1 dB envelope) has
two fixed-frequency operators (977 Hz, 372 Hz), and fixed-frequency handling is exactly F5's
subject — a good canary. **VIBE 1** (10.3 dB envelope) and the two CLAVs (3.9 dB) have no
obvious explanation yet and should not be assumed to fall out of F5.

Also found while sweeping the banks: `syx2patch.py` refuses ROM3A voice 19 with
`eg_level3=127 out of range [0,99] -- corrupted sysex data`. That is the converter's
deliberate fail-loud policy meeting a genuinely out-of-spec byte in the published dump
(Dexed's own `normparm` clamps instead). Worth a decision before F7's full-bank regression:
clamp with a warning, or skip the voice.

### 5.11 F5 — frequency, key scaling, and two dropped fields

Key level scaling, rate scaling and velocity already passed F1 as exact table
diffs (55040 / 1024 / 1024 rows), so F5's real work was the piece deferred out of
F1: **detune and fixed-frequency**, which cannot be table-diffed because Dexed's
`Dx7Note::osc_freq()` is private.

**The test.** `tools/fm_ref/make_freq_bank.py` emits synthetic 32-voice banks whose
every voice is a single audible carrier on algorithm 32 — a bare sine whose frequency
*is* the thing under test. `tools/fm_freq_diff.py` renders both engines and compares
the spectral peak in cents. Both sides consume the bank the same way a factory ROM is
consumed, so this covers `syx2patch.py`'s conversion as much as the engine. The
standard stays exact; only the method differs from F1's table diffs.

Run before touching anything, it found what §1's reading of `osc_freq()` predicted:

| test | before | after |
|---|---|---|
| `freq/coarse@n48` | 0.08 cents worst | 0.08 |
| `freq/fine@n48` | 0.04 | 0.04 |
| `freq/detune@n24` | **11.90** | **0.18** |
| `freq/detune@n48` | 3.93 | 0.03 |
| `freq/detune@n84` | 2.26 | 0.01 |
| `freq/fixed@n48` | **6.68** | **0.10** |

Coarse and fine were already exact (`coarsemul[]` is precisely the 0.5-or-coarse
ratio in log form — checked against the table). The two failures were both detune:

- **Ratio-mode detune is note-dependent and was being baked flat.** Dexed's
  `detuneRatio = 0.0209 * exp(-0.396 * log2(f_note)) / 7` makes one unit of detune
  worth 2.46 cents at C1 falling to 0.68 at C6; `syx2patch.py` baked a flat 1.0
  cents/unit at conversion time, which is 11.9 cents out at C1. Detune exists to set
  the *beating rate* between operators sharing a ratio, so getting it wrong by a
  quarter-semitone changes exactly the thing the parameter is for.
- **Fixed-mode detune was dropped entirely.** Fixed operators use a different,
  sharpen-only rule (`detune > 7 ? 13457 * (detune - 7) : 0`, 0.962 cents/unit),
  worth up to 6.7 cents. The converter forced it to zero.

**The fix is structural, not a constant.** `FmOpParams::detune_cents` (float, baked)
became `detune_offset` (int8, the raw DX7 value minus 7), because a note-independent
converter *cannot* resolve a note-dependent parameter — that was the actual defect.
op.h's new `fm_op_base_inc()` applies both rules at note-on. While there, the
operator's whole neutral-pitch increment (ratio × detune, or the fixed frequency) is
now resolved once at note-on into `FmOp::base_inc`, so the per-block path is a single
multiply by the pitch bend/EG/LFO ratio instead of re-deriving ratio and detune every
block — slightly *cheaper* than before, not more expensive.

### 5.12 Key scaling, verified across the keyboard

Key level scaling and rate scaling only manifest *across notes*, and every scorecard
so far had been run at C3 alone. ROM1A × 32, means over the patches where the
harmonic metric applies:

| note | harmonic MAE | attack timbre | envelope MAE | centroid |
|---|---|---|---|---|
| 24 (C1) | 1.2 dB | 1.6 dB | 1.1 dB | 1.07× |
| 36 (C2) | 1.0 | 1.8 | 0.6 | 1.10× |
| 48 (C3) | 0.8 | 1.9 | 0.6 | 1.16× |
| 60 (C4) | 0.7 | 1.6 | 0.5 | 1.22× |
| 72 (C5) | 0.9 | 1.8 | 0.5 | 1.30× |

Flat across five octaves, which is the statement key scaling needed.

The detune fix also cleared most of what F4 left open. I wrote then that VIBE 1 and
the two CLAVs "should not be assumed to fall out of F5" — they largely did, and the
reason is obvious in hindsight: those are detuned-pair patches, and wrong detune is
wrong beating, which shows up in both the spectrum and the envelope. VIBE 1's envelope
MAE went 10.3 → 4.0 dB, CLAV 1's harmonic error 4.5 → 0.4 dB, PIANO 3 2.7 → 0.1 dB.
Bank-wide at C3: harmonic MAE 1.9 → 1.4 dB, envelope MAE 1.9 → 1.1 dB.

### 5.13 Two fields that were never emitted

Chasing TRAIN — F4's worst patch and its nominated fixed-frequency canary — turned up
something unrelated to frequency. TRAIN is driven by LFO tremolo (`amd` 99, AMS 3),
and rendering it with the mod wheel at 0 and at 127 produced **bit-identical output**.

`amp_mod_sens` was parsed from the sysex and present in `FmOpParams`, but had never
been carried into `syx2patch.py`'s output struct or its emitter — since #49. A C++
aggregate initialiser with one too few members simply zero-fills the rest, silently,
so **AMS was 0 for every converted patch and LFO amplitude modulation was dead
engine-wide.** Fixed; verified live (mod wheel now changes TRAIN's output by 0.5 full
scale).

The durable part is the guard: `test_every_op_field_is_emitted` renders a header and
counts the emitted values per operator against `FmOpOut`'s field count. Verified to
fail correctly — dropping `am_sensitivity` again reports *"16 values emitted per
operator but FmOpOut has 17 fields"*. This is the one bug class the whole
Dexed-comparison approach is blind to: a parameter that never reaches the engine looks
identical to a parameter the engine handles badly, and no amount of spectral scoring
distinguishes them.

**TRAIN itself is still 14.7 dB / 16.1 dB and is now firmly F6's.** With AMS live, its
tremolo runs on LFO waveform 0 (triangle) — which F1 measured as exactly half a cycle
out of phase, i.e. inverted. Inverted tremolo at `amd` 99 is precisely a large envelope
error. BRASS 1 and BRASS 2 (centroid 0.71× and 0.79×) use LFO pitch mod on waveform 4
(sine), inverted the same way. F6 should move all three; if it does not, they need
their own investigation.

Regressions: F1 19/24 unchanged, frequency 6/6, overflow bound holds, converter tests
21/21, legacy host suite ALL PASS (it needed a one-line update for the `fm_op_inc` →
`base_inc` rename). Device build clean, 48 `smlawb`, flash **54,492 bytes — 720 fewer
than F4**, since resolving ratio/detune at note-on removed work from the block path.

### 5.14 F6 — the LFO, and one wrong constant that looked like three broken waveforms

F6 was scoped as "LFO + pitch EG re-verification". F3 had already established `pitch_eg.h`
was correct, so it shrank to the LFO. All five of F1's remaining failures closed, and F1 is
now **exact for the first time: 24/24** (26/26 including the two cases §5.15 adds).

**The three "broken waveforms" were one wrong constant.** F1 reported triangle, square and
sine each exactly half a cycle out while both sawtooths passed. That combination is the
whole diagnosis, because it cannot be a global phase-origin error — a global error would
break the sawtooths too. Dexed's `keydown()` syncs to `phase_ = (1<<31) - 1`, the *middle*
of the cycle, and its two sawtooth cases carry a compensating `^ (1U << 31)`. #49 dropped
both halves of that pair: it synced to phase 0 and wrote the sawtooths unrotated. The two
errors cancel exactly for the sawtooths and for nothing else. Restoring both (one line in
`fm_lfo_trigger()`, one rotation in each saw case) took all three waveforms to 0.015 MAE.

Worth noting how the harness earned this. F1's shape test reports "matches when rolled half
a cycle" as an explicit diagnostic, which is why three separate-looking failures arrived
pre-correlated instead of as three shape mismatches to chase individually.

**The delay ramp was wrong in shape, not in constants.** `dx7_lfo_delay_seconds()` returned
one duration and the caller ramped linearly across it, documented as "a deliberate
simplification... the two-stage accumulator only exists to serve Dexed's own per-block Q32
arithmetic". It does not. Dexed's first stage is not a slow ramp — `getdelay()` returns
*exactly 0* for all of it. The real curve is **silence, then a ramp**, and the old code
turned it into one ramp spanning only the silent stage: it began opening while the reference
was still fully closed, and was fully open at the moment the reference starts to open. The
old comment also had the two stages backwards; `a &= 0xff80` looks like it can only shrink
`a`, but the `max(0x80, ...)` floor makes stage two *faster* wherever they differ (delay 99:
2.66 s closed, then 0.67 s opening). Ported as the real integer accumulator — the shape *is*
the arithmetic, so a seconds-domain re-derivation would only be this with extra steps. Both
delay cases now read **0.00 MAE, exactly**.

**#49's mod-wheel decision is reversed.** #49 made the wheel a 0..1 multiplier on the
patch's PMD/AMD and flagged the consequence honestly: "a patch with real vibrato/tremolo
configured will sound completely flat until the mod wheel is actually moved — expected, not
a bug." Measured, it is a bug: every factory patch with configured vibrato played with no
vibrato at the wheel's resting position. It now follows Dexed's real `max(pmod_1, pmod_2)` /
`max(amod_1, amod_2)` rule — patch depth always plays, the wheel is a separate source that
takes over above it. #49's acceptance criterion ("mod wheel scales LFO depth") still holds,
so this is a strict superset of the old behaviour.

This one deserves emphasis as a *method* failure, not just a defect. It was invisible to
every scorecard run before F6 because the renderer defaults to wheel 0, so both sides looked
quiet and agreed. That is the same shape as §5.13's dropped `am_sensitivity`: **a parameter
that never reaches the engine is indistinguishable from a parameter the engine handles
correctly, whenever the test happens to drive it to zero.** Two instances in two phases is a
pattern, and the guard for it is the same both times — assert the parameter *does* something,
not merely that both sides match.

**Amplitude mod moved from linear gain to the log domain.** #49 multiplied the tremolo into
the already-computed linear gain, "the natural place for it". Dexed subtracts it from the
operator's log-domain level *before* the exp lookup, and the two are not the same curve
rescaled: a fixed linear factor is a fixed dB attenuation, whereas Dexed's is proportional to
the current envelope level, so they diverge further the more the envelope has decayed. Ported
as-is, including two odd properties: the curve is non-zero at zero mod (~1 dB, which is why
Dexed guards on `ampmodsens != 0` rather than on the mod amount), and it scales with level.
**This is the one place where the reference is self-admittedly approximate** — Dexed's own
comment on this block is `// TODO: mehhh.. this needs some real tuning.` It is ported anyway,
because it is what we are measuring against, but it is flagged here as the first thing to
revisit if a hardware listen disagrees.

### 5.15 Two EG paths that no test reached

Chasing ROM1A #30 TRAIN's envelope error turned up a coverage gap rather than a defect. The
six EG cases from F1 all have `L2 != L1` and all have `L4 == 0`, so between them they never
enter `Env`'s `targetlevel_ == level_` branch — the `ACCURATE_ENVELOPE` `staticcount` hold,
which is the entire reason the `statics[77]` table exists — and never release *upwards*. Both
paths are ported in `env_dx.h`; neither was under test. TRAIN's two outlier operators happen
to be one of each, so `eg/static-hold` and `eg/rising-release` are taken directly from it.
Both pass (0.01 and 0.02 dB MAE), so the port was right — but it was right unverified for
three phases.

One process note from the same investigation: my first pass compared the two dumps by *row
index*. They run at different block rates (Dexed N=64, t00t FM_BLOCK=16), so row *i* is a
different time on each side, and the comparison manufactured a 27 dB "attack defect" that
does not exist. `fm_ctl_diff.py` resamples onto a common grid precisely to avoid this; the
lesson is to use the harness rather than reach past it.

### 5.16 The largest remaining defect was in the converter, not the engine

TRAIN's real problem: `syx2patch.py` forced any **carrier** with `L4 != 0` to `L4 = 0`, on
the stated grounds that "a nonzero carrier L4 never reaches `env_dx.h`'s `EG_IDLE`, so the
voice could never be reclaimed by the allocator (the tracker's #21 bug shape)".

That reason does not hold. `voice_alloc.cpp`'s `allocate()` has three tiers, and tier 2 is
"released voice (active on Core 1 but not gated — in release phase)", reached the instant the
key lifts. Such a voice is never stuck and can never make allocation fail; the only thing it
loses is eligibility for tier 1, the *inaudible* steal. The cost of the rule, meanwhile, was
real: a nonzero carrier L4 *is* a patch designed to keep sounding after key-off, and zeroing
it deletes that design. TRAIN is a train whistle whose entire point is that it carries on.

Removing the override takes TRAIN from **16.5 → 0.15 dB** envelope MAE — by far the largest
single defect left on the bank, and it was never in the DSP at all. It is also rare enough to
have hidden easily: **3 voices out of all 256** across the four ROM banks.

The general lesson is worth more than the fix. The converter is allowed to refuse data
(§5.11's fail-loud policy is good, and ROM3A voice 19 still trips it pending an F7 decision),
but *silently rewriting* data to protect a downstream invariant hides the trade in the last
place anyone looks for a timbre bug. The old behaviour's unit test is inverted rather than
deleted, so it cannot quietly return.

### 5.17 Scorecard after F6

ROM1A × 32 at C3, mean over the bank:

| Metric | F0 | F3 | F4 | F5 | **F6** |
|---|---|---|---|---|---|
| Harmonic MAE | 18.9 dB | 5.5 | 1.9 | 1.4 | **1.0** |
| Attack timbre MAE | 26.8 dB | 6.3 | 2.3 | 1.9 | **2.0** |
| Envelope MAE | 33.6 dB | 2.0 | 1.9 | 1.1 | **0.6** |
| Centroid (harmonic patches) | 1.87× | 1.43 | 1.27 | 1.16 | **1.16** |

Cross-bank, against the F4 baselines: ROM1B harmonic 1.05 → **0.33**, envelope 0.77 → 0.37;
ROM2A harmonic 1.29 → **0.76**, envelope 1.29 → 0.53. No patch regressed by more than 0.5 dB
on either bank.

Device build clean: **53,804 bytes flash — 688 fewer than F5**, because the integer delay
accumulator replaced a float divide per block. bss +64 bytes (16 voices × the new
`delay_state`). 48 `smlawb`, unchanged since F2 — the per-sample kernel is still untouched.

**What F6 did not fix, and a prediction that was wrong.** §5.13 predicted BRASS 1 and BRASS 2
would move with the LFO fix, since both use pitch mod on the inverted sine. They did not, and
the prediction was poorly reasoned: at PMD 5 / PMS 3 their vibrato is under 5 cents, nowhere
near enough to explain a 10 dB attack-timbre error. Their actual signature is much more
specific and points elsewhere — the fundamental is *up* ~3.5 dB while **every harmonic above
the second is down by a near-constant 4.5 dB** (BRASS 2: 3.0 dB). A flat, time-independent
deficit across the whole upper spectrum is a static modulation-index shortfall, not an
envelope or LFO shape error. `dx7_note_outlevel()` was checked against Dexed's composition
and matches, so this needs its own investigation in F7. PIPES 1 and GUITAR 2 have unrelated,
sparse per-harmonic errors and should be looked at separately.

**Harness hardening.** `render_fm` had no target in `Makefile.fm`, so `make -f Makefile.fm
render_fm` fell through to make's *implicit* rule — which drops `$(INCLUDES)` and cannot
compile at all, while a stale binary from an earlier manual build sat there passing. Same
failure that cost F4 a phase and nearly cost F5 one. It is a listed target now, with the same
`ENGINE_HEADERS` dependency as everything else.

### 5.18 F7 — four bytes the converter refused, and why clamping was wrong

The full-bank sweep could not start: `syx2patch.py` rejected ROM3A voice 19
(`eg_level3=127`), and then ROM3B and ROM4A for the same class of reason. A scan
of all 256 factory voices found **exactly four** out-of-range bytes — three EG
fields and one `freq_fine` — and, decisively, **every one of those banks has a
valid checksum**. This is Yamaha's own data, not a corrupt read.

Three dispositions were possible and only one is right. *Skip* loses a voice.
*Fail* blocks the bank, which is where we started. *Clamp to 99* is what Dexed's
own `normparm` does — and it is wrong here, measurably: ROM3A #19 TIMPANI scores
**1.5 dB** harmonic MAE with the byte passed through and **3.4 dB** clamped, with
its spectral centroid dropping to 0.75×. The clamp audibly darkens the patch.

Pass-through is correct because both engines consume these fields through
functions that are *total* over 0..127 and agree on the result: `scaleoutlevel()`
is `28 + level` above 19, rates go through `min(qrate, 63)`, and the frequency
ratio is arithmetic (`coarse_ratio × (1 + fine/100)`) rather than a lookup. So
127 is a real, louder-than-the-front-panel-can-enter level, not undefined
behaviour. The fail-loud policy is unchanged everywhere else; it is relaxed only
where the reference is defined over the wider range, and a warning still records
that the patch is off-panel.

### 5.19 The feedback loop is right, including where it cannot be compared

BRASS 1's error (§5.17 handed it to F7) turned out to depend entirely on
feedback: at feedback 7 it scored 4.5 dB, at levels 0–3 it scored 1.0, a clean
monotonic ramp. Feedback *depth* had never been tested — F4 verified the
32-algorithm table exactly, but "which operator has feedback" is all that table
says. `tools/fm_ref/make_fb_bank.py` closes that: synthetic banks where one
operator's self-feedback is the only thing sounding, swept across all 8 levels,
at two output levels, both as a carrier and as a modulator.

Result: **0.1–0.2 dB at every level except 7 at full output**, in both modes. So
the loop arithmetic is right, and the remaining case needed explaining rather
than fixing. Two measurements settled it. First, the waveforms: peak-identical
(2.0000 against 2.0001) and agreeing to ~0.007 through the smooth stretches,
diverging only in the wild ones — feedback 7 at full gain drives the loop
**chaotic**, and no two implementations can track each other there. Second, the
right instrument for a noise-like signal: averaged spectra agree to **0.82 dB**
with RMS matched to 0.1%. The scorecard already flags this itself — harmonic
coverage falls from 49% at feedback 0 to 9% at feedback 7.

Two hypotheses died on the way, both worth recording because both were plausible
and both were wrong. Sine-table interpolation was implemented and measured:
**no change at all** (3.6 dB before and after), so it was reverted rather than
kept on the theory that it ought to help — F0's §5.3 accounting still stands.
And "BRASS 1 is chaotic too" was tested with a control — Dexed against Dexed with
a one-cent detune nudge — which came back at **0.3 dB**. BRASS 1 was stable, so
its error was real, and that ruled-out answer is what forced the next section.

### 5.20 The largest defect in the engine: dropped modulator fan-out

`FmOpParams::mod_target` names **one** operator. DX7 algorithms 19–25 have one
modulator driving two or three carriers at once: in Dexed, OP6 writes a scratch
bus and OP5, OP4 and OP3 each read it. `decode_algorithm()` modelled a bus as
emptied by its first reader (`pending[ib] = []`), so it kept the first edge and
**silently discarded the rest** — on **25 of the 256 factory voices (10%)**.

The audible cost was large and exactly what "does not sound correct" sounds like:
two of BRASS 1's three carriers were never modulated at all, leaving it **7.1 dB
short across its whole upper spectrum** on an averaged-spectrum comparison.

This is the most instructive bug of the whole rewrite, because F1's
`table/algorithms` case has compared the algorithm bytes since F0 and passed
**192/192 every single time**. It was comparing the *input* to a lossy decoder.
Identical inputs, faithfully decoded wrongly, still compare identical. The fix is
therefore as much a test as a patch: `table/routing` reconstructs Dexed's real
bus semantics and compares the decoded **modulation graph**, and it was verified
to fail correctly — re-inserting the clear-on-read line reports exactly
`7/32 algorithms decode to the wrong graph` with the precise missing edges.

The engine change is small because `fm_voice_render_block` was already general —
`in_bus`/`out_bus` are independent per operator over six buses, so the renderer
could always express fan-out; only the resolver could not. `FmOpParams` gains one
byte, `extra_target_mask`. Fan-out is kept as a mask on the *source* rather than
flipping the bus convention to source-indexed, because fan-**in** (algorithm 7
sums OP5 and OP4 into OP3's modulation) needs the receiver-indexed form to work
at all. That is safe because the two shapes provably never collide: across all 32
algorithms, no operator that is the target of a fan-out ever has a second
modulator — checked exhaustively, not assumed.

Adding the field also broke `FM_TEST_PATCH`'s aggregate initialiser — which the
compiler caught, unlike §5.13's dropped `am_sensitivity`, purely because the next
field along is an array rather than another integer. Same latent trap, caught by
luck; the emitted-field guard from F5 is what actually covers it.

Effect on ROM1A: BRASS 1 **4.5 → 0.2 dB**, BRASS 2 2.9 → 0.2, PIPES 1 4.1 → 0.2.
Bank mean harmonic 1.0 → 0.6, attack 2.0 → 1.1, envelope 0.6 → 0.4.

### 5.21 The regression gate (§6, delivered)

`tools/fm_regress.py` + `tools/fm_thresholds.json`: **10 banks × 5 note/velocity
configs × 32 voices = 1600 patch renders per run**, scored against Dexed and
compared to committed numbers. `--update` re-baselines; plain invocation fails on
regression. Both the bank *mean* and the *worst single patch* are gated: a mean
alone lets one patch fall apart while 31 improve, a worst-case alone fails
whenever a single inharmonic patch's tracker wobbles.

The thresholds are measurements with headroom (30% or 0.35 dB, whichever is
larger), not aspirations — so the gate answers "did this change make something
worse?", which a regression suite can actually answer, rather than "is this good
enough?", which it cannot.

Across all 256 factory voices at five note/velocity configurations:

| Metric | F0 baseline (ROM1A, C3) | **F7 (all banks, all configs)** |
|---|---|---|
| Harmonic MAE | 18.9 dB | **0.53 dB** |
| Attack timbre MAE | 26.8 dB | **1.53 dB** |
| Envelope MAE | 33.6 dB | **0.34 dB** |

Device build clean: 54,588 bytes flash (+784 over F6 — the fan-out mask across 32
patches plus the resolver), bss unchanged at 211,984, 48 `smlawb`.

**What is left.** The remaining outliers are concentrated in attack timbre on
ROM2B and ROM4B (bank means 2.3 and 4.4 dB, worst single patches 25 and 53 dB at
the top of the keyboard), and they grow with pitch — 4.4 dB at note 72 against
2.2 at note 36 on ROM4B. That pitch dependence is a real lead and is where F8's
attention belongs. Everything else is at or below 1 dB.

**Not measured, and why.** `c/f/voice` is `(duty − idle) / voice_count` read from
the running device (§7's own convention for the #41–#49 device measurements), so
unlike every other number in this document it cannot be produced on the host. It is deferred to the hardware
session along with the listening test — which is the gate F7 was always going to
stop at.

### 5.22 F8 — hardware voice-count sweep (partial: no FX, 5/32 patches heard)

First real hardware numbers for the F7 engine, `breadboard_rp2350`, held notes,
no FX. Duty cycle converts to cycles/frame at 3401 c/f:

| voices | duty | cycles/frame | c/f/voice above idle |
|---|---|---|---|
| 0 | 2.10% | 71.4 | — (idle) |
| 1 | 7.8% | 265.3 | 193.9 |
| 2 | 13.1% | 445.5 | 187.1 |
| 4 | 23.8% | 809.4 | 184.5 |
| 8 | 45% | 1530.5 | 182.4 |
| 16 | 87.9% | 2989.5 | 182.4 |

A clean line, `cycles ≈ 71.4 + 182.4 × N` — exact at N=8 and N=16, off by only
~10 c/f at N=1/2/4 (plausibly duty-cycle read-out rounding at the low end, not a
second effect). Single-voice, per-patch (idle-subtracted, patches 1–5 of
ROM1A): **177–235 c/f**, patch-dependent (176.9/183.7/234.7/200.7/193.9 for
patches 3/2/4/5/1) — expected now, since F7's real per-patch algorithm,
feedback, and LFO content vary the actual per-block cost, unlike #43's uniform
synthetic rig.

Both numbers are well above what put `MAX_VOICES=16` on the table. #43's
kernel-only baseline was 100.5 c/f/voice; #45's EnvDX-only checkpoint was
137–154 c/f/voice. The complete F7 engine measures **1.2–1.7× #45** and
**1.8–2.3× #43**, which is exactly what §3.4's own sensitivity tiers were
written to catch: ≤130 c/f → 20+ voices, ~160 → 16 as planned, ≥200 → 12 with
6-op as opt-in. The measured range straddles the last two tiers, and patch #4
alone (234.7 c/f) clears into the ≥200 tier on its own.

**Budget, redone with today's numbers.** 16 voices measures 87.9% duty with
*no FX running*. §9's 85% ceiling was meant to leave room for reverb's own
measured 268.7 c/f (7.9%) *inside* that 85%, so 87.9% dry is already past it
before FX is added; with reverb it lands near 95.8% steady-state, and #45's own
bursty-retrigger delta (+8 pp over steady-state at 16 voices) would put that at
or past the 100% deadline — a real risk, not a thin margin. Re-solving the
ceiling with the measured idle and the old reverb figure: `0.85 × 3401 − 71.4 −
268.7 = 2550.75` c/f left for voices, which at 182–235 c/f/voice gives
**~11–14 voices**, not 16, as the reverb-safe count.

**Two things open before this can move #53's needle on `MAX_VOICES` itself:**
idle jumped from #43's 15 c/f (0.44%) to 71.4 c/f (2.10%) here — 4.7×, worth
confirming this is the same measurement convention (same idle condition, no
stray Core 0 traffic) before trusting the budget re-solve above, since that gap
alone is worth more than a third of the difference between 11 and 16 voices.
And there is no FX-on reading yet — everything above about reverb is the old
#43 figure carried forward, not measured against the current engine. `MAX_
VOICES` stays at 16 pending both, plus the rest of the by-ear pass (5/32 ROM1A
patches heard so far, reported as sounding "much more authentic" than attempt
1) — per #53's own acceptance criteria, this is a HITL gate, not a number to
set from a cycle count alone.

**A separate signal from the same session, resolved.** Mild distortion was
reported at 16 voices with no FX; §5.22 flagged two candidate mechanisms
(Core 1 deadline overrun vs. mix-stage summing headroom) since 87.9% dry duty
was close enough to saturated that a deadline miss was live, not a stretch.
The author's own follow-up settles it: on a sustained chord with LFO running, the
distortion's intensity tracks *volume*, which a deadline overrun would not do
(that would read as clicks/dropouts tied to retrigger timing, not a level-
dependent grit) — mix-stage headroom, not CPU. `MAX_VOICES` stays at 16;
carrier summing headroom is the thing to revisit in a later optimization pass,
not voice count or the budget re-solve above.

**Core 1 code placement, confirmed (module_fm.md §3.6 lever 2, open question 10).**
Checked directly rather than assumed: no `PICO_COPY_TO_RAM`/`copy_to_ram`
binary type anywhere in `CMakeLists.txt`, so the pico-sdk default (execute
in place from flash, through the RP2350's shared XIP cache) applies to the
whole firmware, Core 1's FM kernel included. `op.h`'s render loop is plain
`inline`, not wrapped in `__not_in_flash_func` — the comment at its own top
records *why*: #43 measured SRAM placement as +4.9 c/f/voice *worse*, not
better, because a flash-resident caller reaching SRAM-placed code needs a
linker veneer to cross the >256 MB gap outside a Thumb `BL`'s range, and nothing
in that trade offsets the veneer cost once the loop is small enough to live in
the XIP cache. (`rig.h`'s `__no_inline_not_in_flash_func` machinery is P0-rig-only,
`FM_RIG_NOT_IN_FLASH=1`, gated behind `FM_PROFILE=1` — not part of a normal
build.) Open question 10's own caveat still stands unresolved: #43's flash-vs-
SRAM measurement was taken with Core 0 doing essentially no flash traffic of
its own, and the 16 KB XIP cache is shared by both cores — real Core 0
MIDI/LCD work could evict the FM kernel's cache lines in a way #43 never
tested. `xip_cache_pin_range()` is the mitigation on record if that turns out
to matter.

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

---

## 7. Device build & measurement history (from `engine.md`, #41–#49, #57–#59)

The sections below were originally recorded in `engine.md` as FM's build-time
engine-skeleton and P0–P4 measurement history — moved here as part of the
docs reorg splitting current-state spec/usage from development history.
They run chronologically (#41 through #49, with the #57–#59 Dexed-porting
detour in between) and predate/overlap the Attempt 2 evaluation above
(§§1–6): #41–#45 are the original hardware build-out and P0 measurement
gate; #47's converter is where real DX7 data first exposed the modulation-
depth and envelope-curve problems that #57–#59 (and the Attempt 2 rewrite
itself) exist to fix; #48/#49 are the remaining DX7 parameter-set features.
Cross-references are repointed to `module_fm.md`/`history_subtractive.md` or
made same-file where the referenced section now lives in this document too.

### FM Engine (build skeleton, #41)

Fifth build-time engine (`make ENGINE=fm`, `breadboard_rp2350` default),
proving the build seam before any DX7 logic exists — `module_fm.md` says "start at
P0 and do not skip it," but P0's measurement rig needs a build target to
measure *on*, so this slice exists first, exactly as #13 preceded #15/#16 for
the tracker and #27 preceded #31 for speech:

- `MAX_VOICES = 16`, defined in `src/engines/fm/engine.h` ahead of its
  `#include "engine_base.h"`, per #10 — `module_fm.md` §3.4's working assumption for
  full 6-operator polyphony, explicitly **provisional pending the P0
  measurement gate**. Only this engine — the other four are untouched.
- Standard `ParamExchange`/`voice_alloc` path, same as speech and unlike the
  tracker: `module_fm.md` has no fixed channel→voice mapping, so there's no reason to
  deviate. `src/voice_alloc.cpp` links normally, no `CMakeLists.txt` override.
- `fx/delay.h`/`fx/reverb.h` **are** linked — `module_fm.md` §2: "FM's whole working
  set is ~12 KB... nothing in this design should introduce a dependency on
  PSRAM," and there's no sample-RAM pressure to protect either. Confirmed by
  the `.bss` table below: FM's 202,768 bytes sit alongside groovebox's
  199,764 and subtractive's 198,344 (both link delay+reverb), nowhere near
  stripped.
- `src/engines/fm/sine_tab.h` — the FM-specific 4096-entry `int16_t` operator
  sine table from `module_fm.md` §5.1: quarter-wave symmetric generation (only the
  first quarter computed with `sinf()`, the rest mirrored/negated into
  place) but the full table stored, indexed by `phase >> 20` with **no
  interpolation** (module_fm.md §3.5: interpolation costs ~45% more per operator for no
  audible benefit under FM's own harmonic density). Header-only inline
  variable (C++17), same pattern as `res2p.h`'s `res2p_radius_lut` — no
  separate `.cpp` needed, so the top-level `CMakeLists.txt`'s engine source
  list needed no changes at all (unlike the tracker's #13, which added
  `ENGINE_VOICE_ALLOC`/`ENGINE_TRACKER_PLAYER` overrides). `osc/sine.*`
  itself is untouched — the shared 1024-entry interpolating table stays
  exactly as the other four engines left it, and is still used here for
  `pan.h`'s quadrature pan-gain lookup (a different table, different job).
- `src/engines/fm/render.h`'s `fm_render_test_tone()` is the literal shared
  source between the device path (`audio_engine.cpp`, called from the Core 1
  render loop) and the new host target below — same shape as speech's
  `speech_render_test_tone()`, proving the table/phase seam identically on
  both before any operator kernel exists. No pico-sdk dependency (only
  `sine_tab.h` + `pan.h`).
- `src/engines/fm/display.cpp` and `midi_controller.cpp` are stubs (no UI, no
  patch/note logic yet) so the shared `gfx.cpp` path and MIDI transports
  still link — same reasoning as speech's #27 stubs, including staying off
  the shared `src/midi/midi_controller.cpp`, which expects a
  `presets.h`/`VoicePreset` shape this engine's minimal `VoiceParams`
  doesn't have. The `presets.h`-existence gate that broke on speech (#38) is
  unaffected — FM has no `presets.h` either, same as speech and groovebox at
  this stage.
- Sound source: voice 0 is a hardcoded, always-on 440 Hz test tone (centre
  pan), rendered through the FM sine table via `fm_render_test_tone()` at the
  full 44.1 kHz output rate (no ZOH — FM runs at the shared `SAMPLE_RATE`,
  unlike speech's half-rate native path) — a build/boot smoke test, not a
  synth. Voices 1–15 are unused placeholders. `PROFILE_PIN` (GPIO 22) is
  bracketed around the render call, ready for the P0 measurement slice.
- Host target: `render_fm` (`tools/host_render/render_fm.cpp`, built via
  `make host`) calls the identical `fm_render_test_tone()`, renders 2 s to
  `fm_test_tone.wav`, and checks the sine table is an odd function about the
  origin (`fm_sine_table[i] == -fm_sine_table[N-i]`) — the invariant its
  quarter-wave-symmetric construction depends on — rather than trusting it by
  ear. All checks pass as of 2026-08-08.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=fm`, alongside a fresh rebuild of all four other engines to confirm
#41 changed nothing about them:

| Engine | text | bss | dec |
|---|---|---|---|
| fm (skeleton) | 27,784 | 202,768 | 230,552 |
| subtractive (default) | 206,096 | 198,344 | 404,440 |
| groovebox | 55,580 | 199,764 | 255,344 |
| tracker | 58,124 | 409,080 | 467,204 |
| speech | 73,156 | 196,588 | 269,744 |

`make`, `make ENGINE=groovebox`, and `make ENGINE=tracker` reproduce their
#27-era table entries byte-for-byte, confirming #41 didn't touch them; the
speech figures have simply grown since #27 (P2–P4 landed in the meantime),
unrelated to this change.


### FM P0 Rig (#42)

`module_fm.md` §11: "Start at P0 — measure." This is the measurement rig itself, not
the measurement — it's blocked ahead of the actual bench session (a future
issue), the same relationship #16's tracker mixer rig and #31's speech
profiling rig had to their own follow-up bench passes. It **decides nothing**
about voice count, table size, BLOCK, or any other module_fm.md §3.6 lever; it only makes
all of them buildable and switchable in one bench sitting.

Self-contained in `src/engines/fm/rig.h`, entirely separate from #41's real
engine skeleton (`engine.h`/`sine_tab.h`/`render.h` are untouched) so nothing
here risks the already hardware-verified #41 build. Selected by `make
ENGINE=fm FM_PROFILE=1` (`T00T_FM_PROFILE`, same pattern as speech's
`SPEECH_PROFILE` — see `src/engines/fm/audio_engine.cpp`'s `#if`/`#else`),
with every `module_fm.md` §3.6 lever as its own compile-time switch, documented in
the Makefile next to `FM_PROFILE`:

| Switch | Values | Default |
|---|---|---|
| `FM_RIG_VOICES` | voice count | 24 (past the predicted 12–21 ceiling, module_fm.md §3.4) |
| `FM_RIG_BLOCK` | 8 / 16 / 32 | 16 |
| `FM_RIG_TABLE_BITS` | 10 (1024) / 12 (4096) | 12 |
| `FM_RIG_INTERLEAVE` | 0 / 1 | 0 |
| `FM_RIG_NOT_IN_FLASH` | 0 / 1 | 0 |
| `FM_RIG_SMULWB` | 0 / 1 | 0 |

`FM_RIG_NOT_IN_FLASH` only moves the kernel *code*; the table itself
(`fm_rig_table`) is a runtime-generated, non-`const` array — always `.bss`/
SRAM, never flash `.rodata` — so that axis was never independently
toggleable to begin with, and the lever really measures code placement only.
`FM_RIG_SMULWB` fuses the `gain >> 8` + multiply into one M33 instruction;
this GCC's `arm_acle.h` has no standalone `__smulwb` wrapper, only
`__smlawb` (multiply-*accumulate*) — `__smlawb(gain, sample, 0)` is exactly
SMULWB with the accumulate operand forced to zero.

**Topology.** No patch, no DAG compiler (`module_fm.md` explicitly: "no patch
logic") — a fixed 6-operator chain per voice, hand-assigned so every
`op_render`/`op_render_fb` accumulate (`+=`) is guaranteed a preceding
`op_render_first` store on the same bus (the property the real engine's
note-on-time router will handle later):

```
op0 -> bus0            first-writer  (op_render_first, in=zero)      \_ interleaved
op1 -> bus1            first-writer  (op_render_first, in=zero)      /  when FM_RIG_INTERLEAVE=1
op2 -> bus1 (+=)       accumulate    (op_render, in=bus0)
op3 -> bus1 (+=)       accumulate    (op_render_fb, self-feedback)
op4 -> bus3            first-writer  (op_render_first, in=bus1)
op5 -> OUT             first-writer  (op_render_first, in=bus3)   -- the voice's carrier
```

All three kernel variants land in a topology that's correct by construction
rather than by convention: bus1 gets three real writers (op1 first, op2 and
op3 accumulating on top), proving the first-writer/no-clearing claim isn't
just asserted but exercised. `bus2`/`bus4`/`bus5` are allocated (`FmRigBuses`
always carries all 6 mod + 1 out) but unused by this particular chain — real
algorithms with more parallel modulators would use them.

**Fixed increments/gains** (`fm_rig_init_voice()`): every operator gets a
harmonic-multiple frequency and a flat gain (`gain_step = 0` throughout —
the field and the kernels' `gain += gain_step` instruction still exist and
execute every sample, since that's the actual cost being measured; it's the
*value* that's fixed, not the instruction). Scaled well below unity so
`FM_RIG_VOICES` summed carriers don't just sit at flat `__ssat` clipping —
a rig that saturates for its whole run can't tell a healthy render from a
broken one by ear or by eye.

**Host correctness check** (`tools/host_render/render_fm_rig.cpp`, `render_fm_rig`
target): calls the exact `fm_rig_render_buffer()` the device's `T00T_FM_PROFILE`
branch calls, in device-sized chunks, and checks the output is non-silent and
its magnitude stays within a generous per-voice-unity bound (catching a real
accumulator overflow without false-triggering on ordinary saturation, which
device `__ssat()` handles the same way regardless). Verified locally against
every lever combination by compiling the driver directly with each `-D` (host
`cmake`/`make` only builds the default combination; sweeping the rest is a
compile-flag exercise, not something worth a matrix of CMake targets):
default, `FM_RIG_INTERLEAVE=1`, `FM_RIG_TABLE_BITS=10`, `FM_RIG_SMULWB=1`,
`FM_RIG_BLOCK=8/32`, `FM_RIG_VOICES=1/32` — all pass, and every non-default
lever reproduces the default's exact peak (7644), confirming each is a pure
performance/placement change with zero effect on the arithmetic.

**Device build.** `make ENGINE=fm FM_PROFILE=1` and a combined-levers build
(`FM_RIG_INTERLEAVE=1 FM_RIG_TABLE_BITS=10 FM_RIG_BLOCK=8 FM_RIG_VOICES=32
FM_RIG_NOT_IN_FLASH=1 FM_RIG_SMULWB=1`) both build clean:

| Build | text | bss | dec |
|---|---|---|---|
| fm, FM_PROFILE=1 (defaults) | 32,080 | 23,064 | 55,144 |
| fm, FM_PROFILE=1 (all levers combined) | 29,104 | 18,296 | 47,400 |
| fm, FM_PROFILE=0 (#41 skeleton, unchanged) | 27,784 | 202,768 | 230,552 |

The profiling build's `.bss` is far smaller than #41's skeleton because
`T00T_FM_PROFILE`'s branch doesn't link `fx/delay.h`/`fx/reverb.h` at all —
the rig has no use for the 128 KB delay line, and `module_fm.md` never asked this
slice to carry it. `rm -rf build && make ENGINE=fm` (no `FM_PROFILE`)
reproduces #41's exact 27,784/202,768/230,552 byte-for-byte, and `subtractive`/
`groovebox`/`tracker`/`speech` all rebuilt clean and unchanged from their #41
table.

**Emitted assembly vs. `module_fm.md` §3.2.** Extracted by compiling `rig.h`
directly with the flags `module_fm.md`'s own provenance note specifies
(`arm-none-eabi-g++ -O3 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard
-mfpu=fpv5-sp-d16 -std=gnu++17`) through a `noinline` wrapper, since
`op_render()` normally inlines completely into `audio_engine_run()` at `-O3`.
First attempt reloaded `op.inc`/`op.gain_step` from memory every sample (15
instructions, not 13) — the C++ source only hoisted `phase`/`gain`/`in`/`out`
into locals, not `inc`/`gain_step`, and GCC's strict-aliasing rules can't
prove an `int32_t*` write to the output bus doesn't alias those `FmRigOp`
fields, so it played it safe and reloaded them. Hoisting all four fixed the
gap — a real, source-level finding this exercise existed to catch, not a
compiler quirk to shrug off. The corrected `op_render()` loop body:

```
ldr   r3, [r2, #4]!         @ modulation bus in (pre-increment)
add   lr, lr, r7            @ phase += inc
add   r3, r3, lr            @ phase + modulation
lsrs  r3, r3, #20            @ table index (20, not module_fm.md's #22 -- this rig's
                              @ default is the 4096-entry table #41 settled on)
ldrsh fp, [r1, r3, lsl #1]  @ table lookup
asrs  r3, r4, #8             @ gain >> 8
mul   fp, r3, fp             @ x sample
ldr   r3, [ip, #4]!         @ output bus accumulate (pre-increment)
cmp   r5, r2
add   r3, r3, fp, asr #14    @ >>14, accumulate
add   r4, r4, r6             @ gain += gain_step
str   r3, [ip]
bne   .L3
```

**13 instructions, exact match to `module_fm.md` §3.2's hand-analyzed listing** —
same instruction sequence, same register roles (phase/inc/gain/gain_step/
in-ptr/out-ptr/table-ptr each resident for the whole loop), same
pre-increment addressing GCC chose on its own. The only difference is the
shift amount (`#20` vs. the original draft's `#22`), which is exactly the
1024-vs-4096-table difference `module_fm.md` §3.5/§5.1 already settled — not a
discrepancy, a confirmation. `op_render_first()` compiles to 12 instructions
(one less: no pre-read of the output bus before storing, exactly the
saving module_fm.md §5.2 attributes to that variant). `op_render_fb()` compiles to
14–15 instructions per iteration (the self-feedback average and history
shuffle add real cost, smaller than the original draft's 1024-table-era
18-instruction estimate but the same direction). None of this is a bench
result — it's confirmation that the *compiled* kernel matches the
hand-analyzed one closely enough that the upcoming cycle-count bench session
is measuring what `module_fm.md` §3 actually modeled, not a compiler-introduced
detour.


### FM P0 Measurement (#43) — measured, decisions, FX-insert estimate

`module_fm.md` §11 step 1's actual bench session — the direct analogue of the
tracker's #16 and speech's #31. Measured on `breadboard_rp2350`, GPIO 22,
2026-08-08, via a 13-build sweep (`tools/fm_rig_sweep.sh`) each flashed and
read individually, since the #42 rig holds one fixed voice-count/lever
combination per build rather than self-cycling through phases like #16/#31.

#### FX insert in isolation — reused, not freshly measured

`fx/delay.h`/`fx/reverb.h` are unchanged, engine-agnostic, global post-mix
code (`module_fm.md` §2: "Unchanged — global insert on Core 1"; module_fm.md §11 step 1 itself
suggested this cross-check: "`history_subtractive.md`'s existing subtractive table has
delay/reverb deltas that can be sanity-checked against whatever comes out
here"). Their cost is a fixed per-buffer tax applied after the voice mix,
independent of which engine produced that mix — so the subtractive engine's
already-measured deltas (`history_subtractive.md`'s Performance §, post-#12 table) are valid FX-insert
numbers for FM too, without a new build:

| | Idle duty | Delta vs. no-FX | Cycles/frame (×3401) |
|---|---|---|---|
| No FX | 0.6% | — | — |
| Delay FX | 2.1% | +1.5% | **51.0 c/f** |
| Reverb FX | 8.5% | +7.9% | **268.7 c/f** |

This **replaces `module_fm.md` §9's ~150 c/f (4.4%) reservation** for reverb with a
measured 268.7 c/f (7.9%) — about 1.8× the reservation. Re-running module_fm.md §3.4's
headline arithmetic with the corrected FX cost (available budget =
85% × 3401 − 15 idle − 268.7 FX = 2607.15, vs. the original 2726):

| Derate scenario | Cycles/voice | Voices (module_fm.md §9 reservation, 150 c/f) | Voices (measured FX, 268.7 c/f) |
|---|---|---|---|
| Static, as compiled | 126 | 21 | 20 |
| 25% derated | 158 | 17 | 16 |
| 50% derated (worst case) | 189 | 14 | 13 |

The correction costs about one voice at every tier. **It does not change the
plan-against-16 decision** at the 25%-derate tier module_fm.md §3.4 flags as the
expected case (2607/158 = 16.5, still rounds down to 16) — and as the
operator bake-off below found, the real per-voice number (measured ~100.5
c/f kernel-only, projected ~120 c/f with P2's still-unmeasured EG/LFO added)
lands nowhere near the 200 c/f/voice tier where the thinner headroom would
have mattered.

Caveat: this is a reused number from a different engine's build, not a fresh
reading from an FM binary with delay/reverb linked (the #42 rig's
`T00T_FM_PROFILE` branch deliberately excludes `fx/`, per this document's
"FM P0 Rig (#42)" section above, to keep `.bss` small for the operator sweep). The code path is
identical either way, so a divergence would be surprising, but this was not
re-confirmed on an FM-specific build in this pass — a follow-up reading
(`make ENGINE=fm`, normal non-profile build, delay/reverb toggled) would
upgrade this from "reused" to "confirmed," at effectively no cost since
#41's skeleton already links both effects. Not done here since the 16-voice
decision doesn't depend on the last few percent of precision in this number.

#### Voice-count sweep

`make ENGINE=fm FM_PROFILE=1 FM_RIG_VOICES=<n>`, defaults otherwise (BLOCK=16,
TABLE_BITS=12, INTERLEAVE=0, NOT_IN_FLASH=0, SMULWB=0, FB=1):

| Voices | Duty | Cycles/frame | Per-voice (c/f) |
|---|---|---|---|
| 1 | 2.97% | 101.0 | 101.0 |
| 2 | 6.2% | 210.9 | 105.4 |
| 4 | 12.1% | 411.5 | 102.9 |
| 8 | 23.8% | 809.4 | 101.2 |
| 16 | 47.3% | 1608.7 | 100.5 |
| 24 | 70.8% | 2407.9 | 100.3 |

Linear regression across all six points: **slope 100.05 c/f/voice, intercept
7.79 c/f** (fixed per-buffer overhead — buffer clear, DMA/FIFO handoff,
`__ssat` output write; no MIDI/IPC cost, since the rig bypasses
`ParamExchange` entirely). Flat to within measurement noise from 2 to 24
voices — no falloff or superlinear growth, same shape #16 and #31 found for
their own sweeps.

**This is markedly *below* `module_fm.md` §3.3's ~126 c/f static estimate — the
opposite direction from the tracker/speech historical pattern** (measured
usually runs 25–50% *above* static, which is exactly why P0 exists). Fully
explained, not just noted: module_fm.md §3.3's 126 c/f bundles two things this P0 rig
deliberately excludes. (1) It assumed "5 plain operators + 1 self-feedback,"
but the actual topology (§"FM P0 Rig (#42)" above) is 4 first-writer
operators + 1 modulated `op_render` + 1 `op_render_fb` — `op_render_first`
is cheaper (12 compiled instructions vs. 13, #42's assembly extraction), so
4 of the 6 operators cost less than module_fm.md §3.3 assumed. (2) module_fm.md §3.3's ~19 c/f/voice
"per-block overhead amortised (6× EG step + exp2, LFO, pitch EG, bus setup)"
has nothing to measure here — P0's rig has no EG, no LFO, no pitch EG by
design (`module_fm.md` §1's P0 scope). Recovering per-operator costs by fitting the
`FM_RIG_FB=0` vs. `FM_RIG_FB=1` delta below to the compiled instruction-count
ratios (12:13:14.5 for first-writer:plain:self-feedback) gives **first ≈
15.6 c/f, plain render ≈ 16.9 c/f, self-feedback ≈ 21.3 c/f** — matching
module_fm.md §3.2's original hand-analyzed 16–18 / 21–23 c/f predictions closely. module_fm.md §3.2's
*per-operator* numbers were right; module_fm.md §3.3's *per-voice total* was only off
because of the topology miscount and the not-yet-existing EG/LFO line item,
both now accounted for.

#### Lever bake-off (all at `FM_RIG_VOICES=16`, one lever changed per row vs. the table above's 100.5 c/f/voice baseline)

| Lever | Duty | c/f/voice | Δ vs. baseline |
|---|---|---|---|
| Baseline (all defaults) | 47.3% | 100.5 | — |
| Interleaved pair (`FM_RIG_INTERLEAVE=1`) | 46.57% | 99.0 | **−1.5 c/f (−1.5%)** |
| SRAM-resident kernel (`FM_RIG_NOT_IN_FLASH=1`, original rig) | 47.3% | 100.5 | **0 — lever didn't engage, see below** |
| BLOCK=8 | 44.97% | 95.6 | **−4.9 c/f (−4.9%)** |
| BLOCK=32 | 52.41% | 111.4 | **+10.9 c/f (+10.8%)** |
| 1024-entry table (`FM_RIG_TABLE_BITS=10`) | 47.3% | 100.5 | **0** |
| `smulwb` fusion | 45.9% | 97.6 | **−3.0 c/f (−3.0%)** |
| Plain vs. self-feedback (`FM_RIG_FB=0`) | 45.2% | 96.1 | **−4.5 c/f** (isolates the fb premium: 21.3 − 16.9 ≈ 4.4, matches) |

Two results need explaining, not just recording:

**`FM_RIG_NOT_IN_FLASH` measured zero effect — because the lever didn't
actually engage.** Checked directly: `arm-none-eabi-objdump -h` on both
builds showed an identical `.text` section (size and load address) whether
`FM_RIG_NOT_IN_FLASH` was 0 or 1. Cause: `op_render()` etc. are `inline`
functions that fully inline into `audio_engine_run()` at `-O3` (the same
inlining #42's assembly-extraction section relied on). `__not_in_flash_func`
places a function's *out-of-line* code in a linker section; once GCC inlines
the body away, there is no separate symbol left for the attribute to apply
to, so it does nothing — confirming the code really was always executing
from flash (`.text` sits at `0x10000000`, RP2350's XIP range; SRAM starts at
`0x20000000`).

**Fixed and re-measured, tests 14/15** (`tools/fm_rig_sram_retest.sh`,
`breadboard_rp2350`, 2026-08-08). `rig.h` now uses the pico-sdk's own
`__no_inline_not_in_flash_func` for `FM_RIG_NOT_IN_FLASH=1` — the SDK
documents this exact inlining trap and ships the fix (adds `noinline` so
there's a real symbol for the section-placement attribute to act on).
Device-verified the placement genuinely changed this time: `nm` shows
`op_render`/`op_render_first`/`op_render_fb` at `0x2000....` (SRAM) for
`=1`, vs. `0x1000....` (flash) for the new `=2` control (noinline, still
flash — isolates the SRAM-vs-flash effect from the call/return overhead
`noinline` itself adds, which the fully-inlined default never paid):

| Build | Duty | c/f/voice | Δ vs. inlined-flash baseline (100.5) |
|---|---|---|---|
| 14: noinline, flash (control) | 51.83% | 110.2 | **+9.7 c/f (noinline cost alone)** |
| 15: noinline, SRAM | 54.13% | 115.1 | **+14.6 c/f (noinline + SRAM)** |

**Isolated SRAM-vs-flash effect (15 − 14): +4.9 c/f/voice — SRAM is *more*
expensive, not less.** Backwards from the naive "SRAM is faster than flash"
assumption `module_fm.md` §3.6 item 2 built the "non-negotiable" framing on.
Explained, confirmed by evidence rather than asserted: `nm` on the `=1`
build shows three linker-generated veneer stubs
(`___Z9op_renderR7FmRigOpm_veneer` and two siblings) that the `=2` build
does not have at all. Flash sits at `0x10000000` and SRAM at `0x20000000` —
a ~256 MB gap, outside a Thumb `BL`'s encodable range, so every call from
the still-flash-resident render loop into the SRAM-placed kernel must
detour through an indirection the same-region flash call never pays. RP2350
also XIP-caches flash reads, and this kernel is small and reused every
sub-block — exactly the case where the cache erases most of flash's
latency disadvantage, leaving the veneer indirection as a net cost with
nothing to offset it.

**Decision: keep the kernel inlined in flash.** Not just "SRAM measured
worse than the noinline-flash control" — inlined-flash (today's actual
default, `FM_RIG_NOT_IN_FLASH=0`) beats *both* noinline variants by a wide
margin (100.5 vs. 110.2/115.1), since inlining also removes the call/return
overhead entirely. There is no configuration in this data where moving the
kernel out of flash helps; `module_fm.md` §3.6 item 2 is closed against the
opposite of its original assumption.

**Caveat, `module_fm.md` open question 10.** All of the above was measured with
Core0 doing essentially no flash-side work — MIDI/LCD/control are still
stubs. RP2350's 16 KB XIP cache is one shared resource for both cores
(`hardware_xip_cache.h`), so this margin isn't guaranteed once Core0 has
real LCD/MIDI/control traffic that can evict the FM kernel's cache lines
right when Core0 is busiest — exactly the timing where an audio glitch
would be most noticeable, and exactly what this measurement, run with a
quiet Core0, could not catch. Not retested here since there's no real Core0
workload yet to contend against. Mitigation on hand if it turns out to
matter: `xip_cache_pin_range()` (RP2350-only) permanently reserves the
kernel's flash range against eviction by anything else. If that doesn't pan
out, SRAM's "measured worse" verdict above was itself measured in
isolation — SRAM sidesteps this specific shared-cache problem entirely (its
own contention risk is per-bank and controllable), so it remains a fallback,
not a closed door.

**BLOCK direction is inverted from `module_fm.md` §3.6 item 3's framing, and the
cause is unrolling, not per-block amortisation.** module_fm.md §3.6 assumed larger BLOCK
saves cost by amortising per-block overhead — but that overhead (EG step,
exp2, LFO) doesn't exist in this rig (same reason as the voice-sweep
discrepancy above), so there was nothing for a larger BLOCK to amortise.
What actually happened: `arm-none-eabi-size` on the compiled
`audio_engine.cpp.o` shows `audio_engine_run()` at 3,172 bytes (BLOCK=8),
**5,568 bytes (BLOCK=16, the largest of the three)**, and 1,336 bytes
(BLOCK=32) — and disassembly confirms why: at BLOCK=32 the per-operator
sample loop compiles to a real branching loop (`bne.n`, 13-instruction body,
exact match to module_fm.md §3.2's hand-analyzed listing), while BLOCK=8 and BLOCK=16 get
substantially unrolled by GCC (no backward branch in an isolated,
`noinline`-wrapped probe of the same loop), eliminating the per-sample
loop-control cost that BLOCK=32 keeps paying. This is a compiler
unrolling-threshold artifact specific to this EG/LFO-free rig, not a
property of BLOCK size itself — once P2 adds the real per-block control-rate
work, that will reintroduce genuine amortisation and could shift the balance
back. Recorded as the "operator-cost side of the trade" `module_fm.md` asked P0 to
settle; final confirmation is still P2's, as already planned.

The interleaved-pair result (−1.5%) is real but far smaller than module_fm.md §3.6 item
1's "likely the single largest win" expectation. Plausible explanation, not
confirmed by disassembly: op0/op1 are called as two sequential
`op_render_first()` invocations even at `FM_RIG_INTERLEAVE=0`, and once both
fully inline into `audio_engine_run()`, GCC's own instruction scheduler has
the same independent-load-use-stall visibility across that boundary that the
hand-written interleaved kernel provides deliberately — so much of the
win may already be captured by the compiler before the lever is even
applied.

Table size (0) and `smulwb` (−3.0%) landed exactly as predicted: module_fm.md §3.5's
"identical instruction count" claim for 4096 vs. 1024 holds, and the DSP
fusion buys a small, real win with no correctness cost (already host- and
device-verified in #42).

#### Decisions

- **`MAX_VOICES = 16`, confirmed** (was `module_fm.md` §3.4's provisional plan
  value since #41). Projected real per-voice cost = measured kernel (100.5
  c/f) + `module_fm.md` §3.3's still-unmeasured ~19 c/f EG/LFO reservation (P2
  scope, out of reach for this EG/LFO-free rig) ≈ **119.5 c/f/voice**.
  Against the FX-corrected budget (2607 c/f, below), 16 voices costs 1,608
  c/f — a comfortable ~27% margin even before any credit for the P0 kernel
  number beating its own static estimate. The projection also clears module_fm.md §3.4's
  ≤130 c/f "20+ voices" threshold, but that number leans on an unverified P2
  estimate rather than a bench reading, so raising `MAX_VOICES` past 16 is
  deferred to a P2 bench pass once `EnvDX`/LFO exist to measure for real,
  per module_fm.md §3.4's own "surplus is spent on polyphony, not features" guidance —
  not decided here on a projection.
- **BLOCK = 16, provisional** (unchanged from `module_fm.md`'s assumption). The
  kernel-only measurement doesn't clearly favor a change: BLOCK=8 is 4.9%
  cheaper but BLOCK=32 is 10.8% more expensive, and both effects are
  compiler-unrolling artifacts of this EG-free rig rather than the
  per-block amortisation module_fm.md §5.3's actual BLOCK/EG-resolution tradeoff is
  about. Final call stays P2's, against real rate-99 attacks, as `module_fm.md`
  already planned.
- **Kernel form: plain, not interleaved.** −1.5% doesn't justify the
  two-operand interleaved kernel's added complexity (only valid for
  mutually-independent operand pairs, more code paths in the eventual
  routing compiler). `op_render_pair()` stays in `rig.h`, unused by the
  decision.
- **Self-feedback stays "always on," no cheap fallback needed.** Measured
  premium (~4.4–4.5 c/f, isolated two ways: the FB=0/FB=1 rig delta and the
  fitted per-operator decomposition) matches `module_fm.md` §3.3's assumed 22-vs-17
  budget closely (fitted: 21.3 vs. 16.9). No surprise here to explain.
- **Keep the kernel inlined in flash — do not move it to SRAM** (tests 14/15,
  above). Isolated SRAM-vs-flash effect is +4.9 c/f/voice *worse*, not
  better (linker veneers on every call crossing the flash→SRAM gap, with no
  offsetting win since RP2350's XIP cache already erases most of flash's
  latency disadvantage for a small, reused-every-block loop like this one).
  Inlined-flash beats both noinline variants outright regardless of
  placement. Closes `module_fm.md` §3.6 item 2 against the opposite of its
  original "non-negotiable" assumption.
- **Freeverb stays.** Real cost is 268.7 c/f (7.9%), not the ~150 c/f (4.4%)
  reservation, but the 16-voice budget still clears with margin (above) —
  see the FX-insert section below for the full number.

`module_fm.md`'s provenance caveat, module_fm.md §3.4, and open questions 1–2 are updated to
match — see `module_fm.md` directly.

The `FM_RIG_FB` lever, the CMake/Makefile plumbing for it, and the isolated-
plain-vs-feedback topology fork in `fm_rig_render_voice_block()` did not
exist before #43 — #42's fixed topology could exercise `op_render_fb()`
correctly but had no way to A/B it against a plain operator in the same
chain position. Host-verified (`render_fm_rig`, direct compile-flag sweep):
`FM_RIG_FB=0`, `FM_RIG_FB=0 FM_RIG_INTERLEAVE=1`, and the `FM_RIG_FB=1`
default all pass the existing bounded/non-silent checks. Device-verified:
`make ENGINE=fm FM_PROFILE=1 FM_RIG_FB=0` builds clean, and a combined-
levers build with `FM_RIG_FB=0` added to #42's existing combination
(`FM_RIG_INTERLEAVE=1 FM_RIG_TABLE_BITS=10 FM_RIG_BLOCK=8 FM_RIG_VOICES=32
FM_RIG_NOT_IN_FLASH=1 FM_RIG_SMULWB=1`) builds clean at 29,144/18,296/47,440
(text/bss/dec) — smaller than #42's all-levers-combined 29,104/18,296/47,400
by the expected margin (`op_render()` is shorter than `op_render_fb()`, and
that's the only thing this lever changes). The default `FM_PROFILE=1` build
(all levers unset, `FM_RIG_FB` implicitly 1) reproduces #42's exact
32,080/23,064/55,144 byte-for-byte, and the plain `make ENGINE=fm` skeleton
(no `FM_PROFILE`) reproduces #41's exact 27,784/202,768/230,552 — confirming
this slice changed nothing observable about either existing build.


### FM Engine — EnvDX + BLOCK Confirmation (#45)

`src/engines/fm/env_dx.h` (new): the DX7 envelope (`module_fm.md` §5.3, P2's gate).
Deliberately not `envelope.h`'s ADSR (`module_fm.md`: "Do not reuse `envelope.*`")
-- 4 x (rate, level) stages per operator, direction-agnostic (any stage can
ramp up or down to any target), stepped once per control block, log domain
throughout with a single exp2-table conversion to linear at each block
boundary. `op_render`/`op_render_first`/`op_render_fb` (op.h) are
byte-for-byte unchanged from #44 -- the EG only decides what `gain`/
`gain_step` are at each block boundary; the per-sample kernel still just
does `gain += gain_step`, confirmed against the emitted assembly below.

#### Fixed-point design

Everything lives in a single "log2 offset from an operator's reference
level" domain (Q iiii.8, `EG_LOG2_FRAC_BITS = 8`, matching the 256-entry
exp2 table 1:1, no interpolation needed). Three independent 0-99 DX7
parameters resolve into this domain and simply add:

- **Operator output level (TL)**, resolved once at note-on.
- **Each EG stage's target level**, looked up fresh every stage transition.
- **Velocity sensitivity** (0-7), resolved once at note-on: 0 at
  sensitivity 0 (regardless of velocity) or at max velocity (regardless of
  sensitivity); increasingly negative for softer hits on a more sensitive
  operator (~24 dB / 4 octaves of range at sensitivity 7, velocity 0).

Level 0 (both TL and an EG stage target) maps to a deliberately deep floor
(-40 octaves) rather than merely "very quiet" -- deep enough that
`eg_to_linear()` underflows to an *exact* int32 zero for any valid
reference (references are always < 2^31; 2^31 x 2^-40 < 1). This is what
makes "a voice reports itself free only when its carriers have actually
decayed" a real guarantee: `EnvDX` has a genuine terminal `EG_IDLE` state,
reached only by completing the release stage, not an epsilon-on-a-
decaying-value guess -- the tracker's #21 bug ("key-off never frees a
voice") was exactly a missing version of this guarantee, in a different
engine.

The level curve (both TL and EG stage levels) and the rate curve (0-99 ->
octaves/second, independent of BLOCK size) are honest approximations of
the real DX7's general shape -- linear-in-dB level curve (~96 dB across
1-99), exponential rate curve (~20s slowest full sweep, ~6ms fastest) --
not a byte-exact reproduction of Yamaha's hardware tables. Exact
replication is P3/P6 territory, once real `.syx` patches exist to compare
directly against Dexed.

#### Host-verified (`tools/host_render/render_fm`, extended)

- **Level table**: level 99 = the reference exactly; level 0 = an exact
  digital 0; monotonic across 0-99; level 50's linear gain is far below
  `reference * 50/99` (a real nonlinear/log curve, not "a linear
  approximation" -- an explicit acceptance criterion).
- **Velocity sensitivity**: sensitivity 0 is a true no-op at any velocity;
  sensitivity 7 is real and monotonic (softer hits quieter, more so at
  higher sensitivity).
- **Six independent EGs**: stepping `FM_TEST_PATCH`'s real (non-flat) EGs
  block-by-block and reading each operator's `gain` directly (not the mixed
  audio, which can't cleanly attribute a level to one operator) --
  op4 (the modulator driving the carrier, EG level `{99,20,15,0}`) decays
  to 3.2% of its own 5ms level by 800ms; op5 (the carrier, `{99,70,60,0}`)
  *grows* to 4.9x its own 5ms level over the same window (still mid-attack
  at 5ms) -- and all six operators land at six distinct gains at 800ms.
  This is `module_fm.md`'s DX-EP shape (bright pluck settling into a mellower
  sustain) confirmed in the actual per-operator numbers, not just eyeballed
  from a mixed WAV.
- **Release**: a held note stays active through release (no #44-style hard
  cutoff), goes idle (`EG_IDLE`) 783 ms after note-off at `FM_TEST_PATCH`'s
  rates, and the carrier's `gain`/`gain_step` are both an exact 0 at that
  point -- not merely below some threshold.
- **Routing/spectrum** (#44's checks, re-verified against a "flat EG"
  variant of `FM_TEST_PATCH` -- same routing/ratios/levels, EG jumps to and
  holds at full immediately -- so the routing claim is measured in
  isolation from #45's deliberately fast-decaying real EG shape): unchanged
  from #44's PASS.

#### Kernel identity, confirmed against the emitted assembly

`make ENGINE=fm` (real, non-profiling build) compiles clean;
`arm-none-eabi-objdump` on `audio_engine.cpp.o` shows the same 48 `smlawb`
instances as #44's build, and the per-sample body is unchanged in shape:

```
adds  r4, r6, r2      @ phase += inc (register-carried across the unrolled block)
add   r0, r4          @ + modulation bus value
lsrs  r0, r0, #20      @ table index -- unchanged
ldrsh.w r6, [r5, r0, lsl #1]  @ table lookup -- unchanged
add   lr, r7           @ gain += gain_step -- the ONE add #45 adds nothing to
smlawb r6, ip, r6, sl   @ x gain -- unchanged
add.w r0, r0, r6, asr #6  @ accumulate/store -- unchanged
str   r0, [r3, #0]
```

Exactly one extra `add` per sample versus #44 (`add lr, r7` / `add ip, r7`,
feeding the running gain register into the next `smlawb`) -- `module_fm.md` §5.3's
"per-sample cost in the kernel is one add" claim, confirmed against real
compiled output, not assumed from the source.

#### BLOCK Confirmation (module_fm.md open question 3, closed)

`op.h`'s `FM_BLOCK` is now a compile-time override (`T00T_FM_BLOCK`, wired
through `CMakeLists.txt`/`Makefile` as `make ENGINE=fm FM_BLOCK=8|32`, same
convention as `DMA_BUFFER_SIZE`) -- distinct from `FM_RIG_BLOCK`, which only
affects the #42 profiling rig. Compared via a standalone host probe
stepping a single carrier operator (R1=99, the fastest DX7 attack) through
`env_dx_step_block()` at each candidate BLOCK size, recording gain at every
block boundary:

| `FM_BLOCK` | ms/block | Steps to 99% of full scale | Time to 99% | vs. BLOCK=16 |
|---|---|---|---|---|
| 8 | 0.181 | 34 | 6.17 ms | smoothest, most accurate |
| 16 | 0.363 | 17 | 6.17 ms | baseline |
| 32 | 0.726 | 9 | 6.53 ms | coarsest, 8% timing overshoot |

BLOCK=32 loses on both axes now measured -- #43's kernel-only bench already
found it 10.8% more expensive, and this reading adds a real timing/
granularity cost on top (fewest distinct gain steps across the fastest
attack, and the only one of the three whose "reached target" check lands
measurably late). Ruled out.

BLOCK=8 edges out BLOCK=16 on both the #43 kernel cost (4.9% cheaper) and
this reading (smoothest ramp, no timing overshoot) -- but #43's rig has no
EG, and BLOCK=8 means twice as many `env_dx_step_block()` calls per second
as BLOCK=16 (a 64-bit divide plus a handful of table lookups per operator,
6 operators per voice). A back-of-envelope projection (`module_fm.md` §3.3's own
~19 c/f/voice BLOCK=16 EG-overhead estimate, doubled for BLOCK=8 ≈ 37.5
c/f/voice, ×16 voices ≈ +300 c/f) plausibly outweighs BLOCK=8's ~78 c/f
total kernel saving (16 x 4.9% of 100.5 c/f) -- but this is a projection,
not a bench reading; #43's own "surplus isn't spent on a projection"
discipline applies here too.

**Decision: BLOCK=16 confirmed, not changed.** Neither the smaller nor
larger candidate is a clear-cut win once EG overhead exists to weigh
against kernel cost, and BLOCK=32's regression on both axes is unambiguous
enough to rule out outright. Revisit BLOCK=8 with a real profiling-pin
reading (not this projection) if a future issue wants to chase it.


### syx2patch Host Converter (#47) — bug narrative and validation

#### Two bugs caught by actually rendering a real bank, not by design review

1. **Multi-carrier int16 overflow.** `FmOpParams::level` (the reference-gain
   ceiling — not DX7 data, see `patch.h`'s own comment) was set flat per role
   using `FM_TEST_PATCH`'s own precedent (`FM_CARRIER_LEVEL_REF = 1<<21`,
   `FM_MODULATOR_LEVEL_REF = 1400000000`). `FM_TEST_PATCH` only ever has one
   carrier, so this was never exercised; real DX7 algorithms 19-32 sum 3-6
   carriers into one voice, and each at the same flat reference clipped
   badly (measured on ROM1A: BRASS 1's peak hit 43185, E.ORGAN 1's 46675,
   both well past int16's 32767). Fixed by scaling each carrier's reference
   by `1/carrier_count` — N summed carriers at output_level=99 land back at
   the same envelope one carrier alone would, exactly mirroring how
   `output_level` already carries the real per-patch balance.
2. **Carrier release level never reaching idle.** A carrier operator with a
   nonzero EG L4 (release-stage target) never reaches `env_dx.h`'s
   `EG_IDLE` — the exact shape of the tracker's #21 "key-off never frees a
   voice" bug, just reached through DX7 patch data instead of engine logic.
   `convert_voice()` forces carrier L4 to 0 with a logged warning; the
   audible cost is that a handful of sustained-pad-style patches release
   fully instead of holding a residual level, since this engine has no
   sustain-pedal semantics to make that distinction meaningful anyway.

A third — the modulator-side equivalent of bug 1, only possible once #57
raised modulator headroom ~64× — surfaced immediately after this section was
written; see "FM Kernel — Modulation-Depth Ceiling Fix (#57)" below.


#### Validation

`python3 tools/test_syx2patch.py` — unit checks (all 32 algorithms decode
and only 4/6 flag interleaved, algorithm 1/32 topology by hand, coarse/fine
ratio formula, fixed-frequency skip, carrier L4 forcing, feedback-level-0
exactness, multi-carrier level scaling, bit-packing round-trip on a
synthetic voice, checksum/header rejection paths) always run; corpus-
dependent checks run against whatever `.syx` files are in `../syx/`
(gitignored, same as `xm/`) and skip cleanly if empty. Verified against
ROM1A and ROM1B (2026-08-08): 28/32 and 31/32 voices converted respectively
at #47's v1 (skips: fixed-frequency-mode patches — ROM1A's "TUB BELLS"/
"STEEL DRUM"/"REFS WHISL"/"TRAIN", ROM1B's "PIPES 2"); ROM1B exercises the
algorithm 4/6 fallback for real ("CLAV 2"/"CLAV 3"/"PIPES 4"). **Updated,
#48: all previously-skipped patches now convert too** (fixed-frequency
mode is wired up) — see "FM P3v2" below. `render_fm`'s patch-bank render
(host-only, `T00T_FM_HAS_PATCHES`-gated) renders a 3-second A3 note per
converted patch to `fm_patches/*.wav` and checks every one is bounded and
non-silent. `make ENGINE=fm` builds clean both with and without
`patches.h` present.


### FM P3v2 — Key/Rate Scaling, Detune, Fixed Frequency (#48)

Picked up immediately after #59's rate-curve fix, per the author's own call on
whether to keep chasing Dexed-comparison fidelity work or move on to
completing the remaining signal-path features first and compare again
after: "I think it is best to continue implementing the remaining signal
path functionality and do comparisons after." All four ported directly
from Dexed's real code (`dx7note.cc`'s `ScaleRate`/`ScaleLevel`/
`ScaleCurve`, `osc_freq()`'s fixed-frequency branch), continuing the same
method #57/#58/#59 established.

#### Key level scaling and rate scaling — new runtime plumbing

Both need the actual played MIDI note, which `VoiceParams` never carried
before (`phase_inc` is already bend-scaled into a frequency, not invertible
back to a clean note number) — added `VoiceParams::note` (raw MIDI 0-127),
set in `midi_controller.cpp`'s `midi_voice_on()`, threaded through
`audio_engine.cpp` into a new `midinote` parameter on `fm_voice_note_on()`.

`FmOpParams` gained six new fields: `scale_breakpoint`/`scale_left_depth`/
`scale_right_depth`/`scale_left_curve`/`scale_right_curve` (key level
scaling) and `rate_scaling` (RS, 0-7). `FmOp` gained `rate_scale_qrate`,
resolved once at note-on (`dx7_scale_rate()`, env_dx.h) and added directly
to each stage's own qrate at every transition (`env_dx_step_block()`,
extended with an optional `rate_qrate_delta` parameter, default 0) —
matching Dexed's own `qrate += rate_scaling_` order exactly, not a post-hoc
scale on the already-converted octaves/second value. Zero-delta takes the
exact same table-lookup fast path #59 already verified, so this is
behavior-neutral for every patch that doesn't use rate scaling (including
`FM_TEST_PATCH`, whose six new fields all default-zero).

Key level scaling (`dx7_scale_level()`/`dx7_scale_curve()`, direct ports of
Dexed's `ScaleLevel`/`ScaleCurve`) resolves into `static_log2` at note-on —
but combined with TL *before* the log2 conversion and clamped to [0,127],
exactly like Dexed's own `outlevel = min(127, outlevel)`, not added as a
separate unclamped log2 offset. This mattered: a boosting curve (DX7 curve
2/3, "+EXP"/"+LIN") at high depth on an extreme note can produce a raw
combined value equivalent to *tens* of octaves of boost, and
`eg_to_linear()`'s shift computation has no defined behavior for a large
positive log2 offset (`shift = 15 - oct` goes negative, an unsigned
right-shift by a negative amount) — caught by reasoning through the
arithmetic before it ever ran, not by a crash.

#### Detune and fixed-frequency mode — already-existing runtime, newly wired

Both `FmOpParams::detune_cents` and `fixed_hz`/`fixed_freq` have existed
since #44 and were already fully handled by `fm_op_inc()` — #47 just never
populated them, and skipped every fixed-frequency patch outright rather
than approximate it. `tools/syx2patch.py` now populates both:

- **Detune**: `op_detune_cents()` approximates DX7's real (note-frequency-
  dependent) detune formula as a fixed ±7 cents at the 0-14 (offset -7)
  extremes — replicating the real formula needs the full pitch pipeline's
  absolute log-frequency value, which this note-independent, note-on-
  baking converter doesn't have. Small enough that "audible as beating
  between two operators at the same ratio" holds regardless of curve shape.
- **Fixed-frequency mode**: `op_fixed_hz()` reduces Dexed's `osc_freq()`
  fixed-mode branch to a closed form: `Hz = 10^(((coarse&3)*100+fine)/100)`
  — Dexed's own magic constant `4458616` is exactly `(2^24 * log2(10) *
  0.01) << 3`, so this needs no `Freqlut`/Q24-log-frequency machinery at
  all, just a direct power. All 32/32 of ROM1A now convert (was 28/32) —
  the 4 previously-skipped patches ("TUB BELLS"/"STEEL DRUM"/"REFS
  WHISL"/"TRAIN") all use a fixed-frequency operator for an inharmonic
  bell/percussion partial, exactly the real DX7 use case this unblocks.
  ROM1B's "PIPES 2" (previously skipped for the same reason) now converts
  too.

#### Verification

A dedicated host check, `render_fm`'s `run_key_rate_scaling_check()` —
nothing else in the suite would exercise `dx7_scale_level`/`dx7_scale_rate`
at all, since `FM_TEST_PATCH`'s own scaling fields are all zero. Builds a
patch with deliberately strong scaling (breakpoint 60, +LIN right curve at
depth 99, RS=7) and checks three things at two notes (30 and 96): the
resolved `static_log2` is higher for the above-breakpoint note, the
resolved `rate_scale_qrate` is a larger positive delta for the higher note,
and — the one that actually matters — stepping both through 20 blocks past
their stage-2 transition, the higher note's gain moves measurably further
(60096 vs. 7116 raw units in the host run, ~8.4x) — a real per-sample
effect, not just a resolved-but-unused number. `test_syx2patch.py` gained
four new tests (`op_fixed_hz()`/`op_detune_cents()` formula checks, a
fixed-frequency conversion check replacing the old skip-check, a scaling-
fields pass-through check) — 19/19 passing. Kernel disassembly unaffected
(still 48 `smlawb` instances) — everything here is note-on/block-rate work,
never inside the per-sample loop. `make ENGINE=fm` builds clean with and
without `patches.h`.


### FM Kernel — Modulation-Depth Ceiling Fix (#57)

Filed immediately after #47's hardware listen: real ROM1A patches sounded
like near-plain sine tones on real `breadboard_rp2350` hardware, differing
only in envelope/octave, never in timbre — "whistling," the author's word. Traced
with a direct Goertzel measurement on the converted "BRASS 1" patch (not
guessed): its two FM-modulated operators produced real but tiny sidebands
(~0.2–1% of their own fundamental), while the algorithm's two *unmodulated*
carriers dominated the mix outright. Likely also the deeper explanation for
#45's "marimba, very weak sustain" report — that fix (a quadratic instead of
linear output-level curve) was independently correct, but a marimba is
essentially a struck near-sine with fast harmonic decay, exactly the
signature this ceiling produces regardless of envelope shape.

**Root cause:** `op.h`'s kernel computed `out[i] = fm_mul_gain(sample, gain)
>> FM_OUT_SHIFT` (`FM_OUT_SHIFT = 6`, on top of `fm_mul_gain`'s own implicit
`>>16`) identically for every operator, whether its `out[]` became final
int16-range audio (a carrier) or the *next* operator's raw phase-modulation
input, added directly into a full-circle (`2^32` = 2π) `uint32_t` phase
accumulator (a modulator). One shift serving both meant modulator deviation
topped out around ~0.03–0.05 rad even at `gain = INT32_MAX` — already noted
as a real, unresolved limitation in `patch.h`'s `FM_TEST_PATCH` comment
since #44, never revisited until real DX7 data made it impossible to ignore.

**Fix:** split the shift by role — `FM_OUT_SHIFT_CARRIER = 6` (unchanged;
carrier headroom/loudness doesn't move at all) and
`FM_OUT_SHIFT_MODULATOR = 0` (only `fm_mul_gain`'s built-in `>>16` applies),
raising modulator headroom ~64× to ~1.57 rad at `gain = INT32_MAX`. Passed
as a per-call `out_shift` parameter to `op_render`/`op_render_first`/
`op_render_fb` — chosen once by `fm_voice_render_block()` from the routing
(`r.out_bus[i] == FM_TARGET_OUT`), never inside the per-sample loop — so the
loop's instruction shape is unchanged. Disassembly-verified against the
device build: still exactly 48 `smlawb` instances, same as #44/#45's own
counts; the shift itself becomes a register operand (`asr.w rX, rY, r9`)
instead of a baked-in immediate, resolved once per block, not per sample.

**A second overflow risk, found by the same headroom increase:** several
real algorithms (7, 8, 10, 12, 13 — all present in ROM1A/B) sum 2–3
modulators into one shared bus; at the new ~64× headroom that's a genuine
int32 risk the old ceiling never made possible. Fixed the same way #47 fixed
multi-carrier overflow: both `tools/syx2patch.py` (`fan_in` dict, divides
each modulator's `level` by how many operators share its target) and
`FM_TEST_PATCH` (`patch.h`: op1/op2/op3 all feed op4, now `1e9/3` each
instead of `1e9`) divide by fan-in count. New unit test:
`tools/test_syx2patch.py`'s `test_multi_modulator_level_scaled_down`
(algorithm 12's 3-way fan-in).

**The author re-flashed and reported back: profiling roughly unchanged (good —
the register-operand shift is cost-neutral, as expected), but still "soft,"
nothing like real brass bite.** That prompted a direct question — how does
Dexed actually implement this, what are we missing — answered by fetching
Dexed's real per-sample kernel (`Source/msfa/fm_op_kernel.cc`/`sin.h`,
Apache-2.0) rather than guessing further. `FM_OUT_SHIFT_MODULATOR = 0`
already extracts the maximum magnitude a 32-bit `gain * sample` product can
yield — no smaller shift exists, that multiply structurally caps there. But
Dexed doesn't need a bigger magnitude at all: its phase representation only
needs `2^24` units per full cycle (`Sin::lookup`'s table read ignores every
bit above bit 23), not `2^32` like this engine's `phase`. The real gap was
never raw output magnitude — it's how much phase deviation a given
magnitude buys, and this engine's wider per-cycle convention (chosen for
`inc`'s own pitch precision, not modulation sensitivity) was quietly taxing
every modulator ~256× relative to Dexed's own choice.

**Second fix:** `FM_MOD_INPUT_SHIFT = 4` (op.h) pre-scales incoming
modulation (`in[i]`, and self-feedback's `fb1`/`fb2` average) left by 4 bits
*at the point it's added to `phase`*, using unsigned wraparound — the same
trick a narrower phase representation gets for free, applied only where
needed. `in[]`/`out[]` stay `int32_t` (the fan-in/carrier-count overflow
fixes above are untouched); only how aggressively a stored value bends
phase changes. `FM_TEST_PATCH`'s own modulator levels needed retuning down
at the new headroom — a real bug caught in the process: its old
near-ceiling constants (picked before this shift existed) over-drove badly
enough at op3's slightly-slower attack rate (R1=90, not 99) to underflow
`eg_to_linear()` and produce an exact-zero gain mid-attack.

**Host-measured, after both fixes:** across ROM1A's 28 converted patches,
2nd-harmonic/fundamental ratios (per-patch diagnostic in `render_fm`'s
patch-bank render, computed from each patch's own lowest-ratio carrier —
carriers can run at fractional ratios) now span ~0.0 (patches with no real
modulation at that voicing, e.g. VIBE/MARIMBA/TIMPANI — legitimate, not a
bug) to several times the fundamental (BRASS 1 ~3.9×, ORCHESTRA ~7.0×) —
real variety, not a uniform near-zero.

**A test bug this surfaced, not an audio bug:** `render_fm`'s own routing/
ratio spectrum check (`run_patch_spectrum_check`) started failing at the
new depth. `FM_TEST_PATCH`'s op0 (ratio 0.5, feeding the carrier two hops
upstream through op2→op4) barely mattered at the old, tiny ceiling; at real
depth it visibly pulls the whole chain's true fundamental down to half the
played note (110 Hz, not 220 Hz). The check's "noise floor" probe
(`note*4.5` = 990 Hz) turned out to be exactly the 9th harmonic of that real
110 Hz fundamental — confirmed by sweeping the full spectrum (every
multiple of 110 Hz populated, a genuinely off-grid probe at `note*4.3`
measuring ~0.01) rather than assumed. Fixed by moving the probe frequency,
not by detuning the patch further to satisfy a check with the wrong
reference. Worth remembering: a "broadband noise" symptom from a spectral
check is only real if the probe frequency is verified off *every* harmonic
grid a patch's own ratios can produce, not just the played note's own.

**Still needs the author:** re-flash and re-listen (again) — this second fix
changes the timbre further, and should be the one that actually delivers
real brass/bell bite; a fresh profiling read wouldn't hurt either, though
this change is a compile-time constant shift with no new per-sample cost
either way.


### FM Level & Rate Curves — Ported from Dexed (#58, #59)

Two more rounds in the same investigation #57 started, both following the
same method: when a hand-fit curve is suspect, port and *run* Dexed's real
code to get ground truth, rather than adjust the fit by ear.

#### #58 — level curve

The author's report after #57's depth fix: "more character now... but still
sounds incorrect, almost overdriven sometimes." A direct question — "how
does Dexed implement the operator math, what are we missing" — led to
porting Dexed's real `Env`/`Exp2` pipeline (Source/msfa/env.cc/exp2.cc,
Apache-2.0) into a standalone harness and running it, rather than continuing
to hand-adjust #57's depth constants. That first confirmed #57's own
modulation-index calibration was already in the right ballpark (Dexed's
real TL=99 index ≈12.57 rad / 2.0 cycles matches what `FM_MOD_INPUT_SHIFT`
produces) — the actual problem was one level upstream.

`env_dx.h`'s `DX7_LEVEL_TO_LOG2[]` (#45's hand-fit quadratic-in-dB curve,
shared by operator output level and EG stage levels) is considerably
flatter than the real DX7 curve across most of the 0-99 range:

| Level | #45 curve | Real DX7 (Dexed, measured) |
|---|---|---|
| 90 | -0.8 dB | -6.8 dB |
| 70 | -8.2 dB | -21.9 dB |
| 50 | -23.5 dB | -36.9 dB |

Most real modulators sit at TL 70-90, not 99 — so #57's newly-unlocked
depth was running hot on nearly everything, not because the ceiling was
wrong but because the curve let mid-range TL values stay too close to full
strength. Fixed by porting Dexed's real `Env::scaleoutlevel()` directly
into `env_dx_init_level_table()` (`dx7_scaleoutlevel()`, env_dx.h) —
verified structurally equivalent to this engine's existing "two independent
log2 offsets, added, one `eg_to_linear` call" design (Dexed's own TL and
EG-level combine the same way before one `Exp2::lookup`), so no
architecture change, just a correct table. One `scaleoutlevel(v)*32` unit
is exactly 1/256 octave in Dexed's own convention — exactly this file's
existing `EG_LOG2_ONE` scale, direct subtraction, no rescaling needed.

Host-measured after the fix: the 2nd-harmonic/fundamental spread across
ROM1A's 28 patches tightened into a much more realistic range (mostly
0.02-0.7, matching how real DX7 patches mostly use moderate TL), while
BRASS 1 (whose modulator genuinely uses TL=86, closer to the top) stayed
meaningfully rich (~3.9x). One diagnostic-only quirk noted, not a bug:
ORCHESTRA's ratio (141x, later 59.7x after the rate fix below) is inflated
because its "2nd harmonic" probe bin coincides with a *second, independent
carrier's own fundamental* (op2 at ratio 2.0), not a real sideband of op0 —
a known limitation of the simple lowest-ratio-carrier diagnostic for
patches with multiple harmonically-related carriers.

#### #59 — rate curve

The author's next report: "envelopes feel a bit sluggish, particularly in
program 3 and 4 (STRINGS 1/2)... a noticeable 'delay' from note on until
something audible comes out. The duty cycle shows activity immediately, but
the attack feels delayed." Same method: ported and ran Dexed's real
`Env::advance()`/`getsample()` rate derivation rather than adjust by ear.

`env_dx.h`'s `DX7_RATE_TO_STEP[]` (#45's smooth single-exponential fit,
rate 0 ~20s to rate 99 ~6ms for a full 40-octave sweep) turned out
dramatically too slow at the fast end:

| Rate | #45 curve | Real DX7 (Dexed, measured) | Ratio |
|---|---|---|---|
| 0 | 2.0 oct/s | 2.7 oct/s | 1.35x |
| 20 | 10.3 oct/s | 21.5 oct/s | 2.1x |
| 50 | 120.3 oct/s | 689.1 oct/s | 5.7x |
| 70 | 619.4 oct/s | 5512.5 oct/s | 8.9x |
| 90 | 3189.0 oct/s | 55125.0 oct/s | 17.3x |
| 99 | 6666.7 oct/s | 154350.0 oct/s | 23.2x |

Real DX7 rate isn't smooth — it's piecewise, built from a `qrate` value and
a `(4+(qrate&3)) << (2+6+(qrate>>2))` bit-shift step that accelerates far
more steeply toward the high end than any single exponential fit can.
R1=99 (an extremely common "instant attack" choice, including
`FM_TEST_PATCH` and most of ROM1A/B) was landing ~20x slower than real
hardware — exactly matching "duty cycle shows activity immediately, attack
feels delayed": the EG genuinely was running (CPU cost identical either
way), just at roughly 1/20th real speed. Fixed by porting the real
`qrate`/`inc_` formula into `env_dx_init_rate_table()`
(`dx7_rate_to_octaves_per_sec_q8()`, env_dx.h); keyboard rate scaling
(Dexed's `rate_scaling_` term) stays deferred to #48, same as key level
scaling, since this engine has no per-note rate scaling yet at all.

Host-measured: `FM_TEST_PATCH` now reaches its stage-3 sustain within
~200ms instead of ~800ms+ (directly probed: op4/op5 gain both flat,
unchanging, well before 200ms) — `render_fm`'s own EG-shape checkpoints
(5/100/800ms, calibrated to the old curve) were updated to 1/200ms to
still capture "near attack peak" vs. "settled" at the new, correct speed.
Release now reaches `EG_IDLE` in ~163ms instead of ~871ms. All host tests
pass; disassembly unaffected (48 `smlawb`, table-generation-only changes,
same as #58).

**Still needs the author:** re-flash and re-listen once more — #58 should have
tamed the overdrive, #59 should have fixed the sluggish/delayed attacks,
together getting closer to real DX7 timing and character on top of #57's
depth fix.


### FM P4 — Pitch EG + Per-Voice LFO (#49)

Picked up as the next item on module_fm.md's own roadmap after #48 (P3v2): the
last of the DX7 parameter set, both control-rate-only per module_fm.md's own zero-
per-sample-cost constraint. Same method as #57/#58/#59/#48: real Dexed
source (`Source/msfa/pitchenv.{h,cc}`, `lfo.{h,cc}`, and the relevant
`dx7note.cc`/`PluginData.cpp` glue, all Apache-2.0) fetched directly and
either ported verbatim (data tables) or run in a standalone calibration
harness to pin down real numbers before committing to this engine's own
re-expression of the combination math.

#### Pitch EG (`fm/pitch_eg.h`, new file)

One per voice (not per operator, unlike `EnvDX`), same 4-stage
(rate, level) shape and block-rate-stepping convention, but in a plain-float
cents domain instead of `EnvDX`'s log2 fixed point, and consumed
differently: instead of a `gain`/`gain_step` pair for the kernel, its
per-block result scales every non-fixed-frequency operator's `inc`
(`fm_voice_step_pitch_and_mod()`, op.h).

`DX7_PITCHENV_LEVEL`/`DX7_PITCHENV_RATE` (Dexed's `pitchenv_tab`/
`pitchenv_rate`) ported verbatim. Calibration harness run against Dexed's
real formula before committing to the cents re-expression:

| | Dexed real value | This engine's derivation |
|---|---|---|
| Level 0/50/99 | -4800 / 0 / +4762.5 cents | table's own ±128 raw span *is* the real ±4-octave range — `raw * 37.5` cents, no rescaling surprise |
| Rate 0/99 | 0.047 / 11.97 octaves/sec | `raw / 21.3` — Dexed's own `21.3` constant, N/sample-rate cancel out of `PitchEnv::init()` the same way #59 found for the amplitude EG's own rate table |

Rate 99's ~0.67s full-8-octave sweep is real DX7 character, not a bug — the
pitch EG's own "attack blip"/"brass scoop" character is a fast *relative*
move over tens/hundreds of cents, not a full-range sweep; the amplitude
EG's own rate 99 (near-instant) is a different, unrelated parameter.

The 4-stage state machine turned out to be exactly isomorphic to `EnvDX`'s,
confirmed by reasoning through Dexed's real `PitchEnv::getsample()`/
`keydown()` condition (`ix_<3 || (ix_<4 && !down_)`) rather than assumed:
stages 1-2 auto-advance on reaching target, stage 3 holds forever while a
note is held (real hardware never applies R4/L4 until release, even if
natural progression would otherwise reach that stage), release
(`fm_pitch_eg_release()`) jumps to stage 4 from wherever the EG currently
is — same convention `env_dx_release()` already uses.

One real footgun, caught and documented before it could bite: `PitchEnv::
set()` starts each note from L4 (release level), not silence — there's no
"silence" for pitch, so real hardware (and this port) resumes from wherever
the previous note's release left off, approximated by L4 itself. This means
a **zero-initialized `FmPitchEgParams`** (level all 0) is a real ~4-octave
pitch drop applied to every note, not "off" — "off" is level
`{50,50,50,50}` (DX7 hardware center, 0 cents). `FM_TEST_PATCH` (and every
copy-constructed test patch derived from it) needed this set explicitly;
`tools/syx2patch.py` never hits the pitfall since it always copies real
patch bytes straight through, never leaves a field at its implicit zero.

#### LFO (`fm/lfo.h`, new file)

One per voice. All six DX7 waveforms (triangle/saw down/saw up/square/
sine/sample & hold — same numbering as real hardware, no remapping needed
by the converter), rate, delay, key sync, PMD, AMD, PMS (voice-wide), plus
each operator's own AM sensitivity (`FmOpParams::am_sensitivity`, 0-3).

**Closes module_fm.md open question 5: per-voice, global-phase mode dropped, not
built.** module_fm.md's own text already recommended per-voice-with-key-sync as
"strictly better for polyphony," keeping a global-phase patch flag as an
option "where DX7 fidelity on specific patches matters." That flag was
built as far as the design stage and then deliberately not implemented:
#48's multitimbrality (one patch pointer per voice) means a literal single
shared LFO instance has no principled behavior once two simultaneously-
active voices request global phase with two *different* patches' rates —
whose rate does the one shared phase follow? Real DX7 hardware never faces
this question (one patch loaded at a time), so there's no "real DX7
behavior" to fall back on, only an arbitrary tie-break to invent. Per-
voice-with-key-sync already gives every note struck at the same instant
identical LFO phase — the common "block chord" case DX7 fidelity actually
cares about. The residual gap (notes of the *same* patch struck at
different times drift slightly out of phase with each other over a long
sustained chord) is small, patch-dependent, and not worth the architectural
ambiguity above. Revisit only if #53's real Dexed-diff work finds this
audible on a reference bank — not before.

Waveform generation is reasoned through against Dexed's real per-waveform
bit tricks (`Lfo::getsample()`) and cross-checked shape-by-shape (triangle
peaks at the cycle's center per Dexed's own complement-past-halfway trick,
not at the edges as a naive guess might assume), but re-expressed as a
plain float function of this engine's own Q32 phase convention rather than
replicating Dexed's `phase_>>7`/`phase_>>8` bit-for-bit — those shifts are
tuned to Dexed's own internal table/Q-format, not a property of the DX7's
real waveform shapes, and control-rate cost (~2756 Hz at BLOCK=16) makes
float math effectively free either way.

Rate table (`DX7_LFO_RATE_SOURCE`, Dexed's `lfoSource`) *is* ported
verbatim. Real Hz derivation (`dx7_lfo_rate_to_hz()`) cancels Dexed's own
per-block/sample-rate normalization the same way the pitch EG's rate table
did — cross-checked against commonly cited real DX7 figures: rate 0 ≈
0.065 Hz (~15.5s period), rate 99 ≈ 50.9 Hz, both matching.

Delay ramp (`dx7_lfo_delay_seconds()`) reduces Dexed's real two-stage Q32
accumulator (`Lfo::reset()`/`getdelay()`) to a single dominant-stage real-
seconds formula — a documented simplification, not a fidelity claim (the
two-stage curve exists to serve Dexed's own fixed-point arithmetic, not
because the perceptual "not yet audible, then fading in" curve genuinely
needs two pieces). Cross-checked anchors: delay 0 → instant, delay 99 →
~2.66s, delay 50 → ~0.31s, all matching commonly cited real DX7 figures.

PMD/PMS/AMD combination *is* numerically equivalent to Dexed's real
`Dx7Note::compute()` LFO-driven pitch/amp-mod terms (`pmod_1`/`amod_1`) —
the real integer formula `(pmd*lfo_delay * pms*(lfo_val-center)) >> 39` is
exactly four independent multiplicative factors with no cross-term, so
re-deriving it as plain fractions times one calibration constant
(`FM_LFO_PMD_MAX_CENTS = 1200 * 255*255*256 / 2^24 ≈ 1190.6` cents — just
under an octave, at PMD=99/PMS=7/LFO-extreme) is *exact*, not approximate,
verified against a standalone harness running Dexed's real integer formula
(16,646,400 raw Q24-octave units at those settings, matching this engine's
own closed-form expression exactly) — just without the Q24/Q32/`>>39`
bit-shift plumbing that only ever existed to keep Dexed itself integer-only.

Dexed's *separate* non-LFO controller-driven pitch-mod path (`ctrls->
pitch_mod`, a JUCE-plugin-configurable mod-matrix feature letting
aftertouch/breath/wheel target pitch/amp/EG independently, real DX7
hardware's own wheel assignment being a global synth setting outside any
patch's own bulk-dump data) is **not** replicated — #49's own acceptance
criterion ("mod wheel scales LFO depth") asks for the simpler, extremely
common convention `lfo.h` implements instead: mod wheel is a 0..1
multiplier on the LFO's own configured PMD/AMD depth, the same "mod wheel
→ vibrato depth" convention speech's own CC1 handling already uses (#36).
Worth flagging clearly for the first hardware listen: with `mod_wheel`
defaulting to 0, a patch with real vibrato/tremolo configured (PMD/AMD > 0)
will sound completely flat until the wheel is actually moved — by design,
not a missing feature.

#### New runtime plumbing

`VoiceParams` gained `mod_wheel` (Q15, `engine.h`) — wired into
`midi_controller.cpp` via CC1, live-pushed to held voices the same way
CC10/CC21/CC22 already are in this engine/speech. `fm_voice_note_on()`/
`fm_voice_note_off()` gained optional `FmPitchEg*`/`FmLfo*` parameters
(default `nullptr`, so any existing caller that doesn't pass them keeps
#44/#45/#48's exact old behavior) to trigger/release the voice's pitch EG
and LFO alongside its six amplitude EGs. `fm_voice_update_pitch()` (#44) is
superseded by `fm_voice_step_pitch_and_mod()`, called once per control
block from inside `fm_render_voice()` (previously the old function ran
once per whole audio buffer) — a strict precision improvement for ordinary
pitch bend, not just the new pitch EG/LFO's own home. `fm_voice_step_
envelopes()` gained an `amp_atten` parameter (default 0.0, same
behavior-neutral-default pattern #48's `rate_qrate_delta` used) that
multiplies each operator's already-computed linear gain by
`1 - amp_atten * DX7_AMP_MOD_SENS[am_sensitivity]` — real DX7 AMS is a
2-bit (0-3) field, its own small ported table (`ampmodsenstab`, Dexed's
real Q24 values re-expressed as 0..1 floats).

#### Converter (`tools/syx2patch.py` v3)

Voice-wide pitch EG (bulk offset 102-109: R1-4, L1-4) and LFO (offset
112-116: speed/delay/PMD/AMD, then a packed byte — LKS bit 0, LFW bits
1-3, LPMS bits 4-6) copied straight through as raw bytes, same "no host-
side DSP math needed" reasoning as every other field — offsets and the
byte-116 bit-packing cross-checked against Dexed's own `Cartridge::
unpackProgram` (`Source/PluginData.cpp`, Apache-2.0), not guessed from the
MIDI format sheet alone. `test_syx2patch.py` gained a dedicated round-trip
assertion (`test_unpack_voice_roundtrip`, extended) and a pass-through
check (`test_lfo_and_pitch_eg_pass_through`) — 20/20 passing (was 19).

#### Verification

Two new dedicated host checks in `render_fm` — nothing else in the suite
would exercise pitch EG/LFO at all, since `FM_TEST_PATCH`'s own `pmd`/`amd`
default to 0 (no LFO effect regardless of rate/waveform) and its
`pitch_eg.level` is explicitly `{50,50,50,50}` (no deviation).

- `run_pitch_eg_check()`: builds a patch with a real L4≠center (so trigger
  starts from a real nonzero offset), fast rise to L1 (the "attack blip"),
  settle at center, slow release swoop to L4. Verifies the exact trajectory
  (start value, blip direction/magnitude, settle value+stage, release
  motion+final value+stage) *and* the operator-increment consequence: a
  ratio operator's `inc` moves with the blip, a fixed-frequency operator's
  does not (matching Dexed's own `osc_freq()` fixed-mode exemption — pitch
  EG/LFO never reach a fixed-frequency operator on real hardware either).
- `run_lfo_check()`: all six waveform shapes sampled at known phase points
  (unit-level, direct against `fm_lfo_waveform_unipolar()`); rate/delay
  calibration anchors; a full block-rate integration run (fast LFO, max
  depth/sensitivity) showing real pitch oscillation near the calibrated
  max and real amp-mod swing; `mod_wheel=0` silencing both outputs
  regardless of patch depth; the delay ramp actually suppressing the very
  first block's output (using square, not sine, so the check isn't
  confounded by sine's own zero-crossing start); and an integration check
  that `fm_voice_step_envelopes()`'s AM path really reduces a sensitive
  operator's gain while leaving an insensitive one untouched.

One real regression caught and root-caused during this pass, not a bug in
the new code: after wiring `FmPatch`'s two new trailing members (`lfo`,
`pitch_eg`), `render_fm`'s own patch-bank render briefly showed
2nd-harmonic/fundamental ≈ 0.000 for every patch (previously real and
varied, e.g. BRASS 1 ≈ 3.9x per #57). Root cause: the *stale*, previously-
generated `patches.h` sitting in the local tree (gitignored, from before
#49's `patch.h` change) zero-initialized every patch's `pitch_eg` via plain
C++ aggregate rules — exactly the footgun documented above, hitting every
converter-generated patch at once because `tools/syx2patch.py` hadn't been
updated yet to populate it. Not a bug in the runtime (`fm_pitch_eg_trigger()`
correctly read the zero-initialized level and produced a real -4800-cent
drop, exactly as designed) or the test (the spectral probe was simply
looking at the wrong, un-shifted frequency once the real audio moved down
four octaves) — resolved by finishing the converter update and regenerating
`patches.h`, after which the 2nd-harmonic ratios returned to real, varied
values (BRASS 1 ≈ 2.77x).

`test_syx2patch.py`: 20/20 passing. `render_fm`: all checks pass, including
the two new ones. Kernel disassembly unaffected (still 48 `smlawb`
instances, device build) — confirms the "zero per-sample cost" constraint
this feature's own acceptance criteria opens with; everything here is
note-on or control-block work. `make ENGINE=fm` builds clean with and
without `patches.h`.

**Still needs the author:** hardware listen. Test first with the mod wheel
actually moved (a patch with vibrato/tremolo configured will sound
completely flat at wheel=0, by design — see this section's own note on the
mod-wheel convention chosen). Listen for: a real pitch "blip"/"scoop" on
patches whose pitch EG differs meaningfully from a flat center (many of
ROM1A's brass/bass patches do); real vibrato depth and rate that tracks
the mod wheel; tremolo (amplitude wobble) on patches with real AMD/AMS.

### Multi-Bank syx2patch, Full ROM By-Ear Pass, and the ORCH-CHIME RAM Fix

`tools/syx2patch.py convert` now takes one to four `.syx` files and
concatenates their patches into a single `patches.h`, banks in file order.
Capped at 4 files (128 patches): `FM_PATCH_COUNT` indexes Program Change and
CC30 directly, both 7-bit MIDI values, so a larger table would leave some
patches permanently unreachable. Cross-bank name collisions are
disambiguated the same way single-bank ones already were (`FMPATCH_DUP_0`,
`FMPATCH_DUP_0_2`, ...).

The author completed a full by-ear hardware pass of all 8 ROM half-banks
(rom1a/b, rom2a/b, rom3a/b, rom4a/b — 256 patches) plus the mod-wheel
check, closing that part of module_fm.md's `MAX_VOICES` re-confirmation
TODO. Two patches initially looked like bugs and were confirmed to be real,
intentional DX7 behavior instead, both matching the L4-passthrough case
already covered by Decision Record #17: **TRAIN** (ROM1A) and **ST.HELENS**
(ROM4B) never release on their own — ST.HELENS ships nonzero L4 (85-90) on
all 6 operators, not just the audible carrier, so the whole timbre (not
just TRAIN's single whistle) is designed to ring forever until voice-stolen.
**EXPLOSION** (ROM3B) looked similar (a very long decay) but is a real,
different case: its carriers all have L4=0 (genuine eventual silence), just
with R4 in the 9-17 range (DX7's rate scale runs 0=slowest..99=fastest) —
a deliberately glacial release, not a stuck one.

#### The ORCH-CHIME cost anomaly

During the by-ear pass, ROM1A's ORCH-CHIME measured a real per-voice CPU
outlier — roughly 60-70% higher duty than every other ROM1A patch at
matched voice counts — but only in a `patches.h` built from ROM1A alone (32
patches). Rebuilding with ROM1A+1B (64) or ROM1A+1B+2A+2B (128) made the
same patch measure identically to the rest of the bank, on real hardware,
reproduced by the author across three independent builds with an identical
test procedure (idle baseline on BRASS 1, then hold C2 on patch 24).

Two hypotheses were ruled out directly rather than assumed:

- **Not a converter/data bug.** ORCH-CHIME's emitted `FmOpParams` struct was
  diffed byte-for-byte between a standalone ROM1A conversion and the merged
  ROM1A+1B+2A+2B one — identical.
- **Not a codegen difference.** Both a 32-patch and a 128-patch firmware
  were built locally from the same source commit and compared: `
  audio_engine_run()` landed at the same address (`0x10000575`) and the same
  size (12676 bytes) in both, and a raw byte diff of the compiled function
  showed only 44 bytes differing, in small paired clusters consistent with
  relocated absolute-address constants elsewhere (`FM_PATCHES` itself grew
  and moved), not any change in what the loop does.

That left the data table's own flash placement. `fm_voice_step_envelopes()`
and `fm_voice_step_pitch_and_mod()` (`op.h`) dereference
`patch.op[i].am_sensitivity`, `patch.pitch_eg`, and `patch.lfo` directly
from `FM_PATCHES` every control block — continuously, for as long as a note
is held, not just once at note-on. `FM_PATCHES`'s own linker address (and
therefore ORCH-CHIME's specific byte range within it, computed from the
struct's real 212-byte size, confirmed exactly from the linker symbol size
across all three builds: 6784/32, 13568/64, 27136/128) genuinely differed
between the 32/64/128-patch builds (`0x1000ad64`, `0x1000aed0`,
`0x1000b1a0` respectively) — consistent with an XIP-cache placement/
aliasing effect: RP2350's flash cache is small and shared, and a specific
address happening to collide with something else hot (likely the render
loop's own instruction fetches, served through the same cache) would
produce exactly this signature — one patch, one build size, a real and
reproducible cost swing, gone once the address moves. The precise
collision was not (and likely cannot be, without on-chip cache
hit/miss counters) pinned down further.

**Fix**: `FM_PATCHES` is no longer `const`/`constexpr` in the generated
header, which places it in RAM (`.data`) instead of flash (`.rodata`) on
this target — confirmed by the symbol moving from `0x10009984` (flash) to
`0x20001174` (RAM) in a rebuild. RAM access has no cache-placement
sensitivity, so this doesn't just reduce the effect, it removes the
mechanism outright. Confirmed on real hardware: ORCH-CHIME's cost is flat
across all three table sizes (32/64/128 patches), at the previously-normal
lower cost, in all three. See module_fm.md Decision Record #18.

Incidental fixes made while investigating: the algorithm-4/6 warning
message (`syx2patch.py`) referenced `#54` and a "module_fm.md open question
6" that doesn't exist — `#54` was in fact already closed as "not needed"
(§5.9); the message now just states the fact without the stale citation.
The printed flash-size estimate (a hand-computed struct-layout guess) was
replaced with the real measured 212 bytes/patch.

A small side effect noticed after the RAM move, not separately chased:
16 voices + reverb on real hardware, previously measured at 95% duty
(flash-resident `FM_PATCHES`), now runs high-80s to 91% across repeated
checks. The move only targeted ORCH-CHIME's address-dependent cost, but
apparently shaved a little general XIP pressure elsewhere too. Treated as
a modest, incidental gain — no new profiling task opened for it.

### op.h Prefactor for Engine-Agnostic Kernel Reuse (#77)

Part of #76 (adding an OPL2 engine): a prefactor of `op.h` so a future
non-DX7 engine can reuse its per-sample kernel and per-block voice glue
without forking them, with no new user-facing behavior for FM itself —
verified by "DX7 is unaffected," not a demo.

Two independent changes, both scoped by #76's own review of what the
original OPL scoping conversation had missed:

- **Per-operator waveform table.** `FmOp` gained a `table` field (a
  pointer, not a link-time constant), and `op_render`/`op_render_first`/
  `op_render_fb`/`fm_voice_render_block`/`fm_render_voice` were templated
  on the table's bit width. `fm_voice_note_on()` sets every operator's
  `table` to `fm_sine_table` — FM has only ever had the one table — so the
  device kernel indexes the exact same 4096-entry table it always did, at
  the same compile-time-constant phase shift, just read through a struct
  field instead of a global name.
- **Envelope-glue de-hardcoding.** `fm_voice_note_on()`/
  `fm_voice_step_envelopes()`/`fm_voice_note_off()` previously called
  `env_dx_init`/`env_dx_step_block`/`env_dx_release` and
  `dx7_note_outlevel`/`dx7_scale_rate` directly. They're now templated on
  an envelope-glue policy type; `env_dx.h` gained `DxEnvGlue`, bundling
  Dexed's own note-on composition (velocity recovery, output level, key
  scaling) behind the same three-function shape. FM's own call sites
  (`audio_engine.cpp`, and every host tool driving `op.h` directly)
  instantiate with `DxEnvGlue` explicitly — nothing picks it by default.

No behavior changed as a result: the DX7 conformance suite
(`fm_ctl_diff.py`, 27/27) and the full four-ROM-bank spectral/envelope
regression against Dexed (`fm_regress.py`, all 40 bank/note/velocity
configs) both pass with zero threshold changes, `render_fm`'s own
self-check suite (routing, EG shape, release, pitch EG, LFO, 128-patch
bank render) all pass, and the device (`T00T_ENGINE=fm`) build is
unaffected. Real per-voice cycle cost was not re-measured on hardware —
the kernel's inner loop is unchanged (the table-bit-width shift is still a
compile-time constant; the envelope-glue call is still a direct,
non-virtual function call once the template is instantiated), so no
regression is expected, but this wasn't a hardware-gated ticket.

