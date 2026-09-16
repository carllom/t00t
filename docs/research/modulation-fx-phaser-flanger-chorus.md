# Research: Efficient phaser, flanger, and chorus for the global FX insert

Research-only — no implementation. Requested directly by the project owner (not
tied to a GitHub issue): "I would like to add more effect types, more
specifically phaser, flanger and chorus. How can I make the most efficient
implementation of these three? Where are they approximately in complexity and
estimated performance hit compared against delay and reverb?"

## Question

For each of phaser, flanger, chorus: what is its canonical DSP structure, what
does a minimal-but-correct implementation look like on this project's terms
(fixed-point Q15 vs float, buffer size, taps/stages, LFO cost), and how does
its cost likely compare to this codebase's two existing global-insert
effects — `src/fx/delay.h` (`FxDelay`, measured **51.0 c/f**, ~1.5% of the
3401 c/f/frame budget) and `src/fx/reverb.h` (`FxReverb`, measured
**268.7 c/f**, ~7.9%) — per the "Performance gain table" in
`docs/logs/history_subtractive.md` and the cross-check in
`docs/logs/history_fm.md`'s "FM P0 Measurement (#43)" section. No hardware
benchmarking was done for this doc; all phaser/flanger/chorus cost figures
below are **estimates extrapolated from operation counts**, clearly marked as
such and kept separate from cited primary-source facts.

## Primary sources

