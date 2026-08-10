# T00T — Chip Synthesis Module: SID (and friends) (Design & Implementation Plan)

A design document for a **chip-music synthesis** variant of the t00t engine,
built around the MOS 6581/8580 SID and structured to admit other 8-bit sound
chips (AY-3-8910, SN76489, NES 2A03, GB DMG) later without rework.

It is **not** a SID player and **not** a `.sid` file emulator. It is a synth whose
voices sound like SID voices and whose instruments are expressive in the way SID
instruments were expressive — via a per-frame table VM, not via LFOs.

Status: **design draft.** No code written yet. All CPU numbers are static
estimates derived from the measured figures in `engine.md`; see §9 for the
explicit caveat and §1 for the P0 gate that must clear before any of it is
trusted.

---

## 1. Scope & phasing

| Phase | Deliverable |
|-------|-------------|
| **P0** | **Measurement gate.** Primitives built standalone + host harness; measure per-voice cost, filtered-voice cost *including bus round trip*, and sync at 1× vs 2× oversampling. No engine, no VM. |
| **P1** | Engine skeleton: `engines/chip/`, `VoiceType` dispatch, static MIDI-channel→voice map, register-stream playback path. Prove a SID voice sounds right against reSID. |
| **P2** | Filter buses + 6581 model (cutoff LUT + saturation). Bus binding, degrade-to-unfiltered. |
| **P3** | Frame table VM on Core 1: wave/pulse/filter tables, vibrato, arpeggio, hard restart, gate-off timer. This is where instruments become expressive. |
| **P4** | Instrument import: GoatTracker `.ins` host converter; hand-authored text format → header. Dynamic voice allocation. |
| **P5** | Speaker simulation output stage (§10). LCD UI. |
| **P6** | 8580 model (table swap). Combined-waveform LUTs. |
| **later** | Other chips (§12): AY/YM2149, SN76489, NES 2A03, GB DMG. |

**P0 is a hard gate.** Nothing in §9's budget is trusted until it clears, and
`MAX_VOICES` / `FILTER_BUS_COUNT` are provisional until then. This follows the
FM and speech modules' precedent, and speech's #31 result is the reason: its
prediction ran **25–55% low** against measurement.

---

## 2. What we already have (reuse inventory)

| Existing component | File | Reused for |
|--------------------|------|------------|
| SVF multimode, fixed-point | `filter.h` | **The SID filter *is* a two-integrator-loop SVF.** Direct structural reuse; needs a mode *mask* and SID cutoff/resonance mapping (§5) |
| Two-pole resonator | `res2p.h` | Speaker simulation resonant hump (§10) |
| LFSR noise | `osc/noise.h` | Pattern only — SID needs its own 23-bit LFSR with different taps and bit scatter |
| ParamExchange double-buffer IPC | `engine_base.h` | Unchanged mechanism; new `VoiceParams` payload + new `FilterBusParams` sibling (§7) |
| Dynamic voice allocator | `voice_alloc.*` | Reused as-is at P4 — flat pool, no grouping (§8) |
| Active-voice bitmap (1 × `uint32_t`) | `engine_base.h` | Unchanged. `MAX_VOICES = 32` fits exactly |
| Per-voice `VoiceType` dispatch | `engines/groovebox/` | **The architectural template for this whole module** (§7) |
| Sub-block render loop | `engines/speech/` | Per-voice sub-block cut point; frame VM tick boundary |
| Delay + Freeverb FX | `fx/` | Master FX, unchanged (upstream of the speaker stage) |
| MIDI transports + controller | `midi/` | Note/velocity/CC → voice triggers |
| Host render harness | `tools/host_render/` | `render_sid.cpp` + reSID diff, mirroring `render_res2p.cpp` / `diff_xm.py` |

**Key insight:** once routing limitations are discarded (§3), this module's
architecture is **exactly the groovebox's** — heterogeneous voices, per-voice type
dispatch, flat voice pool, one shared render pass. No chip-container abstraction
exists anywhere in the firmware. `groovebox.md` §5.2 already calls this pattern
"contained Option B, *within* this engine", and it is built and working.

