# Research: OPL3/4's full 8-waveform set

Resolves [#139](https://github.com/carllom/t00t/issues/139), part of wayfinder map
[#136](https://github.com/carllom/t00t/issues/136).

## Question

`src/engines/opl/waveforms.h` implements OPL2's original 4 selectable operator
waveforms (register values 0-3: sine, half-sine, full-wave-rectified/abs-sine,
quarter-sine pulse). Real OPL3/OPL4 hardware extends the waveform-select field
to 3 bits, adding 4 more shapes at register values 4-7. What are they,
precisely enough to generate a lookup table from?

## Primary source

[Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3) (nukeykt), a cycle-accurate
OPL3 reimplementation reverse-engineered from real chip die analysis — vendored
into this repo's own tooling at `tools/opl_ref/` (fetched by
`tools/opl_ref/fetch_nuked_opl3.sh`, pinned at commit
`cfedb09efc03f1d7b5fc1f04dd449d77d8c49d50`, not committed to this repo per its
LGPL-2.1 license — see that script's own comment). This module already treats
Nuked-OPL3 as its ground-truth reference implementation for other purposes
(`nuked_dump.cpp`, `nuked_render.cpp`), so its source is read directly here
rather than a secondary description.

All line numbers below are against `tools/opl_ref/nuked/opl3.c` at that pinned
SHA.

## How Nuked-OPL3 generates a waveform sample

`envelope_sin[8]` (line 357) is an array of 8 function pointers,
`OPL3_EnvelopeCalcSin0` through `Sin7` (lines 220-355), indexed directly by
the operator's waveform-select register field (`slot->reg_wf`, used at line
692: `envelope_sin[slot->reg_wf](slot->pg_phase_out + *slot->mod, slot->eg_out)`).
This *is* the register-index-to-shape mapping — 0-7 map to `Sin0`-`Sin7` with
no reordering.

Each `SinN` function takes a 10-bit `phase` (0-1023, one full cycle — masked
with `phase &= 0x3ff` in every variant) and an envelope attenuation value, and
returns a signed sample. Internally every variant does the same two-step
thing:

1. Compute a log-domain attenuation value `out` from `phase`, usually via
   `logsinrom` — a 256-entry ROM (lines 75-108) storing one quarter-cycle of
   `-log2(sin(x))`-shaped attenuation (12-bit fixed point, ~0.375 dB/step:
   `0x859` at the quietest end near a zero-crossing, decreasing to `0` at
   peak amplitude).
2. Convert `out + (envelope << 3)` from log domain back to a linear sample via
   `OPL3_EnvelopeCalcExp` (line 211) — a table-driven power-of-two conversion
   backed by `exprom` (256-entry mantissa table, lines 114-147: exactly
   doubling from `0x400` to `0x7fa` across its span) with `level >> 8`
   selecting the integer octave shift. The net effect, confirmed against
   `exprom`'s doubling range: `EnvelopeCalcExp(level) ∝ 2^(-level/256)`.

So `logsinrom` + `OPL3_EnvelopeCalcExp` together are simply *how the chip
computes an ordinary sine curve in a cheap log/antilog domain* — an
implementation detail, not part of the waveform's actual geometry, for
shapes 0-3 and (as shown below) 4-6. This matches this module's own existing
precedent: `waveforms.h`'s comment and `docs/module_opl.md`'s Decision Record
already state this module's tables are "a plausible approximation... not a
port of Yamaha's own logarithmic sine + antilog tables — exact reproduction
is out of scope." Shape 7 is the one exception — see its section below.

### Confirming shapes 0-3 against the existing implementation

Decoding `Sin0`-`Sin3` (lines 220-287) against their phase-bit tests confirms
they match `waveforms.h`'s existing four tables exactly in geometry:

- **Sin0** (`ws=0`): sign flips on bit `0x200` (second half of cycle
  negative), magnitude mirrors on bit `0x100` — a full sine, one cycle.
  Matches `opl_wave_sine`.
- **Sin1** (`ws=1`): forced-silent (`out=0x1000`, saturates to ~0 through
  `EnvelopeCalcExp`'s `level > 0x1fff` clamp) whenever bit `0x200` is set,
  ordinary sine magnitude otherwise, no sign flip — positive half-sine hump,
  silent second half. Matches `opl_wave_half_sine`.
- **Sin2** (`ws=2`): same magnitude mirroring as Sin0, but no sign bit at all
  — always positive, two humps per cycle. Matches `opl_wave_abs_sine`.
- **Sin3** (`ws=3`): forced-silent on bit `0x100`, else `logsinrom[phase &
  0xff]` used *unmirrored* — an ascending quarter-sine, repeated (since only
  the low 9 bits are consulted) twice per full cycle. Matches
  `opl_wave_quarter_sine`'s "one full positive hump via `sin(2*theta)`,
  silent for the rest" shape (Nuked's addressing is a different bit-trick
  but traces the identical curve).

This cross-check gives confidence the same bit-decoding approach, applied to
`Sin4`-`Sin7`, yields the real shapes 4-7.

## The 4 new shapes (register indices 4-7)

Notation matches `waveforms.h`'s existing style: `theta = 2*pi*i/OPL_TABLE_SIZE`
(one full cycle), `N = OPL_TABLE_SIZE`.

### WS4 (index 4) — double-frequency signed sine, first half-cycle only

Source: `OPL3_EnvelopeCalcSin4`, lines 289-311. Decoding the bit tests: sound
only when phase's bit `0x200` is clear (`phase < 512`, i.e. `theta < pi`);
magnitude addresses `logsinrom` at *double* rate (`(phase << 1) & 0xff`,
mirrored on bit `0x80`) — the same double-frequency addressing trick as
`Sin3`/`opl_wave_quarter_sine`, but spanning a full mirrored cycle instead of
one quarter; sign flips when `(phase & 0x300) == 0x100`, i.e. for
`theta` in `[pi/2, pi)`. Combined, this traces one full period of `sin(2*theta)`
across `theta in [0, pi)` — the positive hump for `theta in [0, pi/2)` and the
negative hump for `theta in [pi/2, pi)` come out of `sin(2*theta)` on their
own, with no separate sign logic needed — and is silent for `theta in [pi, 2*pi)`.

```
opl_wave_ws4[i] = (i < N/2) ? sin(2*theta) : 0
```

### WS5 (index 5) — rectified double-frequency sine, first half-cycle only

Source: `OPL3_EnvelopeCalcSin5`, lines 313-330. Identical addressing to
`Sin4` (same double-rate `logsinrom` lookup, same silence on `phase >= 512`)
but with no sign-flip logic at all — always positive. Two positive humps
compressed into the cycle's first half, silent second half:

```
opl_wave_ws5[i] = (i < N/2) ? fabs(sin(2*theta)) : 0
```

### WS6 (index 6) — plain square wave

Source: `OPL3_EnvelopeCalcSin6`, lines 332-341. No `logsinrom` lookup at
all — `out` is a phase-*independent* constant (`envelope << 3`), i.e. no
per-sample waveform shaping whatsoever; only the `phase & 0x200` bit flips
sign. This is a pure square wave, full scale for the first half-cycle,
inverted full scale for the second:

```
opl_wave_ws6[i] = (i < N/2) ? +32767 : -32767
```

### WS7 (index 7) — logarithmic/exponential sawtooth

Source: `OPL3_EnvelopeCalcSin7`, lines 343-355. This one does **not** go
through `logsinrom` — `out` is instead a direct linear ramp in the log
domain, `phase << 3`, mirrored and sign-flipped for the second half
(`phase = (phase & 0x1ff) ^ 0x1ff` when bit `0x200` is set). Feeding a
*linear* ramp through the chip's log→linear exponential converter
(confirmed above: `EnvelopeCalcExp(level) ∝ 2^(-level/256)`) produces an
*exponential* amplitude curve, not a linear one. Substituting `level =
phase*8` and normalizing to `p = phase/N` (0..1, one full cycle):

- `p` in `[0, 0.5)` (first half-cycle): amplitude `∝ 2^(-32*p)` — starts at
  full scale at `p=0`, decays smoothly (~96 dB, i.e. to the noise floor) by
  `p → 0.5`.
- `p` in `[0.5, 1)` (second half-cycle, mirrored and negated): amplitude
  `∝ -2^(-32*(1-p))` — mirror image, negative polarity, rising back from
  near-zero toward full negative scale as `p → 1`.

The curve is continuous (both sides approach ~0) across the `p=0.5`
midpoint, but has a **hard discontinuity at the phase wraparound**
(`p → 1` snapping to `p = 0`): the sample jumps from near-full-scale
negative straight to full-scale positive. That snap, plus the exponential
(not linear) decay shape within each half-cycle, is what gives this
waveform its "camel/log sawtooth" character and sound (a sharp attack-like
transient each half-cycle, not a smooth ramp).

```
opl_wave_ws7[i] = (i < N/2)
    ? +32767 * pow(2.0, -32.0 * (i / (float)N))
    : -32767 * pow(2.0, -32.0 * (1.0 - i / (float)N))
```

(`pow(2.0, x)` is equivalent to `exp2f(x)` in `<cmath>`, matching the
`sinf`/`fabsf` style already used elsewhere in `waveforms.h`.)

## Flag: WS7 is where the "approximation is fine" precedent needs a caveat

For WS0-3 (already implemented) and WS4-6, `logsinrom` + `EnvelopeCalcExp`
are purely an implementation trick for computing an ordinary sine or a flat
constant cheaply in fixed point — the *geometry* those functions trace is
just sine/square curves, which this module's existing float-`sinf` approach
already captures faithfully per its own stated precedent (approximate
geometry, not exact log-ROM values).

**WS7 is different**: there the log→exponential conversion is not a trick
for computing something else — the exponential decay curve *is* the
waveform's actual, audible geometry. A naive linear sawtooth
(`value = 2*p - 1`) would not be a reasonable approximation of this shape;
it would sound and look qualitatively different (a smooth linear ramp
instead of a sharp exponential "zap" every half-cycle). If/when this module
implements WS7, it should use the exponential formula above (or something
visually/aurally equivalent — e.g. any monotonically-shaped decay with
~90+ dB of range per half-cycle) rather than a linear ramp, or the
approximation-only approach that holds fine for the other 7 shapes will
misrepresent real hardware in a way a listener would likely notice.

## Cross-check against secondary documentation

The shapes derived above (WS4 = signed double-frequency sine restricted to
the first half-cycle, WS5 = its rectified/always-positive counterpart, WS6 =
plain square, WS7 = per-half-cycle exponential/logarithmic sawtooth) match
the widely-cited community descriptions of OPL3's register-4-7 waveforms
(e.g. as summarized in Adlib/OPL3 programming references and other
open-source OPL3 cores' waveform tables), which corroborates the register
index mapping (`ws=4..7` in this order) and shape identification derived
directly from Nuked-OPL3 above. No gaps were found requiring secondary
sources to resolve — the primary source (Nuked-OPL3) was unambiguous for all
four shapes.

## Answer, restated concretely

Extending `waveforms.h`'s existing `OPL_TABLE_SIZE`-length int16 table
pattern (`theta = 2*pi*i/OPL_TABLE_SIZE`, `N = OPL_TABLE_SIZE`, `i` in
`[0, N)`), the 4 new tables needed for `ws` values 4-7:

```c
opl_wave_ws4[i] = (i < N/2) ? (int16_t)(32767.0f * sinf(2.0f * theta)) : 0;

opl_wave_ws5[i] = (i < N/2) ? (int16_t)(32767.0f * fabsf(sinf(2.0f * theta))) : 0;

opl_wave_ws6[i] = (i < N/2) ? 32767 : -32767;

opl_wave_ws7[i] = (i < N/2)
    ? (int16_t)(32767.0f * exp2f(-32.0f * (float)i / (float)N))
    : (int16_t)(-32767.0f * exp2f(-32.0f * (1.0f - (float)i / (float)N)));
```

`opl_waveform_table()`'s switch would extend from `& 3` to `& 7` with 4 new
cases; `OplOpParams::ws`'s comment (`patch.h`) would extend from "0-3" to
"0-7" — noted here for the implementing ticket, not applied by this research
ticket (no source under `src/` is modified here).