- **Julius O. Smith III, *Physical Audio Signal Processing* (PASP)**,
  CCRMA/Stanford, freely published at ccrma.stanford.edu/~jos/pasp/ — the
  canonical, citable reference for allpass-based phasing, comb-filter
  flanging, and delay-line chorus/interpolation. This project's own reverb is
  explicitly Freeverb/Schroeder-derived (`src/fx/reverb.h:7-9`: "Mono
  Freeverb (Schroeder): 8 parallel comb filters into 4 series allpasses"),
  so JOS's site is already this project's implicit DSP lineage.
- **Julius O. Smith, "An Allpass Approach to Digital Phasing and Flanging,"**
  CCRMA Tech. Report STAN-M-21 (Spring 1982) / ICMC-84 Proceedings,
  <https://ccrma.stanford.edu/files/papers/stanm21.pdf> — the original paper
  that proposes the cascaded-second-order-allpass phaser structure still used
  today (e.g. it is cited by name in Faust's `phaflangers.lib`, see below).
  Fetched and read directly (as scanned viewgraphs with equations) rather than
  relying on secondary summaries.
- **Dutilleux & Zölzer, "Modulators and Demodulators,"** ch. 2 of *DAFX:
  Digital Audio Effects* (Zölzer, ed., Wiley) — the standard DSP textbook
  chapter on flanging/chorus/phasing. Caveat: the freely-hosted copies found
  (semanticscholar mirror, dafx.de paper archive) did not extract as legible
  text via automated fetch in this session, so its specific numeric parameter
  ranges below are reported as **corroborated by multiple independent
  secondary sources** (course notes, plugin-vendor technical explainers) that
  consistently attribute the same numbers to this chapter, not as a
  first-hand quote — flagged explicitly where used.
- **musicdsp.org archive, "Phaser" code**
  (<https://github.com/bdejong/musicdsp/blob/master/source/Effects/78-phaser-code.rst>) —
  a long-standing, widely reused reference implementation in the
  demoscene/plugin-dev community; used here as a concrete "what do people
  actually ship" data point for stage count and coefficient handling.
- **Faust `phaflangers.lib`**
  (<https://github.com/grame-cncm/faustlibraries/blob/master/phaflangers.lib>) —
  a well-documented, actively maintained open-source effects library; its
  `flanger_mono`/`flanger_stereo` and `phaser2_mono`/`phaser2_stereo`
  functions are used here as a second concrete reference implementation, and
  its own doc comments cite JOS's `Flanging.html` and the ICMC-84 paper above
  — independent confirmation of the same lineage.

## What "efficient" means for this project

Recap of the two existing effects' shape, since all estimates below are
relative to them:

- `FxDelay` (`src/fx/delay.h`): one `int16_t[65536]` ring buffer, integer
  sample-indexed (no fractional/sub-sample interpolation — the "smoothed
  delay-time glide" at `delay.h:42-44` only glides the *target integer
  index*, it does not interpolate between samples), one Q15
  multiply-accumulate for feedback, `__ssat` saturation. 51.0 c/f measured.
- `FxReverb` (`src/fx/reverb.h`): 8 parallel comb filters (each with a
  one-pole damping lowpass in its feedback path) into 4 series allpass
  filters — **12 filter taps per sample** total, float math, against ~50 KB
  of buffers. 268.7 c/f measured, ≈5.3× delay's cost.

Both are engine-agnostic, `EffectParams{type, mix, p1, p2}`-driven
(`src/engine_base.h:39-44`) — three 0-127 knobs, nothing richer — and both
process mono-in/mono-out (`scratch` is the mono downmix in, wet-only mono out,
caller adds it identically to L and R). Any new effect inherits this same
contract by convention.

## Phaser

**Canonical structure.** A phaser is "any linear filter which modulates the
frequencies of a set of *non-uniformly spaced notches*" (JOS,
[Phasing.html](https://ccrma.stanford.edu/~jos/pasp/Phasing.html)), built as
a cascade of allpass filters whose coefficients are LFO-modulated, summed
with the dry signal through a depth/feed-around gain. Smith's 1982/1984 paper
(STAN-M-21) gives two concrete forms:

- **First-order per stage** (older/simpler, e.g. classic analog phaser
  pedals): `AP₁(g) = (g + z⁻¹)/(1 + g·z⁻¹)`, difference equation
  `y[n] = g·x[n] + x[n-1] − g·y[n-1]`
  ([Phasing_First_Order_Allpass_Filters.html](https://ccrma.stanford.edu/~jos/pasp/Phasing_First_Order_Allpass_Filters.html)) —
  **2 multiplies + 2 adds + 1 state register per stage.** The musicdsp.org
  reference implementation uses exactly this form with **6 stages in
  series**, LFO-modulated `g` via `Fa1 := (1 - delay)/(1 + delay)`, plus a
  feedback path (`inSamp + FOldOutput * FFeedback`) and a depth-scaled wet mix
  — i.e. the commonly-shipped "good enough" phaser is 6 first-order allpass
  stages, not more.
- **Second-order per stage** (Smith's own proposed generalization, STAN-M-21
  pp. 5-7/viewgraphs 11-16): `H(z) = (α + βz⁻¹ + z⁻²)/(1 + βz⁻¹ + αz⁻²)`,
  with `α = R²`, `β = −2R·cos θ`, where pole radius `R = e^(−πBT)` sets notch
  *width* (`B` Hz) and pole angle `θ = 2πfT` sets notch *center frequency*
  (`f` Hz), `T` = sample period. Each second-order section places exactly one
  independently-controllable notch; N sections chained give N notches
  ("theorem": a stable order-`m` allpass has `m` notches "somewhere" along
  the frequency axis, viewgraph 8). State-space (transposed direct form)
  needs 2 state registers and on the order of **4 multiplies + 4-5 adds per
  stage** (two coefficients, `α` and `β`, each used twice).

**Why no delay-line interpolation is needed.** Smith's paper explicitly lists
"Digital Delay-Line Interpolation" as the *first* problem with flanger-style
delay lines (viewgraph 5, "Problems with Flanger") and states the allpass
structure was invented specifically to have "no need" for it (viewgraph 6:
"New structure which directly implements sweeping notch filters... Property:
Allpass gain is unity"). A phaser built this way carries **no ring buffer at
all** — state is just the 1-2 IIR registers per stage — which is a real
structural difference from flanger/chorus below, not just a smaller version
of the same thing.

**Recommended minimal implementation for this project.** Given the
"efficient, simple... not audiophile" goal (`CONTEXT.md`), a 4-6 stage
first-order cascade (matching the musicdsp reference's 6 stages, or fewer for
a cheaper "good enough" default) is the natural fit: it needs only a `g(t)`
coefficient swept by one shared LFO, the same Q15 multiply/`__ssat` idiom
`FxDelay` already uses, and no buffer beyond a handful of `int32_t` state
registers (bytes, not KB). Recompute `g` once per LFO step (or once per
buffer with a `FxDelay`-style one-pole glide toward a target, reusing that
codebase idiom conceptually) rather than per-sample if the underlying
parameter update involves anything costlier than a lookup.

**Cost estimate (my own extrapolation, not measured or cited).** A 4-6 stage
first-order phaser is roughly 8-12 multiply-adds/sample of pure IIR state
math, with none of reverb's large-buffer traffic (reverb's cost comes from 12
taps against buffers of 225-1617 samples each, plus a per-tap damping
lowpass) and no ring-buffer indexing/wraparound at all (unlike delay or
flanger/chorus below). Scaling roughly from delay's measured 51.0 c/f for
~1 MAC/sample plus buffer read/write/mask overhead, I'd estimate a 4-6 stage
phaser at very roughly **80-160 c/f** — same order of magnitude as delay,
clearly below reverb, likely closer to delay's end of that range because it
avoids delay's large-buffer memory traffic entirely. This is a rough
order-of-magnitude estimate; only real hardware profiling (out of scope here)
would pin it down.

## Flanger

**Canonical structure.** JOS's original definition (STAN-M-21 eq. 1;
[Flanging.html](https://ccrma.stanford.edu/~jos/pasp/Flanging.html)): a
feedforward comb filter, `y(t) = x(t) + x(t − τ(t))`, where `τ(t)` is a
short, continuously-varying delay. Frequency response
`|H(e^jω)| = 2|cos(ωτ/2)|` — notches uniformly spaced at `1/τ` Hz intervals,
first notch at `1/2τ` Hz (STAN-M-21 p.3/VG2-3). A **feedback** variant adds a
regen path from the output back into the delay input for deeper, more
resonant notches ([Flanger_Feedback_Control.html](https://ccrma.stanford.edu/~jos/pasp/Flanger_Feedback_Control.html)),
directly analogous to `FxDelay`'s existing feedback tap.

**Delay range and rate.** Widely and consistently reported (corroborated
across multiple secondary sources attributing this to Dutilleux & Zölzer's
DAFX chapter — see the Primary sources caveat above) as roughly **0-15 ms**
delay, LFO rate around **1 Hz** (sine or triangle). At 44.1 kHz that's
0-662 samples — three orders of magnitude smaller than `FxDelay`'s
65536-sample/128 KB buffer; a 1024-sample (2 KB) `int16_t` ring buffer with
the same power-of-2 mask trick covers it with headroom.

**The interpolation problem the phaser was built to avoid.** Because flanger
notches must be *uniformly* spaced (unlike phaser's non-uniform notches),
there's no way to swap in an allpass-chain shortcut — the delay line is
structural. `FxDelay`'s existing glide (`delay.h:42-44`) only smooths the
*target integer sample index*; that's fine for echo-scale delays (tens to
hundreds of ms — sub-sample error is inaudible relative to the delay time),
but flanger's much shorter, continuously-swept delay needs true fractional
(sub-sample) interpolation, or the sweeping notches step audibly instead of
gliding. JOS's own paper flags exactly this as the flanger's problem #1
(STAN-M-21 viewgraph 5, "Digital Delay-Line Interpolation"). Two options,
per [Delay_Line_Interpolation_Summary.html](https://ccrma.stanford.edu/~jos/pasp/Delay_Line_Interpolation_Summary.html)
and [First_Order_Allpass_Interpolation.html](https://ccrma.stanford.edu/~jos/pasp/First_Order_Allpass_Interpolation.html):
  - **Linear interpolation** — "least expensive," one multiply + two adds,
    reads two adjacent taps, supports "random access" (any delay computed
    independently, no history needed) — "sounds very good when the signal
    bandwidth is small compared with half the sampling rate," i.e. plenty
    good enough for a "not audiophile" target.
  - **First-order allpass interpolation** — *identical cost* (1 multiply +
    2 adds) but has "no gain error," which JOS recommends specifically
    "inside a feedback loop" because linear interpolation's small gain
    coloration compounds every time round a feedback path. Since a
    feedback-capable flanger is the natural choice here (mirroring
    `FxDelay`'s existing feedback tap), **allpass interpolation is the
    textbook pick despite equal cost** — free correctness, not a tradeoff.

**Cost estimate (own extrapolation).** Structurally this is `FxDelay`'s
existing read/feedback-MAC/write, plus one interpolation tap (1 extra
multiply + 2 adds) and one LFO step. `history_subtractive.md`'s own numbers
already show LFO (vibrato) modulation costs are negligible on this hardware
("No measurable overhead for vibrato!", post-subchunk-fix row). So flanger
should land only slightly above delay's measured 51.0 c/f — my estimate:
roughly **55-90 c/f**. This is squarely delay-order, not reverb-order.

## Chorus

**Canonical structure.** JOS defines chorus as simulating "many... sources
singing (or playing) in unison" from one source via independently-modulated
copies ([Chorus_Effect.html](https://ccrma.stanford.edu/~jos/pasp/Chorus_Effect.html)).
The efficient implementation JOS recommends is **multiple interpolating taps
on a single shared delay line**, not N separate delay-line buffers — each tap
"oscillate[s] back and forth about the positions [it] would have while
implementing a fixed tapped delay line." This means memory cost is
effectively fixed (one buffer, sized for the longest tap delay) while compute
cost scales with tap/voice count.

**Delay range, feedback, LFO shape.** Same secondary-source-corroborated DAFX
attribution as flanger's numbers: roughly **10-25 ms** delay (longer than
flanger's 0-15 ms), typically little-to-no feedback. Faust's `phaflangers.lib`
doc comments make the flanger/chorus relationship explicit: chorus is
produced from the same `flanger`-shaped code "by increasing delay time while
setting feedback to zero and reducing level and depth" — i.e. chorus is not a
structurally distinct effect from flanger, it's the same delay+LFO primitive
retuned (longer delay, no feedback, usually 2+ decorrelated taps instead of
1). DAFX additionally distinguishes chorus's LFO as sometimes driven by a
random, low-pass-filtered-noise modulator rather than flanger's deterministic
sine/triangle, for a less mechanically-regular detune — a nuance specific to
chorus (secondary-source-corroborated, same caveat).

**Voice/tap count.** Neither JOS's page nor the Faust library pins an exact
minimum tap count for "convincing" chorus — this is a judgment call, not a
cited fact: 2-3 voices is a reasonable "good enough, not audiophile" floor
(true ensemble chorus effects commonly use 3-4+, but each additional voice is
a linear cost adder here, so 2-3 is the pragmatic starting point for this
hardware).

**Architecture note specific to chorus (not a DSP-cost issue).** JOS's page
explicitly recommends panning each tap to "its own stereo position" for
width. This project's FX insert is presently mono-send/mono-return-then-
duplicated-to-L/R by contract (`EffectParams`/`FxDelay`/`FxReverb` all
confirm `scratch` enters mono and the wet return is added "identically to
both output channels" — `delay.h:23-26`, `reverb.h:39-42`). A cheap N-voice
*summed-mono* chorus fits that contract as-is; the classic wide-stereo chorus
sound (per-voice panning) would need `process()` to return distinct L/R wet
buffers, i.e. a change to the FX insert's contract, not just a new struct —
worth flagging now since it changes what "efficient implementation" means for
chorus specifically, but out of scope for this research-only doc.

**Cost estimate (own extrapolation).** One shared buffer write (paid once,
same as delay/flanger) + N × (one interpolated read ≈ 1 multiply + 2 adds +
one LFO step). For N=2-3 voices, roughly **70-150 c/f** depending on voice
count — linear in N above a small fixed floor, and still well short of
reverb's 12-tap/large-buffer cost.

## Summary comparison

| Effect | Cost | Status | Basis |
|---|---|---|---|
| Delay | **51.0 c/f** | measured (hardware) | `history_subtractive.md` / `history_fm.md` #43 |
| Reverb | **268.7 c/f** | measured (hardware) | same |
| Phaser (4-6 first-order allpass stages) | **~80-160 c/f** | estimate | operation-count extrapolation from delay's measured MAC cost; no ring buffer at all |
| Flanger (1 tap, interpolated, feedback-capable) | **~55-90 c/f** | estimate | delay's structure + 1 interpolation tap + 1 LFO step |
| Chorus (2-3 voice, shared buffer) | **~70-150 c/f** | estimate | shared buffer-write + N×(interpolation + LFO) |

All three are structurally in **delay's order of magnitude, not reverb's**:
reverb's cost is specifically the product of 12 independent filter taps
running against comparatively large buffers (225-1617 samples each) plus a
per-tap damping lowpass — none of phaser, flanger, or chorus need that in a
"good enough, not audiophile" minimal form. Phaser needs no buffer at all;
flanger and chorus need buffers 1-2 orders of magnitude smaller than
`FxDelay`'s 65536-sample line. **None of these estimates are hardware-
measured** — they are operation-count extrapolations from delay's and
reverb's measured numbers and should be treated as planning-grade estimates
only, to be corrected against a real GPIO-duty-cycle bench pass once an
implementation exists (mirroring how `history_fm.md` #43 replaced FM's
pre-measurement reverb *reservation* with a real number).

## Implementation pitfalls worth flagging now (fixed-point/embedded relevant)

- **Interpolation choice matters more than it looks.** Linear and first-order
  allpass interpolation cost exactly the same (1 multiply + 2 adds); the
  correctness reason to prefer allpass is specifically "inside a feedback
  loop" (no gain error) — free to get right, easy to get wrong by defaulting
  to linear out of habit. See Flanger section above,
  [First_Order_Allpass_Interpolation.html](https://ccrma.stanford.edu/~jos/pasp/First_Order_Allpass_Interpolation.html).
- **Denormals argue *for* fixed-point here, not against it.** The musicdsp.org
  phaser reference explicitly adds a `kDenorm` bias term to its float allpass
  state (`kDenorm+inSamp * -Fa1 + Fzm1`) to prevent CPU stalls from denormal
  floats decaying toward silence in the feedback path — a real cost the
  existing `FxReverb` doesn't appear to guard against either
  (`reverb.h` has no anti-denormal term in its comb/allpass state). Since
  fixed-point integers cannot denormal, implementing phaser/flanger/chorus in
  Q15 (matching `FxDelay`'s idiom, not `FxReverb`'s float idiom) sidesteps
  this failure mode entirely rather than trading one cost for another.
- **LFO coefficient recompute cost.** If a phaser's second-order form is used
  (`R = e^(−πBT)`, `θ = 2πfT`), computing the exponential every sample would
  be wasteful; `FxDelay`'s own existing pattern — compute a target once (per
  buffer or per LFO step) and glide toward it with a cheap one-pole filter
  (`delay.h:42-44`) — is a directly reusable *idiom* (not code) for
  coefficient smoothing across all three effects, including first-order
  phaser's `g(t)`.
- **Harmonic notch cancellation (musicality, not performance).** STAN-M-21's
  abstract flags that a flanger's uniformly-spaced notches can, if a
  harmonic/periodic input's fundamental lands exactly on the first notch,
  silence the entire signal ("if your parents didn't have children, chances
  are you won't either" — the paper's own aside, p.1). Not a cost concern,
  but worth knowing before shipping a flanger default that can go silent on
  certain pitches.

## What this doc does not answer

No hardware benchmarking was performed — all phaser/flanger/chorus numbers
above are operation-count extrapolations, explicitly not measurements, and
should be re-derived from a real GPIO-duty-cycle bench pass (same method as
`history_subtractive.md`/`history_fm.md`) once any of the three is actually
implemented. This doc also does not decide the FX-insert architecture
question chorus raises (mono-sum vs. stereo-return contract) — that's a
design decision for whoever picks this up, not a research conclusion.
