# T00T — Chip Synthesis Module: SID (and friends) (Design & Implementation Plan)

A design document for a **chip-music synthesis** variant of the t00t engine,
built around the MOS 6581/8580 SID and structured to admit other 8-bit sound
chips (AY-3-8910, SN76489, NES 2A03, GB DMG) later without rework.

It is **not** a SID player and **not** a `.sid` file emulator. It is a synth whose
voices sound like SID voices and whose instruments are expressive in the way SID
instruments were expressive — via a per-frame table VM, not via LFOs.

Status: **P0 closed, P1/P2/P3 built.** P0's CPU budget in §9 is real
breadboard_rp2350 numbers, not static estimates (§14a); target configuration
settled at **20 voices, 4 filter buses**. P1's engine skeleton is in
`engines/chip/`, and both halves of §11.1's validation (the `CHIP_STRICT`
spectral harness and the exact control-plane diff, both missing until now
despite §14a's earlier claim otherwise) are built and passing — see §14b. P2
adds the filter buses themselves: binding (§5.2), degrade-to-unfiltered, and
the per-bus idle skip §5.2 flagged as a P2 TODO — see §14c. P3 adds the frame
table VM (§6) — wave/pulse/filter tables, vibrato, gate-off timer — and
resolves open question 1 — see §14d. **None of P1/P2/P3 have been
re-measured or listened to on real hardware yet**; every number since §14a is
still the last hardware-verified state.

---

## 1. Scope & phasing

| Phase | Deliverable |
|-------|-------------|
| **P0** | **Measurement gate.** Primitives built standalone + host harness; measure per-voice cost, filtered-voice cost *including bus round trip*, and sync at 1× vs 2× oversampling. No engine, no VM. *(Closed — §14a. Hardware measured on breadboard_rp2350; 20 voices / 4 buses confirmed at 86.6% worst-case load.)* |
| **P1** | Engine skeleton: `engines/chip/`, `VoiceType` dispatch, static MIDI-channel→voice map, register-stream playback path. Prove a SID voice sounds right against reSID. *(Built — §14b. Device engine, `CHIP_STRICT` spectral harness, and the exact control-plane diff (`sid_ctl_diff.py`) all in and passing.)* |
| **P2** | Filter buses + 6581 model (cutoff LUT + saturation). Bus binding, degrade-to-unfiltered. *(Built — §14c.)* |
| **P3** | Frame table VM on Core 1: wave/pulse/filter tables, vibrato, arpeggio, hard restart, gate-off timer. This is where instruments become expressive. *(Built — §14d. Not yet heard on hardware.)* |
| **P4** | Instrument import: GoatTracker `.ins` host converter; hand-authored text format → header. Dynamic voice allocation. *(Built — §14e. Not yet heard on hardware.)* |
| **P5** | Speaker simulation output stage (§10). LCD UI. |
| **P6** | 8580 model (table swap). Combined-waveform LUTs. |
| **later** | Other chips (§12): AY/YM2149, SN76489, NES 2A03, GB DMG. |

**P0 was a hard gate.** Nothing in §9's budget was trusted until it cleared, and
`FILTER_BUS_COUNT` was provisional until then (`MAX_VOICES = 32` is a separate,
already-settled decision — §13.3's allocation-pool/bitmap width, not the
concurrent-voice CPU budget). This followed the FM and speech modules'
precedent, and speech's #31 result was the reason to expect trouble: its
prediction ran **25–55% low** against measurement. F0's own static estimate
had the same problem, worse — see §14a.

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