---

## 3. What "chip emulation" means here

`readme.md` states the project strategy as approximations *"based on the general
principles of working, not the actual implementation."* This module takes that
literally, with a sharper rule:

> **A limitation is *tonal* if it operates inside the signal path. It is
> *structural* if it operates on routing or allocation. Discard structural
> limitations freely. Signal-path limitations *are* the sound — keep them.**

| Limitation | Kind | Verdict |
|---|---|---|
| One filter per 3 voices | structural | **discard** — filter buses, §5 |
| Sync/ring wired to the neighbouring voice | structural | **discard** — per-voice sub-oscillator, §4.4 |
| Exactly 3 voices per chip | structural | **discard** — flat pool of 32 |
| SN76489 noise period slaved to ch3 | structural | discard (later) |
| AY: one envelope generator per chip | structural | discard (later) |
| SID 16-bit frequency register quantisation | signal-path | **keep** — vibrato steppiness is character |
| 4-bit master volume / 8-bit envelope resolution | signal-path | **keep** |
| Frame-quantised control at 50 Hz | signal-path | **keep** — this is the whole character |
| 6581 nonlinear cutoff + filter saturation | signal-path | **keep** |
| Summing-node intermodulation through the filter | signal-path | **keep, as a capability** (§5) |
| NES triangle 4-bit / 32-step, DMG 4-bit wave RAM | signal-path | **keep** (later) |

Authentic chip behaviour remains reachable by simply not using illegal parameter
combinations. The strict host harness (§11) enforces it where it matters.

**Consequence for the tonal side.** Because voices sharing a filter bus sum
*before* the 6581 nonlinearity, a chord routed to one bus intermodulates the way
real hardware did, while voices on separate buses do not. That tonal property is
preserved as an option rather than imposed as a constraint — which is the whole
thesis of this section in miniature.

---

## 4. Voice model and render primitives

All primitives are **topology-free**: they know nothing about chips, buses,
allocation or grouping. This is a hard rule from the first commit, because the
strict harness (§11) only buys anything if the same code runs in both modes. It
is cheap now and painful to retrofit — the `res2p.h` lesson.

### 4.1 Accumulator (`chip/sid_osc.h`)

SID's accumulator is 24-bit clocked at ~1 MHz. Represent it as **Q24.8 in a
`uint32_t`** — the 24-bit accumulator shifted left 8:

```cpp
acc += inc;                       // inc = freq_reg * 5805  (44.1 kHz, PAL)
uint32_t top12 = acc >> 20;       // the 12 bits SID's waveform logic sees
```

Three properties fall out for free:

- **Hard sync is plain `uint32_t` wrap detection** — carry out of bit 31 *is* the
  24-bit accumulator overflow.
- The 16-bit frequency register stays the control-plane unit, so authentic pitch
  and vibrato quantisation is inherited at zero cost.
- Noise LFSR clocking keys off accumulator bit 19 → bit 27 here.

### 4.2 Waveforms

| Waveform | Derivation |
|---|---|
| Sawtooth | `top12` |
| Triangle | `top12` XOR-folded about the MSB (MSB also the ring-mod input) |
| Pulse | `top12 >= pulse_width ? 0xFFF : 0` |
| Noise | 23-bit LFSR, taps 22/20/16/13/11/7/4/2, scattered into output bits 11–4 |

**Noise needs a clock count per sample**, not a single clock: at high frequencies
more than one bit-19 transition occurs per 44.1 kHz sample. Compute the count
from the accumulator delta and cap it. Cheap, but not free — budget for it.

**Combined waveforms** are a nonlinear analog artifact, not a bitwise AND — but
AND gets to roughly TinySID grade and is one instruction. **Start with AND (P1);
LUTs at P6** (5 useful combinations × 4096 entries × 8 bit ≈ 20 KB flash, opt-in
per build flag). Real tunes lean on combined waveforms constantly, so this is a
deferral, not a cancellation.

