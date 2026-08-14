# T00T — Chip Module: Development History

Dated build-phase results, measurement scorecards, and bug-discovery narrative for the chip synthesis module. See `module_chip.md` for the current spec/design.

---

*(The section below was originally recorded in `engine.md` while chip module work
was in progress — a snapshot from when the host side was complete but the
hardware measurement was still outstanding, i.e. before §14a below. Moved here
by the docs reorg splitting `engine.md` into cross-module architecture vs.
per-module spec/history; kept as its own section rather than merged into §14a
since it predates and was superseded by that section's own hardware results.)*

## Chip Module F0 — Primitives + reSID Reference Rig

`module_chip.md` P0/F0. Host side complete and green; the hardware measurement is
outstanding, so **the P0 gate is still closed** and none of `module_chip.md` §9's CPU
budget is trusted. Full results in §14a below; this section records the shape of the build
and the numbers that follow.

**Layout.** `src/chip/` holds the topology-free primitives — `sid_osc.h`
(Q24.8 accumulator, waveform logic, 23-bit noise LFSR), `env_sid.h` (the
rate-counter envelope with its piecewise-exponential segments), `sid_filter.h`
(SVF with a 3-bit mode mask, a pluggable cutoff LUT and 6581 saturation, plus
the C64 board's output network), `sid_voice.h` (the fixed-point contract),
`sid_tables.h` (generated, committed). `sid_chip.h` is the only file that knows
the real chip's three-voice/one-filter/adjacency-sync topology; it exists for
the `CHIP_STRICT` harness and for `module_chip.md` P1's register-stream playback path.

**The rig is host-first.** `tools/sid_ref/` fetches reSID 1.0-pre1 at a pinned
SHA (not vendored — GPL-2, host-only, never linked into firmware) and builds
three tools: `resid_render` (register stream → WAV), `resid_dump` (control-plane
ground truth as CSV) and `resid_probe` (filter response recordings, for the
fit). `tools/host_render/render_sid.cpp` is the t00t side with the identical
CLI, and carries its own self-check suite (`./render_sid` with no arguments) for
invariants the reSID diff cannot see: LFSR period, fixed-point headroom bounds,
table monotonicity, saturation monotonicity.

**Verification is split in two**, following the FM module's `history_fm.md` §3
conclusion, adopted here from the first commit rather than retrofitted:

- `tools/sid_ctl_diff.py` — exact, no spectra. **4/4 domains pass.** Triangle,
  sawtooth, pulse and ring's MSB substitution are bit-exact over all 4096
  accumulator phases; the noise sequence is bit-exact over 100,000 shifts with
  the period verified as exactly 2²³−1; both R-2R DAC tables are exact; the
  envelope has 0 of 12,288 trajectory points exceeding both 3 samples and 0.5%.
- `tools/sid_compare.py` — spectral scorecard over a 10-stream corpus.
  Baseline committed at `tools/sid_ref/baseline_f0.json`: mean level gap
  **−0.27 dB**, band MAE **3.54 dB**, envelope MAE **3.25 dB**, `coactive_frac`
  1.00 on every stream.

The level gap needed no calibration pass. There is one scale factor in the whole
chain (`SID_MIX_SHIFT`, a right shift), set from the arithmetic rather than
fitted, because the fixed-point contract adopts reSID's own voice-output units.

**Filter tables are measured, not derived.** reSID's 6581 filter is a
transistor-level model with 2¹⁶-entry op-amp tables and no cutoff frequency
anywhere in it, so `tools/fit_6581_filter.py` fits `sid_filter.h`'s *actual*
two-pass SVF recurrence — via its exact z-domain transfer function — to reSID's
measured response at each of 129 cutoff and 16 resonance grid points, then
interpolates to a 2048-entry table (4 KB flash, no per-sample arithmetic on the
device). Provenance is stamped into the generated header, as `module_chip.md` §5.1
requires: this is reSID `ef7873fc`'s 6581, itself a model of one measured chip.

Two limits found by the fit, both carried into P2:

- The two-pass SVF **cannot follow the 6581's top octave at 44.1 kHz** — the
  table saturates from `fc = 1440` up, the top 30% of the register range.
  Stability is not the constraint (spectral radius stays under 1 to F ≈ 27000);
  above F ≈ 22000 the lowpass simply has no −3 dB corner below Nyquist.
- The LUT is fitted on lowpass. Per-mode error on the resonance stream: LP
  5.21 dB, BP 3.39 dB, HP 7.92 dB, LP+HP 4.35 dB.

**Aliasing is the largest residual and is not in `module_chip.md` at all.** On a
sustained C4 sawtooth the harmonics match reSID within 0.4 dB up to 6 kHz;
above that t00t's non-harmonic floor is −48 dBc against reSID's −74 dBc. reSID
clocks at 985 kHz and resamples through an FIR, t00t generates at 44.1 kHz with
no band-limiting. `module_chip.md` §9's oversampling watch item is written as
conditional on hard sync; it is the oscillator's general problem.

**Measurement build.** `make ENGINE=chip` builds `src/engines/chip/rig.h` — N
voices with fixed parameters through `module_chip.md` §7.2's two-phase bus render, no
MIDI, no VM, no display, PROFILE_PIN (GPIO 22) high for exactly the render. It
steps through 0/1/4/8/16/24 voices on a 4 s hold so one capture gives the slope,
which is the per-voice cost; taking it from two builds' intercepts instead would
fold their code layout differences into it (the FM rig's #43 finding). Every
other lever is a separate build, because a runtime switch would put a branch
inside the loop being measured:

```
make ENGINE=chip                                       # 24 voices, 12 filtered
make ENGINE=chip CHIP_RIG_FILTERED=0                   # for the round-trip diff
make ENGINE=chip CHIP_RIG_MOD=1 CHIP_RIG_OVERSAMPLE=2  # sync at 2x
make ENGINE=chip CHIP_WAVE_DAC=0                       # without the 12-bit DAC LUT
```

`src/engines/chip/` is *not* the P1 skeleton — it is the smallest flashable
thing that can carry the rig onto hardware, with stub display and MIDI, and
`voice_alloc.cpp` left out of the link (`HAS_VOICE_ALLOC=0`, same as the
tracker). P1 replaces `engine.h`, `audio_engine.cpp` and `midi_controller.cpp`
wholesale.

Worth recording about the 6581's ladder, because the table looks broken and is
not: it is **non-monotonic** — 19 descending steps at 8 bits, 347 at 12, worst
−129, clustered on the major carries where the 2.20 ratio and the missing bit-0
termination compound. `dac.h` documents the mechanism. Sorting or smoothing it
would remove a large part of why a quiet 6581 note sounds dirty rather than
merely quiet.

One flash number is already measured, since it does not need a device: the
12-bit waveform DAC table costs **8200 bytes** (41380 → 33180 text with
`CHIP_WAVE_DAC=0`). Everything else in the rig's table waits for hardware.

---

## 14a. F0 results (measured, host side)

The host half of P0 is built and green. `tools/sid_ref/` (see its README for
setup) holds the reSID rig; `src/chip/` holds the primitives;
`tools/host_render/render_sid.cpp` is the `CHIP_STRICT` harness. Baseline
scorecard committed at `tools/sid_ref/baseline_f0.json`, reSID `ef7873fc` vs
`src/chip/` at this commit.

**The hardware half is done.** Per-voice cost, filtered-voice cost with the bus
round trip, sync at 1× vs 2×, FX and speaker-sim cost, and the final
20-voice/4-bus target were all measured on real breadboard_rp2350 hardware via
`src/engines/chip/rig.h` + `make ENGINE=chip`. Two real bugs were found and
fixed in the process, not just estimate-vs-measurement drift — see §14a.9.
**The P0 gate is closed.**

### 14a.1 Four documented errors in this file

Building the reference rig first paid for itself before any audio was rendered.
Each of these would have been implemented straight from the text, and each is
the kind of thing only a numeric diff finds.

| §  | This document says | The reference says | Cost of believing the doc |
|----|---|---|---|
| 4.2 | noise LFSR "taps 22/20/16/13/11/7/4/2" | feedback is `bit22 ^ bit17`; those eight positions are the *output scatter* (register bits 20,18,14,11,9,5,2,0 → output bits 11–4) | a different sequence with a different spectrum. Nothing but a listening test would catch it |
| 4.1 | hard sync is "plain `uint32_t` wrap detection — carry out of bit 31" | sync fires on the accumulator MSB *rising* | right rate, wrong phase — off by half an accumulator cycle, which on a sync lead is the whole timbre |
| 4.1 | `inc = freq_reg * 5805 (44.1 kHz, PAL)` | 5805 is a nominal 1 MHz clock; PAL is 985248 Hz → 5719 | 1.5% sharp, a quarter of a semitone |
| — | the DACs are not mentioned at all | 6581 has an 8-bit envelope DAC and a 12-bit waveform DAC, both R-2R with 2R/R = 2.20 and no bit-0 termination; the waveform DAC's zero is **0x380, not 0x800**; and the 6581's ladder is **non-monotonic** (§14a.7) | §3's own test says signal-path, so keep. The asymmetric zero is why a 6581 clicks on gate and an 8580 does not |

All four are fixed in `src/chip/`, each with the reference quoted at the site.
The remaining sections of this document are unaltered — the errors were in the
primitive-level detail, not in the architecture.

### 14a.2 Control plane: exact, 4/4 domains pass

`tools/sid_ctl_diff.py`. No spectra, no perceptual judgement.

| Domain | Result |
|---|---|
| **wave** | triangle, sawtooth, pulse and ring's MSB substitution **bit-exact over all 4096 accumulator phases**, both ring states |
| **lfsr** | **bit-exact over 100,000 shifts**; period verified as exactly 2²³−1 |
| **dac** | both tables exact — and this is an *independent* derivation, not a copy (§14a.7) |
| **env** | worst 169 samples absolute / 4.9% relative, **0 of 12,288 points exceeding both 3 samples and 0.5%** |

The envelope gate takes both bounds because the two failure modes are different
shapes: sample-grid quantisation is bounded in samples and does not grow, a
wrong rate table entry is bounded in percent and does. Gating on either alone
produces a false failure — the 24-second decay at rate 15 is 169 samples out and
0.016% wrong; the third step of a fast attack is one sample out and 5% wrong.
Neither is a defect.

### 14a.3 Signal plane: the baseline scorecard

Ten streams, `tools/sid_compare.py`. Level in dB, band/attack/envelope in dB
MAE, centroid as a ratio.

| stream | level | band | p95 | attack | centroid | env |
|---|---|---|---|---|---|---|
| saw_c3 | −0.39 | 0.85 | 3.46 | 1.06 | 1.109× | 0.19 |
| pulse_sweep | −0.49 | 1.11 | 3.83 | 1.15 | 1.114× | 0.07 |
| arpeggio | −0.46 | 1.66 | 6.09 | 1.71 | 1.130× | 0.12 |
| filter_sweep | +0.86 | 2.54 | 8.74 | 2.16 | 0.904× | 2.98 |
| sync_lead | −0.40 | 2.87 | 10.92 | 1.21 | 1.137× | 0.11 |
| filter_resonance | +0.18 | 3.60 | 11.42 | 1.59 | 0.759× | 2.67 |
| ring | −0.48 | 3.68 | 14.03 | 5.85 | 1.267× | 0.09 |
| chord_filtered | −2.65 | 4.20 | 11.36 | 2.92 | 1.107× | 8.02 |
| adsr | −0.38 | 5.14 | 20.82 | 5.10 | 1.099× | 1.37 |
| waveforms | +1.50 | 9.72 | 29.66 | 3.61 | 1.803× | 16.88 |
| **mean** | **−0.27** | **3.54** | **12.03** | **2.64** | **1.143×** | **3.25** |

`coactive_frac` is 1.00 on every stream — the envelopes agree everywhere, which
is the number the FM baseline could not produce and the reason its spectral
figures were meaningless.

**Level needed no tuning.** One scale factor exists in the whole chain
(`SID_MIX_SHIFT`, a right shift), it was set from the arithmetic rather than
fitted, and the mean level gap came out at −0.27 dB. That is the payoff of
adopting the reference's own units as the fixed-point contract (§14a.5).

### 14a.4 The three residuals, named

The mean band MAE of 3.54 dB is not diffuse. It decomposes into three specific,
understood causes, and only the first is a surprise.

**1. Aliasing — not addressed anywhere in this document, and the largest term.**
On a sustained C4 sawtooth the harmonics match reSID to within **0.4 dB up to
6 kHz**. Above that, t00t's non-harmonic floor sits at **−48 dBc against reSID's
−74 dBc** — 26 dB of aliasing. reSID clocks at 985 kHz and resamples through an
FIR; t00t generates directly at 44.1 kHz with no band-limiting at all. §9's
budget has no line for this and §4 does not mention it.

The fix is oversampling, and the cost lands on the largest line — which is
already §9's watch item, currently written as conditional on sync alone. It is
not: it is the oscillator's general problem, and sync is one instance.
`CHIP_RIG_OVERSAMPLE` measures it. The likely answer is to accept −48 dBc,
matching the FM module's parallel call on its non-interpolated sine table
(`history_fm.md` §5.3: "−55 dBc under heavy modulation is acceptable on a platform whose
stated remit is lo-fi") — but that should be a decision with a number behind it,
which it now can be.

**2. Combined waveforms — §13.5's deferral, now priced, and much worse than
"roughly TinySID grade".** Against reSID's sampled tables, over all 4096 phases:

| | mean \|err\| | max | level error in the corpus |
|---|---|---|---|
| saw+tri | 1012 / 4095 | 2720 | +15.7 dB |
| pulse+tri | 1423 / 4095 | 3840 | +3.9 dB |
| pulse+saw | 1971 / 4095 | 4080 | **+194.6 dB** |
| pulse+saw+tri | 1020 / 4095 | 2720 | **+200.4 dB** |

The pulse combinations are not approximations. reSID's tables are nonzero for
only 178 of 4096 phases on pulse+saw, so the real chip renders near-silence
where the AND renders a full-scale signal. §13.5's "AND gets to roughly TinySID
grade and is one instruction" holds for the triangle combinations and is simply
wrong for the pulse ones.

This does not change the P6 ordering, but it changes what P6 is: not polish, a
correctness fix. The 5 useful combinations × 4096 × 8 bit ≈ 20 KB flash estimate
in §4.2 stands.

**3. Filter mode — the cutoff LUT is fitted on lowpass only.** Per mode, on the
`filter_resonance` stream: LP 5.21 dB, BP 3.39 dB, **HP 7.92 dB**, LP+HP 4.35 dB.
Refitting per mode is a P2 item; the harness already has the BP and HP probe
recordings.

Two further consequences of the filter fit, both worth carrying into P2:

- **The two-pass SVF cannot follow the 6581's top octave at 44.1 kHz.** The
  fitted table saturates from `fc = 1440` upward — the top 30% of the cutoff
  register is one value. The stability bound is not what limits it (spectral
  radius stays under 1 to F ≈ 27000); the limit is that above F ≈ 22000 the
  lowpass has no −3 dB corner below Nyquist. Visible as `filter_sweep`'s
  0.904× and `filter_resonance`'s 0.759× centroid: t00t is duller than the
  reference at the open end.
- **Resonance moves the 6581's cutoff.** The joint fit measured F drifting
  6933 → 7933 across res 0 → 15 at a fixed `fc`. The Q table is fitted jointly
  so this is not charged to the damping curve, but a filter bus whose cutoff
  is swept *and* resonant will track slightly differently from the reference.

### 14a.5 The fixed-point contract, stated once

`src/chip/sid_voice.h`:

```
voice output = (waveform_dac12(wave) - wave_zero) * envelope_dac8(env)
```

which is reSID's own `Voice::output()`. Adopting the reference's scale rather
than inventing one is the direct application of `history_fm.md` §1.1(a): attempt 1 of the
FM module had no anchor, and ended with six constants whose only job was to
cancel each other out. Here there is one scale and one output shift, so any
level disagreement is a bug in a named curve rather than a tuning opportunity.

### 14a.6 One real bug the harness caught

The C64 board's output network (§10's "free bonus") is two one-poles three
decades apart, and the high-pass coefficient is 75/32768. With the state held in
output units, `(lp - hp) * 75 >> 15` truncates to zero for any difference below
437 — the integrator stops and holds whatever DC it had charged to, forever.

Measured on `saw_c3`, the simplest stream in the corpus: attack, decay and
sustain tracked reSID within 0.4 dB, then the release tail settled onto a
constant −51.5 dBFS floor instead of reaching silence. **188 dB of envelope
error, from a filter that is not part of the chip.** Envelope MAE 10.26 → 0.19
after holding the state in Q16. reSID hit the same wall and says so in
`ExternalFilterCoefficients`: "at least 27 bits of accuracy. This is crucial
since w0lp and w0hp are so far apart."

### 14a.7 The DAC tables are derived, not copied

`tools/fit_6581_filter.py` computes both R-2R tables by nodal analysis of the
ladder, from its measured resistor ratio — 2R/R = 2.20 with the bit-0
termination missing on the 6581, 2.00 with termination on the 8580. It does not
dump them from reSID, for two reasons.

**Licensing.** reSID is GPL-2 and this repo is not, which is why
`fetch_resid.sh` fetches rather than vendors. Committing 4352 entries produced
by reSID's own constructor would put back exactly the question that arrangement
exists to keep out. What is taken instead is four numbers, and they are facts
about the hardware rather than code.

**The test had no teeth.** With the tables generated from `resid_dump`, the
`dac` domain compared reSID's table against a copy of reSID's table; it reported
0/256 and 0/4096 because nothing could make it report anything else. The
derivation here uses a different algorithm from reSID's — direct nodal analysis
and superposition, against dac.h's repeated parallel substitution and source
transformation — so agreement is evidence. Perturbing the ratio by 2% now breaks
**120/256 and 4044/4096** entries; before, it broke nothing. Both tables come
out byte-identical to reSID's.

Three attempts at a structural invariant for the solve were wrong, and each was
wrong in a way worth keeping:

- *"all-ones is full scale"* — true only **without** termination. The 8580's
  ground leg draws current at that code, so its 8-bit table ends at 254.
- *"a DAC table is monotonic"* — true only **with** termination. The 6581's
  ladder has **19 descending steps at 8 bits and 347 at 12, worst −129**,
  clustered on the major carries (15→16, 31→32, 63→64) where the 2.20 ratio and
  the missing termination compound. `dac.h` says as much: "pronounced errors for
  the lower 4–5 bits … resulting in DAC discontinuities."
- The ladder *topology* itself was settled the same way. The 6581 matches with
  or without a separate termination node (an unterminated ladder has none to
  place); the 8580 matches only with its 2R going straight to ground at the LSB
  node rather than through another rail resistor.

The 6581 table looks broken and is not. Sorting or smoothing it would remove
precisely what §3 says to keep — it is a large part of why a quiet 6581 note
sounds dirty rather than merely quiet.

### 14a.8 What the hardware checkpoint must measure

`make ENGINE=chip` flashes the rig; PROFILE_PIN (GPIO 22) is high for exactly
the render, and the build steps through 0/1/4/8/16/24 voices on a 4 s hold so one
capture gives the slope. Each lever is a separate build (`src/engines/chip/rig.h`).

| Measurement | How | §9's estimate |
|---|---|---|
| per-voice cost | slope across the voice sweep | 45–65 c/f |
| filtered-voice cost, round trip included | `CHIP_RIG_FILTERED=12` vs `=0` | 40–50 c/f total |
| filter bus cost | `CHIP_RIG_BUSES` | 50–75 c/f each |
| sync at 1× vs 2× | `CHIP_RIG_MOD=1` with `CHIP_RIG_OVERSAMPLE=2` vs `=1` | not estimated |
| **oscillator oversampling in general** | as above with `CHIP_RIG_MOD=0` | **not in §9 — see 14a.4** |
| 12-bit waveform DAC | `CHIP_WAVE_DAC=0` vs `=1` | not in §9. Flash cost measured: **8200 bytes** |
| saturation | `CHIP_RIG_SAT=0` vs `=1` | folded into the bus line |

### 14a.9 Hardware results and two bugs the rig itself found

The first hardware sweep (24 voices, 4 buses, `CHIP_RIG_SAT=1`) measured
**idle at 31%** — with zero voices rendering. Chasing that down found two real
bugs, neither of them in the SID primitives themselves:

1. **The rig's own ADSR was pathological, not the engine.** `rig.h` set every
   voice to `decay=0, sustain=15` as a shortcut to reach full sustain fast. But
   decay rate 0 is the *fastest* rate period there is, and reaching sustain only
   freezes `EnvSid`'s counter — the phase accumulator kept advancing at attack
   speed forever, re-entering `tick()`'s per-sample loop 2-3×/sample for the
   life of every note instead of the ~0 times a realistic decay rate needs.
   Cost: **~62 c/f/voice**, over half the apparent per-voice overrun. Fixed by
   changing the rig's default ADSR to a slow decay toward a mid sustain
   (`rig.h`, decay rate 7) — a rig-only fix, `src/chip/env_sid.h` was never
   wrong.

2. **`sid_filter_saturate()` (`src/chip/sid_filter.h`) ran two software 64-bit
   divides per call, every bus, every sample.** Cortex-M33 has no hardware
   64-bit divide; each `int64_t/int32_t` in the cubic soft-clip's `x^3/lim^2`
   compiled to a real `__aeabi_ldivmod` library call (confirmed by
   disassembling the actual build, not inferred). With `CHIP_RIG_SAT=1`
   (default) and 4 buses, that was 8 software divides/sample from saturation
   alone — the dominant cause of the filter-bus line measuring ~3.5× over
   estimate. Fixed by rewriting the divide as a Q31 reciprocal multiply (`lim`
   is a compile-time constant); verified against the original formula across
   its full input range, max error 180 units out of a ~870k peak (0.02%, only
   at the clamp edge) — well inside this curve's own "cheap qualitative
   shape, not reSID-fitted" tolerance. Idle-with-4-buses dropped from 31% to
   9.8% after this one change alone.

With both fixed, the remaining per-voice number (~108 c/f vs the original
45–65 c/f estimate) looks like the static estimate simply being optimistic —
consistent with speech's #31 precedent, not a further bug. `sid_filter_saturate`
was the one place F0's estimate was wrong *because of a missed optimization*
rather than an optimistic guess; worth remembering next time a chip primitive
divides by a compile-time constant.

**FX and speaker sim** (`src/engines/chip/speaker_sim.h`, a P0 measurement
stand-in for the not-yet-built P5 stage — §10's shape, not its tuning) were
then measured layered on top: delay ~41 c/f (1.2%), reverb ~255 c/f (7.5%,
matching speech's own ~8% almost exactly), speaker sim ~75 c/f (2.2%, inside
§10's original 55–75 c/f estimate — the one line that held up unmodified).

**Final sweep, both fixes applied, worst case = reverb + speaker sim on top of
a full voice/bus load:**

| Config | Measured (worst case) |
|---|---|
| 24v / 4 buses (original target) | **99.9%** — no margin before the frame VM (§6, ~0.2%, not yet built) is even added |
| 20v / 4 buses (chosen) | **86.6%** |
| 22v / 2 buses (rejected) | **89.0%** |

24 voices was the number this whole document assumed going in; it does not
survive contact with a real reverb load. 20v/4f is the settled replacement —
see §9 for the full breakdown and the exchange-rate reasoning against 22v/2f.

---

## 14b. P1 results

### 14b.1 Engine skeleton

`engines/chip/` now has a real MIDI-driven render loop alongside the P0 rig,
which is preserved rather than replaced -- same idiom as the speech engine's
`SPEECH_PROFILE` flag (`make ENGINE=chip CHIP_PROFILE=1` still builds §9's
exact measurement rig, unchanged, so its hardware-verified numbers stay
re-measurable against later changes). Plain `make ENGINE=chip` now builds the
engine instead.

- `engine.h`: `VoiceType` (`VT_SILENT` / `VT_SID`, per §7.3) dispatched in the
  render loop exactly like the groovebox's, the architectural template §2
  names for this whole module. `MAX_VOICES = 32` (unchanged, the allocation
  pool from §13.3) and `VoiceParams` now carries `type`.
- `audio_engine.cpp`: one `SidVoice` per slot, dispatched by `type`. No
  filter buses yet (P2: straight to the dry mix) and no frame table VM yet
  (P3: a note is freq/pw/waveform/ADSR held static for its duration -- no
  vibrato, arpeggio, or `mod_inc` sweep). Retrigger uses
  `env.hard_restart()` (§4.3's instantaneous-reset default), not
  `gate_on()`'s hardware-literal "only attacks if not already gated" --
  deliberate, because a static channel map retriggers the same voice slot on
  every repeated note regardless of whether the previous note's release has
  finished. Delay/reverb are wired the same way every other engine does it
  (CC74/73/72/75, `engine_base.h`'s `EffectParams`).
- `midi_controller.cpp`: §8's "MIDI channel → voice, no allocator" --
  channel N plays voice N, monophonic per channel (main.cpp's own existing
  comment already named this convention). Channels 16-31 of the 32-voice
  pool sit unreachable until P4's dynamic allocator. No instrument system
  yet (P4 owns `.ins` import), so note-on uses one fixed default patch;
  two live CCs (waveform select, pulse width) are enough to hear and compare
  the primitives against reSID, which is what this phase is actually for.

### 14b.2 The `CHIP_STRICT` harness was missing, not just unmeasured

§14a's own text claimed "`tools/host_render/render_sid.cpp` is the
`CHIP_STRICT` harness... built and green," and `tools/sid_ref/baseline_f0.json`
is a real, populated scorecard. But the file did not exist on this branch --
neither did `tools/host_render/wav32.h`, which `resid_render.cpp` has
included since before P0. Whatever produced the committed baseline was never
committed itself. Since P1's own phase line calls for exactly this ("register-
stream playback path. Prove a SID voice sounds right against reSID"), both
were rebuilt from `sidreg.h`'s spec and §11.1's topology description:

- `tools/host_render/wav32.h` -- float32 WAV writer, ported from the FM
  module's `tools/fm_ref/wav32.h` (same shape, same reasoning: absolute level
  matters to `sid_compare.py`'s `level_gap_db`, so quantising to a shared
  PCM16 range would bake in a guessed headroom constant).
- `tools/host_render/render_sid.cpp` -- the strict topology itself: exactly 3
  `SidVoice`s, 1 shared `SidFilter` bound by `$D417`'s per-voice routing bits,
  `$D418` bit 7's voice-3-disconnect, a linear 4-bit volume DAC (not modelled
  more precisely anywhere else in this document, so not here either), and
  adjacency-wired sync/ring where voice v's source is voice `(v+2)%3` --
  reSID's own wiring (V1←V3, V2←V1, V3←V2). `SidBoardFilter` runs after the
  mix, matching `resid_render.cpp`'s `enable_external_filter(true)` (§10: "the
  t00t side models it too, and taking it out of the reference would make the
  comparison measure a stage neither engine is supposed to omit"). Output
  scale reuses `SID_MIX_SHIFT` -- the same constant the device render path
  already uses to bring a mixed signal into int16 range -- rather than
  inventing a second calibration.
- `CHIP_STRICT=1` is defined before including `chip/sid_voice.h`, per §11.1:
  "velocity scaling must compile out under `CHIP_STRICT`."

Also found and fixed in the process, unrelated to any of the above: reSID's
pinned source doesn't compile on a modern GCC in C++20 mode at all --
`dac.h`'s `DAC<bits>()` constructor spelling is a hard parse error on GCC 13,
not the warning `-Wno-template-id-cdtor` used to suppress. `fetch_resid.sh`
now patches it (one `sed`, standard-conformant fix: drop the redundant
`<bits>`), so a fresh fetch builds without manual intervention.

**Result:** `sid_compare.py --all` now runs end-to-end and reproduces the
committed baseline to within noise --

| metric | baseline | reproduced |
|---|---|---|
| level_gap_db | -0.271 | -0.271 |
| band_mae_db | 3.537 | 3.557 |
| band_p95_db | 12.033 | 12.115 |
| attack_mae_db | 2.636 | 2.694 |
| centroid_ratio | 1.143 | 1.143 |
| envelope_mae_db | 3.25 | 3.249 |
| coactive_frac | 1.0 | 1.0 |

-- independent confirmation of both the rebuilt harness and the numbers
already on record. The per-stream spread behind that mean is exactly what
§4.2 and §5.1 already predict: `waveforms` (AND-combined waveforms, not yet
the P6 LUT) and `filter_resonance`/`filter_sweep` (the fitted cutoff LUT) are
the worst-scoring streams, both already-documented approximations rather than
new findings.

### 14b.3 The exact control-plane diff, now built

`tools/sid_ctl_diff.py` (§11.1's other half -- envelope trajectories, the
noise sequence, the waveform logic and the DAC tables compared *exactly*,
where a spectral score could hide two errors cancelling) expects
`tools/host_render/t00t_chip_dump`, mirroring `resid_dump.cpp`'s four domains
(`env`, `lfsr`, `wave`, `dac`) -- missing for the same reason §14b.2's harness
was: whatever built it before was never committed. Rebuilt directly against
`resid_dump.cpp`'s own header comment, which names itself "the authority on
the domains." Three of the four domains are direct dumps of primitives that
already exist (`sid_lfsr_step`, `sid_combined_and`, the compiled DAC tables);
only `env` needed real care, replicating the reference's "prime the fastest
attack first, then measure" methodology sample-by-sample instead of
cycle-by-cycle.

One real bug surfaced immediately on the first run: all three *pure*
waveforms (triangle, sawtooth, pulse) failed exact comparison, pulse by a
suspicious constant 15 on every one of 4096 phases. Not a primitives bug --
`resid_dump.cpp`'s own `dump_wave()` reads through `readOSC()`, the same
8-high-bits-only OSC3 register its `lfsr` domain already documents using, so
the reference's "12-bit" wave dump only has 8 bits of real resolution and the
low nibble is always zero. `t00t_chip_dump.cpp` masked it for `lfsr` but not
`wave`; fixed by masking there too (`& 0xff0`). Worth remembering elsewhere
this OSC3 quantisation might matter and isn't obviously flagged.

```
[PASS] env    0 of 12288 points exceed 3 samples and 0.5% (both required)
[PASS] lfsr   200000 shifts compared, 0 mismatches
[PASS] wave   pure waveforms exact; AND-combined waveforms reported, not
              gated (module_chip.md §13.5) -- mean |err| 1012/1423/1971/1020,
              max 2720/3840/4080/2720, an exact match to §4.2's own
              already-quoted numbers for saw+tri / pulse+tri / pulse+saw /
              all-three
[PASS] dac    8-bit and 12-bit tables both 0/N differ
```

4/4 domains pass. Independent confirmation twice over now: the spectral
harness (§14b.2) reproduces the committed scorecard, and this exact harness
reproduces the numbers §4.2 already quotes inline -- two different rebuilt
tools landing on the same figures the original (lost) ones produced.

---

## 14c. P2 results

### 14c.1 `FilterBusParams` lands in `engine_base.h`

`VoiceParamBlockT` gained `FilterBusParams bus[FILTER_BUS_COUNT]` exactly as
§7.1 specced, and `FilterModel`/`FilterBusParams` themselves live in
`engine_base.h` (not chip's own `engine.h`) since §7.1 calls this "a change
to `engine_base.h` shared by all engines" and `FB_SVF` already anticipates a
non-SID engine wanting the same bus mechanism. The four engines that don't
use it (subtractive, groovebox, tracker, speech) each got one line --
`static constexpr uint32_t FILTER_BUS_COUNT = 0;` before their own
`#include "engine_base.h"`, the same requirement `MAX_VOICES` already
imposes -- and are otherwise untouched; all four still build.

**A real bug, caught before it reached hardware.** The first attempt
defaulted `FILTER_BUS_COUNT` via `#ifndef FILTER_BUS_COUNT #define
FILTER_BUS_COUNT 0 #endif` inside `engine_base.h` itself, to spare those four
engines the extra line. It compiled everywhere -- including chip, silently
wrong: chip's `engine.h` declares `FILTER_BUS_COUNT` as a `constexpr`, not a
macro, so the preprocessor's `#ifndef` didn't see it as already defined,
fired its own `#define FILTER_BUS_COUNT 0`, and every *later* use of the
identifier in chip's own files -- `bus_filter[FILTER_BUS_COUNT]` in
`audio_engine.cpp`, `bus_owner[FILTER_BUS_COUNT]` in `midi_controller.cpp` --
got silently token-replaced with `0`. A zero-size array is a GCC extension,
not an error, so this built clean and would have produced a firmware with no
filter buses at all, discovered only by ear or by disassembly. Fixed by
dropping the macro default entirely and requiring every engine to define the
constant, matching `MAX_VOICES`'s existing (and correct) pattern -- confirmed
post-fix by checking the actual compiled object: `bus_filter` is 32 bytes (4
x `sizeof(SidFilter)`), `bus_acc` is exactly 1024 bytes, matching §7.2's
"4 x 64 x 4 = 1 KB" arithmetic precisely.

### 14c.2 Binding policy and the live filter CCs

`midi_controller.cpp` implements §5.2's `bind_filter` exactly: a channel that
already owns a bus reuses it, an unowned channel takes any free bus, and a
channel that finds none free renders unfiltered (`BUS_NONE`) rather than
stealing one -- graceful degradation, no voice-stealing logic needed. Bus
ownership is per-channel and sticky (held for the channel's lifetime, not
released at note-off) since P2 has no envelope-silence-triggered reallocation
to hand it off to -- that is P4 territory once real instruments exist.

No per-instrument filter settings yet either (same reason), so P2 uses one
shared on/off + cutoff/resonance/mode preset (CC18/19/20/21) applied to
every currently-held note -- same global-preset shape `CC_WAVEFORM`/
`CC_PULSE_WIDTH` already used, not a per-channel toggle. `filter_on` defaults
false: §5.2's "most voices in real tunes ran unfiltered, because the filter
was scarce" is the period-correct default, not just the cheap one.

**First attempt got this wrong, caught by ear on real hardware.** CC18 was
originally scoped to `ev.channel` -- toggle *this channel's* filter request,
matching the letter of "MIDI CCs are inherently per-channel messages." Real
behaviour: playing repeated notes through a sweeping-cutoff bandpass filter,
toggling the CC produced no audible or measured change until the *next*
note. Cause: a controller whose filter knob sends CC18 on a different
channel than the notes (normal for a knob-panel controller, and matching
this project's own BeatStep Pro) updates a channel with no held note --
nothing changes until that CC's own channel later plays a note and reads the
now-toggled flag at note-on. Fixed by making CC18 global, applied to every
held note immediately regardless of which channel it arrives on, consistent
with how `CC_WAVEFORM`/`CC_PULSE_WIDTH` already worked -- the inconsistency
was the bug, not the per-channel idea in isolation.

### 14c.3 Render: two-phase, sub-blocked, idle buses skipped

`audio_engine.cpp`'s real engine now sub-blocks (`CHIP_SUBBLOCK = 64`,
reusing P0's proven value) where P1 rendered the whole buffer in one pass --
required for §7.2's two-phase shape (clear bus accumulators, render each
voice into its bus or the dry mix, filter each *bound* bus into the dry mix)
and sized right where P3's frame VM will eventually need its own sub-block
tick boundary. Per-bus idle skip (§5.2, this section's TODO closed) falls
out of the same per-sub-block voice scan for free -- no separate bus-active
bookkeeping crossing from Core 0.

Not yet re-measured on hardware: P0's numbers were taken with the rig's
compile-time-fixed routing, and this is the first time bus binding is
dynamic. Worth a bench check before P3 builds further on top, the same
"measure, don't assume" discipline §14a.9 and §14b's rebuilds both leaned on.

---

## 14d. P3 results

### 14d.1 Format decisions not pinned down by §6 itself

§6 describes the instrument model conceptually ("ADSR + vibrato + three
per-frame tables") without a byte-exact row format, so `instrument.h`
(`engines/chip/`) settles the gaps:

- **Wave-table `note` is relative, not absolute.** §6 says "note abs/rel"
  without picking one; every table-model editor's arpeggio row actually is
  relative, so that is what got built. Absolute-note rows are not
  implemented.
- **Loop semantics**: `loop >= len` holds the last row forever instead of
  wrapping (GoatTracker's convention for a non-looping table); `loop < len`
  jumps back there.
- **Pulse and filter tables share one row shape** (`SweepRow`: a per-frame
  delta held for `duration` frames) since both are ramps with identical
  stepping logic, just different targets.
- **`hard_restart`** exists in `Instrument` for P4 `.ins`-import format
  completeness only. It is never read: §4.3 already settled t00t always
  hard-restarting instantly, so an imported value carrying the 6581 delay
  bug's timing simply lands on the fast path, same as that section says.

### 14d.2 Not wired: sync/ring toggles, `mod_inc` sweeps

§4.4's per-voice sub-oscillator (`mod_acc`/`mod_inc`/`mod_mode`) was never
added to the real engine's per-voice state at P1 or P2 -- only the P0 rig and
the `CHIP_STRICT` harness's adjacency topology ever used it.
`WaveRow.flags`' two bits (`WAVE_FLAG_SYNC`/`WAVE_FLAG_RING`) are reserved in
the format but not read by `vm_frame_tick()`, and `mod_inc` sweeps aren't
built at all. Wiring the sub-oscillator into the real engine is real work in
its own right -- sync_reset/ring_msb_flip stay `0` at every `SidVoice::tick()`
call site, same as P1/P2. Half-wiring "sync toggle" without an oscillator
underneath it to toggle would be a no-op that looks implemented; left honest
instead.

### 14d.3 Frequency composition: ratios on the register, not a note recompute

The wave table's arpeggio offset and vibrato both apply as multiplicative/
additive adjustments to the SID frequency *register* Core 0 already computed
(bend included), rather than Core 1 recomputing Hz from a raw note number.
A 49-entry Q16 semitone-ratio table (`-24..+24`, `semitone_ratio_q16`,
computed once at boot the same way `env_sid_make_rates` is) handles the
arpeggio; vibrato is a frame-stepped triangle LFO added as a raw register
delta. Two consequences of this choice, both deliberate:

- **Pitch bend composes for free.** Whatever `bend_ratio` Core 0 already
  baked into `p.freq` survives arpeggio and vibrato untouched, since both
  are just further multiplies/adds on the same register -- no separate bend
  handling needed on Core 1.
- **Vibrato depth is not calibrated to cents or semitones.** It is a raw
  register-wobble scale (0-255) with an arbitrary shift constant. This is a
  by-ear tuning item, explicitly not a correctness one -- flagged here so it
  isn't mistaken for a bug when it inevitably sounds too subtle or too
  seasick on first listen.

Frame-stepped, not smoothed, on purpose: §6's "the 50 Hz steppiness is not a
limitation to smooth over... the quantisation is the sound" applies to
vibrato exactly as much as to the wave/pulse/filter tables it already
governs. A continuously-interpolated vibrato would be a different,
un-asked-for design.

### 14d.4 `FilterBusParams` goes unused by chip's own rendering

P2 built `vp.bus[]` (`engine_base.h`, §7.1) as the live channel for a bus's
cutoff/resonance/mode. P3 makes it redundant for chip specifically:
`INSTRUMENTS[]` is `const` data compiled into flash and equally reachable
from both cores, so once a voice carries an `instrument` index there is
nothing left for Core 0 to push through `vp.bus[]` that Core 1 doesn't
already have locally. `audio_engine.cpp` now reads a bound bus's tonal
parameters directly from the *feeding* voice's own instrument
(`bus_feeder[b]`, tracked in the same per-sub-block voice scan that already
computes `bus_hits[b]`) -- valid because §5.2 binding is 1:1 (a channel
shares a bus only with its own repeated notes, never with a different
channel), so "which instrument feeds this bus" is never ambiguous.
`midi_controller.cpp`'s `bind_filter()` is consequently routing-only now: it
assigns `filter_bus` and tracks ownership, and no longer writes
cutoff/resonance/mode anywhere. `FilterBusParams` stays in `engine_base.h`
for any engine that does want a live Core-0-set bus preset -- chip just
isn't one of them any more.

### 14d.5 Example instruments, and what's still missing

Four hand-authored instruments (`instruments.h`) exercise the documented
feature set one at a time -- arpeggio, PWM sweep + gate-off timer, filter
sweep + gentle vibrato, and prominent delayed vibrato alone -- so a wrong
table shows up as one wrong instrument, not a wrong chord. Selected per
channel by Program Change (real per-channel MIDI semantics: each channel
keeps its own program) or CC16 (the BeatStep-Pro-safe alternative, same
banding `CC_FX_TYPE` already uses), replacing P1's single fixed patch and
P2's manual filter CCs.

**Untested at write time -- first by-ear pass already found two real
issues.** Everything in this section was logic-reviewed and disassembly-
checked (struct sizes match expected layout, no repeat of §14c.1's zero-size-
array class of bug) but not heard, before it was heard:

1. **`ARP_LEAD`'s arpeggio was far too fast.** Wave-table rows have no
   duration field (unlike pulse/filter's `SweepRow`) -- one row is one
   frame, so the original 4-row table (one row per note) cycled at
   50/4 = 12.5 Hz, 20 ms/note. Reported as the instruments sounding "rough"
   and "grainy." Fixed by repeating each note's row 2x (40 ms/note,
   160 ms/cycle, ~6.25 Hz) -- the standard tracker convention for "hold" in a
   1-frame-per-row table, an authoring fix, not a VM one.
2. **Vibrato depth was miscalibrated by ~4 bits, not just "uncalibrated."**
   The `>>8` shift this section already flagged as a by-ear item put
   `FILTER_PAD`'s depth 15 at ~22% frequency deviation and `VIBRATO_LEAD`'s
   depth 40 at ~58% -- a siren, not vibrato, and the more likely dominant
   cause of "rough/grainy" on the instruments that have no arpeggio at all
   to blame instead (`ins.vibrato_depth`'s own comment predicted "too subtle
   or too seasick," not that it would be off by an order of magnitude).
   Changed to `>>12`: depth 15/40 now land around 1.3%/3.6%, a reasonable
   first guess, still not a calibrated one.

Both fixes above were logic-only corrections to already-identified by-ear
tuning items. The next report was a real bug in the mechanism itself, not a
tuning item: `ARP_LEAD`'s notes sounded "too close together in pitch" (cycle
timing confirmed correct) and the waveform sounded "really weird."

3. **`osc.inc` was being stomped back to the raw base pitch every buffer.**
   The per-buffer param-apply loop unconditionally ran
   `voice[v].osc.inc = sid_freq_to_inc(p.freq, acc_scale_g)` -- every ~5.8 ms
   (`SAMPLES_PER_BUFFER`/`SAMPLE_RATE`), regardless of whether a frame tick
   had just run `vm_frame_tick()` and set `osc.inc` to include the arpeggio
   offset (or vibrato). A ~882-sample frame period spans ~3.4 256-sample
   buffers, so the modulated pitch only survived until the *next* buffer --
   roughly 1 buffer in 3.4, the rest snapped back to root. That is a
   buffer-rate (172 Hz, §6.1's own "Core 1 reads `ParamExchange` at 172 Hz"
   figure) alternation between the true pitch and the unmodulated one: never
   cleanly sustaining the interval ("too close together"), and a sawtooth's
   fundamental being yanked at 172 Hz produces real FM-sideband-like buzz
   ("weird waveform"). Verified independently of the render loop first, not
   just reasoned about: a host-side simulation of the ratio math and of a
   full 24-tick cycle both confirmed root/+4/+7/+12 land exactly on notes
   60/64/67/72 in isolation, which is what pointed at *use* of `osc.inc`
   rather than its *computation* as the actual bug.

   Consistent with why `PWM_PLUCK`/`FILTER_PAD` read as fine and `ARP_LEAD`
   didn't: every non-arpeggio, non-vibrato instrument's "modulated" pitch
   *is* the base pitch, so stomping back to base is a no-op for them; a ~1.3%
   vibrato deviation snapping in and out is far below the threshold an
   octave-spanning arpeggio blows straight through.

   Fixed by moving the initial `osc.inc` assignment into the trigger-change
   block (sets the correct raw pitch once, immediately, before the first
   frame tick can) and removing the unconditional per-buffer reassignment --
   `vm_frame_tick()` is now the sole ongoing authority on `osc.inc` for a
   held note's lifetime, matching the comment that was already there
   claiming exactly that and not, until this fix, actually true.

4. **Vibrato onset jumped to the bottom of its swing instead of easing out
   from zero, and both instruments' `vibrato_speed` were uncalibrated by
   roughly the same order of magnitude as the original depth constant.**
   `vib_phase` was reset to 0 at trigger, but the triangle mapping has
   `tri(phase=0) = -16384` -- the *trough*, not the center -- so the instant
   a delayed vibrato started, it snapped straight to the bottom of its range
   rather than rising smoothly from no deviation. Fixed by resetting to
   `16384` instead, where `tri = 0`. Separately, `VIBRATO_LEAD`'s
   `vibrato_speed = 10` produced a ~2.0 s cycle (reported as "quite slow,
   perhaps over a second per cycle" -- exactly what the math gives) against
   an ordinary vocal/instrumental vibrato target of 4-7 Hz; `FILTER_PAD`'s
   `speed = 6` was worse, ~3.4 s/cycle. Neither value had ever been checked
   against a real target rate. Retuned to 110 (~5.4 Hz, `VIBRATO_LEAD`) and
   40 (~2.0 Hz, a deliberately gentler pad wobble, `FILTER_PAD`) -- the phase
   scale itself (`vibrato_speed * 64` per frame) already covers a reasonable
   0-12.5 Hz range at the full 0-255 input; the bug was the two chosen
   values, not the formula.

5. **`PWM_PLUCK`'s sweep "jumps to another value" after it finishes -- but
   the value itself doesn't jump.** A host-side simulation of the exact
   `SweepRow` state machine confirmed the pulse width lands precisely on
   3600 and the following hold row starts from exactly 3600 -- no value
   discontinuity anywhere. What *is* discontinuous is the *rate of change*:
   a flat delta of +90/frame running straight into a delta of 0/frame is a
   kink in the derivative, and the ear hears that as a "jump" even though
   the number itself never does -- the classic reason a swept parameter
   stopping cold reads as a click. Fixed by tapering the last few rows
   (90 -> 45 -> 20 -> 8 -> 0 per frame) instead of a hard stop, same 40-frame
   total. Separately noticed while re-deriving the table: `gate_off_timer`
   (30 frames) was firing *during* the original 40-frame sweep, cutting the
   pluck's own signature effect short before it finished -- bumped to 45 so
   release only starts once the (now-tapered) sweep has settled.

6. **`gate_off_timer` caused a spurious full re-attack, not a graceful
   auto-release -- the real cause of "volume jumps up again and tapers
   down."** `vm_frame_tick()`'s timer calls `env.gate_off()` directly, which
   clears the envelope's own internal `gate` flag. But the per-buffer loop
   unconditionally ran `if (p.gate) env.gate_on()` every buffer, and `p.gate`
   -- the *MIDI* gate, tied to whether the key is still physically held --
   never went false. So the very next buffer saw `p.gate == true`, found the
   envelope no longer gated, and re-triggered a full ATTACK: one spurious
   re-attack immediately after every timed auto-release, same envelope
   shape as the original note-on (confirmed by ear: "it looked exactly the
   same shape as the retrigger"). Fixed with a `gate_off_fired` guard --
   once the timer has fired, the per-buffer loop stops reasserting
   `gate_on()` for that voice until a genuine new trigger (`vm_reset()`)
   clears the flag again.

   This fully accounts for what was heard; a second, independent
   observation from the same report -- "at start the volume is high and
   tapers down" -- turned out on closer listening to be the ordinary attack
   phase, not a separate artifact (the same envelope shape as the confirmed
   re-attack, which is exactly why it looked identical).
7. **`pulse_cur` started at the degenerate `pw = 0`, a real issue on its own
   merit even though it wasn't the cause of finding 6.** `sid_pulse()`'s
   `top12 >= pw` is true for *every* `top12` when `pw = 0`, so a pure pulse
   waveform outputs a constant, non-oscillating level -- not audio -- until
   the first frame tick corrects it. Added an explicit `pulse_init` field
   (`instrument.h`, mirroring `filter_cutoff_init`'s existing pattern) so
   `vm_reset()` seeds a real starting pulse width instead of the degenerate
   default. Also caught in passing: `FILTER_PAD`'s waveform is `0x6`
   (SAW|PULSE combined, §4.2's AND-combination), so its *static* pulse width
   matters even with no sweep table -- at the old implicit `pw=0` its PULSE
   component was a permanent no-op and the "combined" waveform was silently
   just SAW. Both now seeded to real values (200 and 2048 respectively)
   instead of 0.

**The streak breaks here, which is itself useful signal.** A follow-up
question -- "is PWM_PLUCK a 2-voice instrument?" -- turned out to be the
pulse sweep's own shifting harmonics (a single oscillator's PWM sweep is
well known for a thickened, near-chorus quality on its own) mistaken for a
second voice. Not a bug: this engine has no unison/layering mechanism at
all, every instrument is strictly one `SidVoice` (§4.4's "every note
occupies exactly one voice"). Recorded so the tally below doesn't read as
"every report is a bug" pattern-matching -- seven real fixes and one correct
"working as intended" out of eight questions is what a well-calibrated
by-ear pass actually looks like.

Seven for seven bugs so far: every reported "sounds wrong" has been a real,
fixable issue, not an expectation mismatch -- worth keeping that base rate in
mind for what's still unheard. The frame VM's timing, hard-restart
interaction, and the sub-block-quantised tick's own audibility are still
owed a real listen beyond what these seven findings covered.

---

## 14e. P4 results

**Built, all-engine build regression clean via the top-level `make` (not raw
`cmake` -- that skips the Pico cross toolchain and fails on `__ssat`/board
defines).  Not yet heard on hardware** -- unlike P1-P3, no by-ear pass has
happened for this phase yet, and that should be the next checkpoint before
trusting any of this section's design calls the way §14d's seven findings
got trusted only after real speakers disagreed with several of them.

### 14e.1 Dynamic voice allocation

`voice_alloc.*` reused unmodified per §8's own instruction, but two things
P1-P3's static one-voice-per-channel model never had to get right showed up
immediately once allocation went dynamic:

- **`active_mask` was computed from `p.type == VT_SID`**, which is set once
  at note-on and never cleared -- every voice would have read as
  permanently "active", so priority 1/2 (steal silent/steal released) of
  the three-tier policy could never fire and every allocation would fall
  through to stealing the oldest held note regardless of whether quieter
  voices existed. Fixed to `voice[v].env.counter > 0` (`EnvSid::counter` is
  the literal audible-amplitude proxy) so the bitmap means "still audible",
  matching what the allocator's steal policy actually needs it to mean.
- **Pitch bend was a single global `bend_ratio`** in P1-P3, correct only
  because the monophonic-per-channel model never had two channels sounding
  at once to expose it as wrong. Made per-channel (`channel_bend[16]`),
  live-pushed to every currently-held voice on that channel on
  `MIDI_PITCH_BEND`, same shape as speech's live-CC push pattern.
- **Filter-bus binding (§5.2) stays per-channel, not per-voice** -- multiple
  simultaneous notes on one channel (a chord) share that channel's one
  bound bus, which is exactly §5.2's "already owns a bus -> share it" rule,
  just no longer limited to one note at a time to demonstrate it.

`midi_controller.cpp` was rewritten around `midi_note_voice[128]` (note
number -> allocated voice) + `voice_held[]`/`voice_channel[]`/`voice_note[]`,
the same shape speech's controller already used for #36 -- speech got there
first, chip's version differs only in adding `voice_note[]` for pitch-bend
reverse lookup, which speech's controller doesn't need since it has no
pitch-bend handling at all. `CMakeLists.txt` now links `voice_alloc.cpp` for
chip like every engine except tracker.

**Hardware-observed bug (the author, first P4 by-ear/by-scope pass): CPU duty
cycle never comes back down after release.** Hold 8 keys and release all of
them -- the duty cycle stays pegged at the 8-voice level. Press and hold new
keys one at a time afterward and it stays exactly where it was, only rising
again once more than 8 keys are held at once. Root cause: `VoiceParams.type`
is set once at note-on and **never reset to `VT_SILENT`** -- dynamic
allocation reuses a slot by flipping `p.gate` only. The render loop's sole
gate for doing any DSP work (`p.type != VT_SID`) therefore stays true
forever once a slot has been touched even once, so Core 1 keeps fully
ticking that voice's oscillator/envelope/bus-feed indefinitely, long after
it has actually decayed to silence -- CPU cost tracks the high-water mark of
voice slots ever used, not currently-audible voices, and the *audio* itself
was never wrong (a fully-decayed envelope is inaudible regardless), which
is why this surfaced on a duty-cycle read rather than by ear. Fixed with a
second per-buffer bitmask, `render_mask` (`p.gate || env.counter > 0`,
alongside the existing `active_mask`'s `env.counter > 0`), used in place of
the `p.type != VT_SID` check in both the frame-VM tick loop and the
per-sample tick/bus-feed loop; `|| p.gate` (not `active_mask`'s bare
`counter > 0`) matters specifically for the sample right after
`hard_restart()`, which resets `counter` to 0 before the attack has had any
samples to ramp it back up.

### 14e.2 Hand-authored text format + `chipgen.py`

`tools/chip_instruments.txt` (source) → `tools/chipgen.py` (generator) →
`src/engines/chip/instruments.h` (device header), same host-authors/
device-ships-a-table split as `speechgen.py`. `instruments.h` is now a
generated file; the four hand-tuned P1-P3 instruments (`ARP_LEAD`,
`PWM_PLUCK`, `FILTER_PAD`, `VIBRATO_LEAD`) were ported into the text format
and the generator's output verified byte-for-byte identical to the prior
hand-written header before it was replaced. Negative-tested with four
deliberately malformed inputs (bad waveform name, out-of-range
`wave_loop`, missing `end`, duplicate instrument name) -- all produced
clear, line-numbered errors and non-zero exit, same discipline as
`speechgen.py`'s own error path.

### 14e.3 GoatTracker `.ins` converter (`tools/ins2chip.py`)

An earlier web search claimed a fixed "GTI5" header (magic at offset 0, then
AD/SR/Wavepointer/Pulsepointer/Filterpointer/Vibrato at offsets 4-9) --
unverified, and wrong: it doesn't match either GoatTracker's own source or
two real `.ins` files pulled from the same repo. Per this project's
"verify against primary source" rule (the same one `fetch_resid.sh` exists
for), the byte layout actually used was read out of
[leafo/goattracker2](https://github.com/leafo/goattracker2)'s own
`src/gsong.c` (`saveinstrument()`/`loadinstrument()`), `src/gcommon.h`
(`INSTR` struct), and `src/gplay.c` (the row-execution semantics --
wavetable delay/command/jump rows, pulse absolute-set vs. delta+duration
rows, filter type/ctrl/cutoff vs. delta+duration rows), then cross-checked
by hand-decoding two real files (`examples/sfx_arp1.ins`,
`examples/sfx_gun.ins`) byte-by-byte against that source and confirming the
decode consumes the file exactly (both landed on `file length == bytes
consumed`, the strongest available check with no independent parser to
diff against).

The converter emits `chip_instruments.txt`-format text, not a header
directly -- it feeds `chipgen.py` rather than replacing it, which is what
§1's own P4 line ("host converter; ... → header") already implied as a
two-stage pipeline once both halves existed.

**Scope is deliberately narrower than the full `.ins` format**, refusing
(hard error, not a silent wrong translation) rather than guessing at
constructs that don't map onto chip's model:

- WAVECMD rows (0xF0-0xFE wavetable bytes: portamento, vibrato-via-table,
  set-AD/SR/wave/filterptr/filterctrl/cutoff/mastervol) -- these dispatch
  through GT's per-tick command executor, which the frame VM has no
  equivalent of.
- Absolute-pitch wavetable rows (note byte ≥ 0x80, used as a literal
  freqtable index instead of an offset from the played note) -- chip's
  `WaveRow.note` is always relative to the played note; there's no slot for
  "ignore what was played, use this fixed pitch". **Both real example
  files hit exactly this case** (they're one-shot SFX instruments with
  hardcoded absolute pitches, not melodic patches) and were correctly
  refused rather than mistranslated.
- A pulse or filter absolute-set row anywhere but the start of its table --
  chip has one `pulse_init`/`filter_cutoff_init`, not a reseekable pointer.
- GT's real per-instrument vibrato is command/speed-table-driven, not a
  constant depth/speed/delay triple; a standalone `.ins`'s `vibdelay` byte
  only means anything paired with a pattern-level vibrato command that
  lives in the `.sng`, not the `.ins`, so it can't be reconstructed from an
  instrument file alone regardless of engine differences.

**The author pushed back on the first version of this section** ("how do we know
we can import goattracker instruments? Have we tried with proper community
instruments?") -- rightly: the only verification at that point was two real
files (both correctly refused, proving nothing about the positive path) and
a hand-built synthetic file checked against *my own* understanding of the
spec, which is circular -- it can confirm the code does what I think it
should, not that what I think it should do is right. Real testing required
real community content.

`leafo/goattracker2`'s `examples/*.sng` are real GoatTracker songs (demo
tunes with named, played instruments -- `dojo`, `hyperspace`,
`cabrinigreen`, `sixpack`, and others). A throwaway script
(`gettablepartlen()`'s own algorithm, gtable.c: scan forward from an
instrument's table pointer to the first `0xff` row, inclusive) extracted
every instrument from 9 of these songs as standalone `.ins` blobs, byte-
identical in shape to what GoatTracker's own "export instrument" would
produce -- **200 real, community-authored instruments**, not two SFX files
and a synthetic guess.

First pass: 7/200 converted. Investigating the failures found two real bugs
in `ins2chip.py` itself, not scope limitations:

- **The wave-table delay-row fold was wrong.** A raw row with `wave` in
  1-15 holds the *previous* row's pitch, but a `note` byte on that same row
  still gets applied -- on the *last* tick of the hold, once it expires. My
  first pass required delay rows to carry no note at all (`note == 0x80`),
  which is only the "pure repeat" case; a delay row *with* a real note is
  an ordinary, common authoring pattern (arpeggios overwhelmingly use it --
  every `cabrinigreen` instrument literally named "arp minor/major
  7/9" hit this). Traced tick-by-tick against `gplay.c`'s `WAVEEXEC` and
  cross-checked against `16_arp_minor_7.ins`'s actual decoded note sequence
  (0, +3, +7, +10, +12, +10, +7, +3, 0, loop) -- a real, musically correct
  minor-7 arpeggio, which a wrong reading would not have produced by
  coincidence. Fixed: `note == 0x80` extends the previous row by `w+1`
  ticks (nothing changes); a real note extends it by `w` ticks then emits
  a separate one-tick transition row (waveform untouched, since a delay
  row never sets `cptr->wave`).
- **Loop-jump targets were resolved as if "raw row index" and "emitted row
  index" were the same number.** They aren't once any row folds (delay
  rows, or a pulse/filter table's leading absolute-set row, don't produce
  their own `rows` entry) -- a jump could resolve to the wrong row, or to
  a "row" that was folded away entirely. Fixed with an explicit
  raw-index → emitted-index map built while decoding, so a jump's target
  is looked up rather than assumed. Also found and fixed: a raw jump target
  of exactly `0` is GT's own null-pointer convention ("stop, don't loop",
  same as `loadinstrument()`'s own `if (rtable[c][d])` guard skips
  relocating it) -- not literally row 0, which the first pass had backwards
  often enough to matter.

Second pass: 53/200. Two more *legitimate* patterns turned up as the
dominant remaining failure, not bugs but also not un-fixable: a pulse or
filter table resetting its absolute value **partway through** a sweep (16
files), and a loop jumping back to the table's own initial absolute-set
row (7 files) -- both real GT features with no direct chip equivalent
(one `pulse_init`/`filter_cutoff_init`, not a reseekable pointer), but both
staticly computable: since the converter already walks the whole table, it
can simulate the running pulse/cutoff value through every prior
delta/duration row (clamped exactly as `vm_frame_tick` clamps it) and
synthesize a one-frame delta row landing on the same absolute value,
instead of refusing. Third pass: **89/200 (44.5%)**, all 89 also verified
through `chipgen.py` end-to-end with zero further failures.

That last check caught a real bug in **`chipgen.py`**, not `ins2chip.py`:
its keyword dispatch treated a bare `pulse` line as *always* opening a new
pulse-sweep section, even when it was actually a wave row's own first
token (`pulse` is a valid `WAVE_BITS` name too) -- `wave` / `pulse 15` /
`pulse 12` / ... silently mis-parsed the second and third rows as a
section header instead of a waveform. None of the four original hand-
authored instruments ever happened to start a wave row with `pulse`, so
this was invisible until real content (an "echo arp" instrument using a
pulse-then-tri wave sequence) exercised it. Fixed with a `len(parts) == 1`
guard so the bare-keyword and wave-row-content cases are distinguished by
token count, the same way `filter`'s multi-token header line already had
to be. `instruments.h` re-verified byte-identical after the fix.

The remaining 111/200 failures are, on inspection, overwhelmingly correct
refusals rather than gaps: 58 absolute-pitch wavetable rows and 36 sync/
ring/test-bit instruments (both documented as out of scope above), plus a
handful of table-sharing tricks (an instrument's table segment is *itself*
just a jump into a differently-owned region of the original song's shared
table -- not reconstructable from that one instrument alone, and arguably
not "self-contained" even inside GoatTracker, the same point §11.2 already
makes about sync).

---

## 14f. P5 results

**Built, all-engine build regression clean (both `CHIP_PROFILE=0` and `=1`
configs). Not yet heard on hardware** -- same status P3/P4 carried before
their own by-ear passes; the numbers below are a documented first guess
against §10's ranges, not a tuned or measured result.

### 14f.1 Speaker simulation output stage

`speaker_sim.h`'s `SidSpeakerStage` (already built at P0-gate quality, one
fixed HP/peak/LP corner set, for the P0 rig's cost measurement only) now
carries the five presets §10 named -- `SPEAKER_1702`/`TV`/`GAMEBOY`/
`ARCADE`/`BYPASS` -- as one shared HP→peak→LP→soft-clip chain with per-
preset corner frequencies and a `drive` scalar into one fixed cubic clip
curve, rather than five separate signal paths. Wired into the real engine's
render tail, downstream of the FX insert and upstream of the final `__ssat`
(`module_chip.md` §15 open question 4, resolved) -- previously only reachable from the
`CHIP_PROFILE` rig's `CHIP_RIG_SPEAKER` flag. `res2p_init()` (the peak
stage's pole-radius LUT, needed once before any `res2p_set()` call) was
missing from the real engine's boot -- added; the rig already had its own
copy via a different init path, so this had never been exercised end-to-end
before.

---

*(§15 Open questions and §16 Glossary have moved to `module_chip.md` — they're forward-looking/reference material, not dated results.)*

**`BYPASS` was first built as corners pushed outside the audible-effect
range (20 Hz HP, 20 kHz LP, a deliberately wide-bandwidth peak) rather than
a conditional skip, on the reasoning that it should exercise the same DSP
as every other preset. The author heard it anyway** -- "a tiny bit dull" against
the pre-P5 build. Real cause: `res2p_radius()` (res2p.h) clamps to its LUT's
last entry rather than extrapolating past `RES2P_RADIUS_X_MAX`, so the
intended ~4 kHz peak bandwidth silently became ~1.1 kHz at 44.1 kHz -- a
real, if broad, resonant bump, not the negligible one the preset row aimed
for. A resonant 2-pole filter can't actually be tuned into a flat response
by widening `bw` past what the pole radius supports; only a real
passthrough is guaranteed transparent. Fixed: `tick()` short-circuits to
`return x;` when the current preset is `SPEAKER_BYPASS`, before touching
any of the HP/peak/LP state -- an actual bypass, not an approximation of
one.

Preset selection is global, not per-voice, but `VoiceParams` is the only
Core0→Core1 channel chip has (§7.1) -- rather than widen the shared
`VoiceParamBlockT` (`engine_base.h`) for one scalar every other engine
would carry unused, the chosen preset is replicated identically to all
`MAX_VOICES` slots on CC17 and Core 1 reads it once from voice 0. CC17 was
free and sits inside the BeatStep Pro's absolute-CC encoder range
(16-31) other CC-selectable controls already target for that reason.

### 14f.2 LCD UI

`display.cpp` (previously a stub -- "no display yet, module_chip.md §1 P5 owns
this") now shows VOICES/CPU/NOTE (same shape as subtractive's and speech's
displays), the active speaker preset name, the current instrument index,
and a fixed 8-voice grid (of `MAX_VOICES = 32`) showing each voice's
instrument + current wave-table row, colour-coded held vs. ringing-out.
32 voices don't fit a full one-cell-per-voice grid the way speech's 8 do;
the grid is fixed to voices 0-7 rather than scrolling, which `voice_alloc`'s
own allocation order (always scans from `v = 0` first, `src/voice_alloc.cpp`)
makes representative for anything up to 8-note polyphony -- past that the
VOICES count row is still exact, the grid just stops being the whole
picture. Instrument names aren't shown (only the numeric index) --
`chipgen.py` doesn't currently emit a name table, and adding one felt like
scope creep on top of everything else this phase already touched; a
reasonable next small addition, not a gap in the design.