**Skip idle buses individually — done, §14c.** §14a.9 measured a bound-but-
silent filter bus at ~80 c/f/*sample*, not per note — real cost even when
nothing feeds it. `audio_engine.cpp`'s per-bus voice count (derived from the
same per-sub-block voice scan the render loop already does, not a separate
Core-0-side counter) gates §7.2's per-bus clear+tick so each of the 4 buses
skips independently when its count is 0, rather than all-or-nothing.
`SidFilter`'s `lp`/`bp` state resets (`init()`) the moment a given bus's
count goes 0→nonzero, so a new voice binding to a just-woken bus doesn't
inherit one sample of the *previous* voice's resonant tail. Doesn't lower the
worst-case budget (86.6% already assumes all 4 buses bound) — it's
average-case headroom, for whenever a performance isn't using all 4 at once.

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
  independent decision made at note-on. Built — §14e.

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

**These are real breadboard_rp2350 measurements (§14a), not estimates.** The
original static estimates are struck through for the record; every one of them
was wrong, in both directions, by up to 3.7×. §14a.9 has the two bugs that
caused the worst of it — one in this rig, one in `src/chip/`.

| Item | ~~Est. c/f~~ Measured c/f | % |
|---|---|---|
| SID voice (acc + waveform + `EnvSID` + sub-osc + accumulate) | ~~45–65~~ **~108** | ~~1.3–1.9%~~ **3.2%** |
| Filter bus (SVF + LUT + saturation + mix), post-fix | ~~50–75~~ **~80** | ~~1.5–2.2%~~ **2.4%** |
| Bus round trip, ~12 filtered voices | ~~40–50~~ **negligible** — filtered and unfiltered voice cost measured indistinguishable | ~~1.4%~~ **~0%** |
| Frame VM, 20 voices @ 50 Hz | 7 (still an estimate — P3 not built) | 0.2% |
| Speaker simulation, mono (§10) | ~~55–75~~ **~75** — F0's stand-in landed inside the original estimate | ~~1.6–2.2%~~ **2.2%** |
| Delay insert (`fx/delay.h`) | **~41** | **1.2%** |
| Reverb insert (`fx/reverb.h`), the expensive one | **~255** | **7.5%** — matches speech's own ~8% almost exactly, independent cross-check |

**Target configuration — 20 voices, 4 buses, speaker stage, reverb (worst case):**

| Config | Voices+buses only | + delay + speaker | **+ reverb + speaker (worst case)** |
|---|---|---|---|
| **20v / 4 buses (chosen)** | — | — | **86.6%** (measured) |
| 22v / 2 buses (rejected) | — | — | **89.0%** (measured) |

20v/4f was chosen over 22v/2f on two independent grounds, not one: it has the
more realistic voice-to-filter ratio, *and* it measures more headroom, not
less. A bus (~80 c/f) is cheaper than a voice (~108 c/f), so trading 2 buses
for 2 voices is a bad exchange rate — 22v/2f spends the bus savings back on
voices at a loss. Frame VM (~0.2%, P3, not built) isn't in either number; at
13.4pp / 11.0pp headroom that's noise, not a risk, unlike the 24v/4-bus
config this replaced (99.9%, effectively zero margin before the frame VM was
even added — see §14a.9).

**Comparison for context:** a PSG voice (AY/SN/NES, est. 10–25 c/f, 0.3–0.7%) is
roughly a third of a SID voice's *estimated* cost — now that the real SID voice
number is ~108 c/f, PSG voices are a smaller fraction still. SID remains the
most expensive chip in the family; later chips are strictly easier to fit.

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
2. **GoatTracker `.ins`** — single-instrument format, large community corpus.
   Host converter → header, same pattern as `xm2t00t` and the planned `.syx`
   converter. Built — §14e.3. Turned out not to be "documented" in any
   byte-level sense anywhere in GoatTracker's own repo or the community docs
   found by search; the real spec is the save/load code itself.
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
3. **Filter buses, typed, pooled** — `MAX_VOICES = 32` (allocation pool),
   `FILTER_BUS_COUNT = 4` confirmed by P0 (§9, §14a.9): the CPU-budget target of
   20 concurrently-sounding voices affords 4 buses with real headroom, and a
   fifth bus is a worse trade than the voices it would cost (§9's exchange-rate
   finding). Degrade to unfiltered on exhaustion.
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
(fm2.md §5.3: "−55 dBc under heavy modulation is acceptable on a platform whose
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
than inventing one is the direct application of fm2.md §1.1(a): attempt 1 of the
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
              gated (sid.md §13.5) -- mean |err| 1012/1423/1971/1020,
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
   and "grainy." Fixed by repeating each note's row 6x (120 ms/note,
   480 ms/cycle, ~2.1 Hz) -- the standard tracker convention for "hold" in a
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

**Hardware-observed bug (Carl, first P4 by-ear/by-scope pass): CPU duty
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

Verified three ways: (1) both real `.ins` files parse their header and all
three tables exactly, and are correctly refused for the absolute-pitch
reason above; (2) a hand-built synthetic `GTI5` file exercising the full
positive path -- wavetable delay-rows folded into repeat counts, a loop
jump, pulse absolute-init + signed delta/duration sweep + loop, filter
type/resonance + cutoff + delta/duration sweep -- round-trips through
`chipgen.py` to field values matching the hand-authored `ARP_LEAD`/
`FILTER_PAD` shape exactly; (3) three deliberate negative constructs
(a WAVECMD row, a delay row with nothing preceding it, a filter table
missing its required cutoff row) all produce clear, correctly-attributed
errors and non-zero exit.

**Honestly still open:** neither real example file available for testing
was a melodic/tonal instrument using only relative-note rows -- both were
absolute-pitch SFX, which is the one case the converter is built to refuse.
The positive path is verified against a hand-built file matching the
documented byte layout, not against a real relative-note `.ins` pulled from
the wild. Worth running against a real melodic patch (e.g. from a `.sng`'s
instrument table, if one can be isolated) before trusting the converter on
an arbitrary user-supplied file.

---

## 15. Open questions

1. ~~**Sub-block size vs. frame tick.** Speech cuts sub-blocks per voice; the
   frame tick is global here. Does the VM tick get its own cut point, or ride
   the existing `SUBBLOCK` boundary with ≤1.45 ms of quantisation? (1.45 ms on
   a 20 ms frame is 7% — likely fine, unlike Core 0's 29%. Confirm at P3.)~~
   **Resolved (§14d): rides `CHIP_SUBBLOCK`.** Built as the doc's own lean
   predicted -- no separate sample-exact cut point. Not yet confirmed by ear.
2. **Multispeed default.** 50 Hz PAL only at v1, or 2× from the start? Affects the
   instrument data format, so it wants deciding before assets are generated.
3. **Telemetry struct contents** for the LCD — how much VM state does the display
   actually want, and does it justify widening the reverse channel beyond the
   bitmap?
4. **Speaker stage placement vs. master FX** — confirmed downstream of the insert,
   but does it sit before or after the final `__ssat` clip?
5. ~~**`FILTER_BUS_COUNT` after P0** — 4 is a guess. If voices measure cheap, is 6
   better spent than 6 more voices?~~ **Resolved (§9, §14a.9): no.** Voices
   measured *more* expensive than buses (~108 vs ~80 c/f), the opposite of what
   this question assumed — a bus is the cheaper thing to add, not the pricier
   one. 4 buses / 20 voices confirmed on hardware with real headroom (86.6%
   worst case); trading buses for voices (22v/2f) measured *worse*, not
   better (89.0%).

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