### 4.3 Envelope (`chip/env_sid.h`)

SID's envelope is a rate-counter design with a piecewise-exponential decay
(`/2, /4, /8, /16, /30` segments). That shape is a large part of the character and
is **not** substitutable with the existing `EnvConfig` ADSR — this module needs a
dedicated `EnvSID` type, for the same reason the FM module needs `EnvDX`.

At 44.1 kHz the per-cycle rate counter is replaced by precomputed per-sample
increments that replicate the segment shape. The 4-bit ADSR nibbles and their
rate tables are preserved verbatim as the control-plane interface.

**Hard restart.** On hardware this exists to dodge the 6581 ADSR delay bug and
costs 1–2 frames (20–40 ms) of pre-gate — a direct hit to this project's latency
priority. Since we are not bit-exact: **implement hard restart as an
instantaneous envelope reset (zero latency) by default.** The frame-delayed form
is only needed if the ADSR bug is also modelled, which is not planned. Imported
GoatTracker instruments carrying a hard-restart ADSR value simply get the fast
path.

### 4.4 Modulation sub-oscillator

Ring mod uses the source **accumulator MSB**; hard sync uses the source
**accumulator overflow**. Neither uses the source's waveform, envelope or filter.
So a modulator is an accumulator and a frequency — roughly 5–8 c/f, versus 45–65
for a whole voice.

Therefore modulation is a **per-voice field, not an allocated voice**:

```cpp
uint32_t mod_acc;    // sub-oscillator accumulator (Core 1 state)
uint32_t mod_inc;    // its frequency
uint8_t  mod_mode;   // MOD_OFF / MOD_RING / MOD_SYNC
```

Cheaper, no allocation involved, and strictly *more* capable than hardware —
every voice can sync or ring independently, including all 32 at once.

Two consequences:

- **The frame VM must be able to target `mod_inc`.** Real tunes sweep the sync
  source with an arp table; that is where the classic sync-lead squeal comes from.
  Omit this and sync instruments sound static in a way easily misdiagnosed as a
  synthesis bug.
- **Chained sync (A→B→C) is given up.** Rare enough to defer; the escape hatch is
  to let `mod_mode` optionally name another voice index. Not built at v1.

This also removes `channels_needed` from the instrument format entirely — **every
note occupies exactly one voice.**

---

## 5. Filter buses

Voices carry a bus index; buses are a small typed pool. Not a chip-fidelity
constraint — a **CPU budget guard**, so worst-case load is bounded by construction
rather than being a function of what the player happens to play (see §9).

```cpp
enum FilterModel : uint8_t { FB_OFF, FB_6581, FB_8580, FB_SVF };

struct FilterBusParams {
    uint8_t  model;
    uint8_t  mode_mask;   // LP | BP | HP, summed — SID mode-register semantics
    uint16_t cutoff;      // raw register units, mapped by the model's LUT
    uint16_t resonance;
};
```

`VoiceParams` carries only `uint8_t filter_bus`, with `BUS_NONE` as sentinel.

**Because buses are typed, cross-chip routing falls out for free** — an NES
triangle through a 6581 filter, or an AY square through SID resonance. Sounds that
never existed and are period-plausible. Squarely "capabilities, not limitations."

### 5.1 Changes to `filter.h`

Three, all of which backport usefully to the subtractive engine:

1. `FilterMode` enum → **3-bit mask**; SID sums LP+BP+HP simultaneously.
2. **Pluggable cutoff LUT** instead of `svf_compute_f_half`'s linear map, so 6581
   vs 8580 is a table swap rather than a code fork.
3. **Optional saturation** in the feedback path (6581 only).

**The 6581 cutoff curve varied enormously between physical chips.** Any curve is
*a* 6581, not *the* 6581. The LUT must be documented as sampled from a named
reference (reSID's, or a specific chip), never presented as canonical.

### 5.2 Binding policy

```
bind_filter(instrument):
    1. bus already owned by this instrument   -> share it
    2. any free bus                           -> bind it
    3. none free                              -> BUS_NONE, render unfiltered
```

Rule 1 means a chord of one filtered instrument sums into one bus and pays for
**one** filter — cheaper *and* the source of the §3 intermodulation property.
Rule 3 is graceful degradation that also happens to be period-correct: most voices
in real tunes ran unfiltered, because the filter was scarce.

**Last-write-wins** when several notes share a bus and their filter tables
disagree. That is what real drivers did and it is the trivial implementation; no
bus-leadership tracking.

---

## 6. The frame table VM — where expressivity comes from

There was never one canonical SID instrument format. But the *editors* converged
independently — GoatTracker, SID-Wizard, CheeseCutter and the classic hand-rolled
drivers all land on the same model, and so do FamiTracker, Vortex Tracker, LSDJ
and Furnace for other chips. The convergence is empirical, not theoretical.

An instrument is **ADSR + vibrato + three per-frame tables**:

| Table | Row content | Provides |
|---|---|---|
| **Wave** | `(waveform-or-command, note abs/rel)` | Arpeggios, waveform sequences, sync/ring toggles, `mod_inc` sweeps, the one-frame noise attack transient |
| **Pulse** | `(delta, duration)` segments | PWM sweeps — the iconic phasing |
| **Filter** | `(delta, duration)` + cutoff init, resonance, mode, bus | Filter sweeps |

Plus: ADSR nibbles, hard-restart value, vibrato (depth/speed/delay), gate-off
timer, first-frame waveform. ~30 bytes per instrument plus shared table rows.

**This is a superset of "LFO + arpeggiator + envelope", not a subset.** A per-frame
table is an arbitrary function of time, so it can be any of those — or a one-shot
transient, which an LFO cannot be. And the 50 Hz steppiness is not a limitation to
smooth over: a continuous LFO in place of a wave table *sounds wrong*. The
quantisation is the sound.

### 6.1 Placement: Core 1, inside the render pass

Following the speech precedent, not the tracker's. Per voice per frame the VM does
~250 cycles of table stepping; at 50 Hz over 24 voices that amortises to **6.8 c/f
(0.20%)**, and the synchronous burst is 6,000 cycles against a ~871,000-cycle
buffer budget — 0.7% of a buffer, every ~3.4 buffers. Not a deadline risk.

The cost is a wash either way. Core 1 wins on **timing precision**, in two distinct
ways:

- **Multispeed above 2× breaks on Core 0.** Core 1 reads `ParamExchange` at 172 Hz
  (per buffer). At 200 Hz frames, Core 0 writes faster than Core 1 reads and
  latest-wins silently drops frames — the one-frame noise transient vanishes
  intermittently. Fixing that on Core 0 means adopting the tracker's ordered ring.
- **Frame boundaries quantise to buffer edges even at 50 Hz.** Up to 5.8 ms of
  jitter on a 20 ms frame period — **29%**. Audible on one-frame clicks, 3-frame
  arpeggios and hard-restart timing. On Core 1, driven by a sample counter, frame
  boundaries are exact.

Nothing significant is lost. The XIP-thrash argument that pushed the XM player to
Core 0 does not apply — instrument tables are small enough to live permanently in
SRAM. The only real cost is observability: the display needs VM state (current
table row, active instrument), so a small telemetry struct joins the reverse
channel. Same shape as speech's per-voice phoneme display.

**Reopen if P0 measures the VM materially above ~250 cycles/voice/frame** — e.g.
if filter-table segment logic turns out branch-heavy. Measure the VM tick in
isolation, the way #31 isolated speech's coefficient recompute.

### 6.2 Frame rate

Instrument table timing is **rate-specific** — a 1-frame noise click at 50 Hz and
at 200 Hz are different sounds. The frame rate is therefore a module-global
constant (default 50 Hz PAL), and instrument data must declare the rate it was
authored for. Multispeed (2×/3×/4×) is a build/preset choice, not a per-instrument
one.

---

## 7. Architecture integration

### 7.1 Param block

`FilterBusParams` is neither per-voice nor global, so it becomes a new sibling in
the param block:

```cpp
template <typename VoiceParams>
struct VoiceParamBlockT {
    VoiceParams     voices[MAX_VOICES];
    FilterBusParams bus[FILTER_BUS_COUNT];   // new
    EffectParams    fx;
};
```

This is a change to `engine_base.h` shared by all engines. Engines that do not use
buses set `FILTER_BUS_COUNT = 0`.

`ParamExchange` keeps **latest-wins** semantics — correct here, because the VM
lives on Core 1 and Core 0 only supplies note/CC/instrument-select. No ordered ring.

### 7.2 Two-phase render pass

The one structural change to Core 1, and it is not how any existing engine is
shaped:

```
per sub-block:
    clear bus accumulators                       // FILTER_BUS_COUNT × SUBBLOCK
    for each active voice:
        tick frame VM if a frame boundary falls in this sub-block
        render → bus[v.filter_bus], or → dry mix directly if BUS_NONE
    for each bound bus:
        filter its accumulator, sum into dry mix
```

Accumulators are `FILTER_BUS_COUNT × SUBBLOCK × int32` — 4 × 64 × 4 = **1 KB**, not
buffer-sized, which is why the sub-block cut point matters here.

Cost is one extra store-and-load per *filtered* voice. Unfiltered voices skip the
round trip entirely and go straight to the mix, keeping the common case at full
speed. **P0 must measure filtered-voice cost with the round trip included**, not
without it.

### 7.3 Chip descriptors

"Chip" survives, but only as a **tonal profile** — a per-voice type tag selecting
oscillator/envelope/LFSR/filter models, dispatched in the render loop exactly like
the groovebox's `VoiceType`. It is not a container, not an allocation unit, and
does not appear in the voice topology.

```cpp
enum VoiceType : uint8_t {
    VT_SILENT = 0,
    VT_SID,          // 6581/8580 voice
    // later: VT_AY, VT_SN76489, VT_NES_PULSE, VT_NES_TRI, VT_NES_NOISE, VT_GB_WAVE
};
```

Per-chip *shared* resources that survive the structural cull (AY's single envelope
generator, SN76489's slaved noise period) are handled by the same
small-typed-pool mechanism as filter buses, so no second abstraction is needed.

---

## 8. Voice allocation

Flat, unchanged, `voice_alloc.*` reused as-is. Because §4.4 made every note exactly
one voice, none of the group-allocation machinery that a chip-container model would
have required exists.

- **P1: static assignment.** MIDI channel → voice, no allocator. Follows the
  groovebox. Enough for register-stream bring-up, and defers every allocation
  question past the point where P0 might change the voice count anyway.
- **P4: dynamic allocation.** Existing three-tier steal policy (silent → released →
  oldest active) applies unmodified. Filter-bus binding (§5.2) is a separate,
  independent decision made at note-on.

`MAX_VOICES = 32` — exactly fills the existing `uint32_t` bitmap, so the FIFO
feedback path is unchanged. >32 was considered and rejected: it is only reachable
for the cheapest PSG voices, and CPU is better spent on the speaker stage (§10).

**If >32 is ever wanted**, the FIFO-safe approach is a tag bit in the MSB marking
msw/lsw, so each word is self-identifying and a dropped word degrades to "one half
is a pass stale" rather than permanent lo/hi inversion. Natural boundaries become
31 and 62; `if constexpr (MAX_VOICES <= 32)` keeps today's untagged single-word
format at zero cost. Recorded here as the answer; not built.

---

## 9. CPU budget

Baseline anchors from `engine.md`: 3401 cycles/frame = 100% at 150 MHz / 44.1 kHz.
Measured references — subtractive voice **5.9%**, speech voice **93.5 c/f
(2.75%)**, reverb **~8%**, idle **~0.6%**.

| Item | Est. c/f | % |
|---|---|---|
| SID voice (acc + waveform + `EnvSID` + sub-osc + accumulate) | 45–65 | 1.3–1.9% |
| Filter bus (SVF + LUT + saturation + mix) | 50–75 | 1.5–2.2% |
| Bus round trip, ~12 filtered voices | 40–50 | 1.4% |
| Frame VM, 24 voices @ 50 Hz | 7 | 0.2% |
| Speaker simulation, mono (§10) | 55–75 | 1.6–2.2% |

**Target configuration — 24 voices, 4 buses, speaker stage:**

| Line | c/f | % |
|---|---|---|
| 24 voices | 1080–1560 | 32–46% |
| 4 buses | 200–300 | 6–9% |
| Bus round trip | 40–50 | 1.4% |
| Frame VM | 7 | 0.2% |
| Speaker sim | 55–75 | 1.6–2.2% |
| **Total** | | **41–59%** |

> **Do not trust these numbers.** They are static instruction-count estimates.
> `speech.md`'s equivalent table ran **25–55% low** against #31's measurement, and
> the gap lived entirely in unbudgeted per-sample glue. Bias-adjusted, the real
> range for the target configuration is roughly **51–91%** — plausible but not
> banked. P0 decides.

**Watch item:** if P0 says hard sync needs 2× oversampling, the cost lands on the
largest line. **Cut order if the budget does not close:** reduce voices first —
24 → 18 reclaims 8–11%, while the speaker stage is only ~2% and is a higher
priority per cycle than a fifth filter bus. Filter bus count is the second lever.

**Comparison for context:** a PSG voice (AY/SN/NES, est. 10–25 c/f, 0.3–0.7%) is
roughly a third of a SID voice. SID is the *most* expensive chip in the family,
not the least — later chips are strictly easier to fit.

---

## 10. Speaker simulation output stage

Period-correct playback was through a TV or monitor speaker, and that colouration
is a large part of what people remember. Prioritised **ahead of** any increase in
filter bus count.

| Stage | Est. c/f | Built from |
|---|---|---|
| HP ~150–300 Hz (cone rolloff) | 15–20 | one-pole |
| Resonant peak ~400–800 Hz (the "boxy" hump) | 15–20 | `res2p.h` |
| LP ~5–9 kHz | 15–20 | one-pole |
| Soft clip / cone breakup | 8–12 | saturation |
| **Total, mono** | **55–75** | **1.6–2.2%** |

Cheaper than one subtractive voice, and about a quarter of what reverb costs.

Three notes on shape:

- **It is an output stage, not an `EffectType`.** The current insert is mutually
  exclusive by CC74 band (delay *or* reverb *or* off). The speaker sim is
  *downstream* of the insert — you want delay → speaker, or reverb → speaker. Own
  params, own slot after the insert.
- **Mono is more authentic and half the price.** Sum → speaker → duplicate L/R.
  Offer mono/stereo as a flag rather than assuming stereo.
- **Free bonus:** the C64 board's own passive output network before the AV
  connector is a one-pole low-pass, and is genuinely part of "the SID sound".
  Applies *before* the speaker stage, near-zero cost.

Presets worth having: Commodore 1702 monitor, portable TV, Game Boy, arcade
cabinet, bypass.

**Backports well beyond this module:** 808 through a boombox, Amiga through a TV,
and especially the speech engine — a toy-speaker curve in front of the Das Boot
bitcrusher does more for that character than further bit reduction would.

---

## 11. Host tooling & validation

Mirrors `tools/host_render/`'s existing pattern (`render_res2p.cpp`, `diff_xm.py`).

### 11.1 `CHIP_STRICT` — the reference harness

A register stream is `$D400–$D418`: three voices, one shared filter,
adjacency-wired sync. Free-routing mode **cannot consume it**. Without a strict
mode there is no way to prove the oscillator, envelope, LFSR and filter are
correct at all.

The resolution is that **validation targets the primitives, not the routing:**

| | `CHIP_STRICT` | Free mode |
|---|---|---|
| Where | host only, `tools/host_render/render_sid.cpp` | firmware |
| Topology | 3 voices / chip, 1 shared filter, adjacency sync | flat pool, typed buses, per-voice sub-osc |
| Input | register write stream | MIDI + frame VM |
| Validated against | reSID / resid-fp, spectral diff | inherits primitive correctness |
| Performance | irrelevant | budgeted (§9) |

Free mode is the same validated primitives wired differently. This costs almost
nothing if planned for and is painful to retrofit — hence the topology-free rule
in §4.

**Velocity scaling must compile out under `CHIP_STRICT`** (see §13 item 7), or the
reSID diff fails for reasons unrelated to the primitives.

### 11.2 Instrument sources

1. **`siddump`** (Cadaver — the GoatTracker author) dumps per-frame, per-voice
   freq/wave/ADSR/pulse/filter columns from a `.sid`. Its output is *literally in
   the same columns as the table model* — ground truth for how a given classic
   sound was made.
2. **GoatTracker `.ins`** — documented single-instrument format, large community
   corpus. Host converter → header, same pattern as `xm2t00t` and the planned
   `.syx` converter.
3. **Hand-authored text → generator script**, same as `speechgen.py`.

**On `.ins` compatibility:** a `.ins` file contains AD/SR, table pointers, vibrato,
gate-off timer, hard-restart ADSR and first wave — and **no channel reference at
all**. Sync/ring live in wavetable control-register values that say "sync me"
without saying to what, because adjacency made it implicit. So a GoatTracker sync
instrument is *already* not self-contained; its sound depends on whatever the
neighbouring channel's pattern was playing. **The sub-oscillator model (§4.4) makes
such instruments reproducible for the first time.**

What is actually lost: filter-table per-voice routing bits collapse to a bus
selection, and bit-exact reproduction of a specific `.sid` whose sync source was
song-dependent. Both accepted.

---

## 12. Other chips (future)

The control model — 50/60 Hz frame tick driving per-voice tables — is shared across
the whole family; Furnace supports ~50 chips behind one macro system. So these are
additional `VoiceType`s in *this* module, not new modules.

| Chip | Voices | Signal-path traits to keep |
|---|---|---|
| AY-3-8910 / YM2149 | 3 tone + noise | Log volume curve; **envelope generator at audio rate as a "buzzer" bass source** — the signature Spectrum sound, and the AY does not sound like an AY without it |
| SN76489 | 3 tone + 1 noise | 15-bit LFSR; attenuation curve differs from AY's |
| NES 2A03 | 2 pulse + tri + noise | **4-bit, 32-step triangle**; sweep/length units |
| GB DMG | 2 pulse + wave + noise | **32 × 4-bit wave RAM** — a genuine wavetable with a per-frame-rewritable table; opens the door to SCC/VRC6/N163 at near-zero marginal cost |
| POKEY | 4 | Borderline — 4/5/17-bit poly counters share nothing. Add late, if at all |

**Explicitly out of scope:** FM (OPL/OPN) belongs with the DX7 module; Amiga Paula
and other sample chips belong with the tracker; SP0256/TMS5220 belong with speech.

---

## 13. Settled decisions

1. **Signal-path limitations kept, structural limitations discarded** (§3).
2. **Modulation is a per-voice sub-oscillator**, not an allocated voice (§4.4).
   Removes `channels_needed` from the instrument format; every note = one voice.
3. **Filter buses, typed, pooled** — `MAX_VOICES = 32`, `FILTER_BUS_COUNT = 4`
   provisionally. Degrade to unfiltered on exhaustion. Revisit after P0, but the
   speaker stage takes priority over a fifth bus.
4. **Frame VM on Core 1**, sample-accurate frame boundaries (§6.1).
5. **Combined waveforms: AND first (P1), LUTs later (P6).**
6. **6581 first**, LUT-based, so 8580 is a table swap plus a saturation bypass.
   Document the LUT's provenance — any curve is *a* 6581.
7. **Velocity by digital scaling**, applied **before the bus sum**, not after the
   filter — a quiet note then drives the 6581 nonlinearity less hard, which is both
   physically right and what makes velocity feel like dynamics rather than a volume
   knob. ~2–3 c/f. Compiles out under `CHIP_STRICT` (§11.1).
8. **Glossary lands in `architecture.md`**, not here — the voice/channel/track
   collision predates this module (§15).
9. **Primitives are topology-free from the first commit** (§4).
10. **Hard restart is an instantaneous envelope reset**, zero latency (§4.3).
11. **>32 voices rejected**; tag-bit approach recorded but not built (§8).

---

## 14. Recommended build order

1. **P0 measurement rig** — SID oscillator + `EnvSID` + LFSR + 6581 filter as
   standalone primitives; `CHIP_STRICT` host harness; reSID diff. Then on hardware:
   per-voice cost, filtered-voice cost *with* bus round trip, sync 1× vs 2×. **Gate.**
2. **P1 skeleton** — `engines/chip/`, `VoiceType` dispatch, static MIDI map,
   register-stream playback. Proves the synth core before any instrument VM exists
   — the analogue of speech's P1 phoneme keyboard.
3. **P2 buses** — `FilterBusParams`, two-phase render, binding policy, 6581 LUT.
4. **P3 frame VM** — wave/pulse/filter tables, vibrato, arpeggio, hard restart,
   `mod_inc` as a table target. This is where instruments become expressive.
5. **P4 instruments** — `.ins` converter, text format + generator, dynamic
   allocation.
6. **P5 speaker stage** + LCD UI.
7. **P6 polish** — 8580 table, combined-waveform LUTs.
8. **Later** — AY, SN76489, NES, GB DMG as additional `VoiceType`s.

---

## 15. Open questions

1. **Sub-block size vs. frame tick.** Speech cuts sub-blocks per voice; the frame
   tick is global here. Does the VM tick get its own cut point, or ride the
   existing `SUBBLOCK` boundary with ≤1.45 ms of quantisation? (1.45 ms on a 20 ms
   frame is 7% — likely fine, unlike Core 0's 29%. Confirm at P3.)
2. **Multispeed default.** 50 Hz PAL only at v1, or 2× from the start? Affects the
   instrument data format, so it wants deciding before assets are generated.
3. **Telemetry struct contents** for the LCD — how much VM state does the display
   actually want, and does it justify widening the reverse channel beyond the
   bitmap?
4. **Speaker stage placement vs. master FX** — confirmed downstream of the insert,
   but does it sit before or after the final `__ssat` clip?
5. **`FILTER_BUS_COUNT` after P0** — 4 is a guess. If voices measure cheap, is 6
   better spent than 6 more voices?

---

## 16. Glossary (proposed — belongs in `architecture.md`)

Terms have been overloaded across this module's design discussion and the existing
docs. Proposed definitions, grounded in current code usage:

| Term | Meaning | Note |
|---|---|---|
| **Engine** | Build-time synthesis module, `src/engines/<name>/` | One per firmware image; already unambiguous |
| **Voice** | A Core 1 render slot: index `0..MAX_VOICES-1`, one bit in the active bitmap, one `VoiceParams` entry | **Existing meaning in code — do not redefine** |
| **Channel** | MIDI channel, and nothing else | Currently overloaded three ways |
| **Track** | A tracker column | `tracker.md` says "channel N is voice N"; "track" removes the collision |
| **Note** | One played event; occupies exactly one voice | True under §4.4 |
| **Preset** | The patch definition (`VoicePreset`, `presets.h`) | t00t-wide term |
| **Instrument** | Chip-module synonym for preset | Alias; do not rename existing code |
| **Chip** | A *tonal profile* selecting oscillator/envelope/LFSR/filter models | Not a container, not an allocation unit |
| **Filter bus** | A pooled, typed filter instance that voices route into | This module only |
| **Frame** | One tick of the chip control clock (default 50 Hz) | Distinct from "buffer" and from "sample" |
