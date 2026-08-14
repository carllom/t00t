# T00T — Chip Synthesis Module: SID (and friends) (Design & Implementation Plan)

A design document for a **chip-music synthesis** variant of the t00t engine,
built around the MOS 6581/8580 SID and structured to admit other 8-bit sound
chips (AY-3-8910, SN76489, NES 2A03, GB DMG) later without rework.

It is **not** a SID player and **not** a `.sid` file emulator. It is a synth whose
voices sound like SID voices and whose instruments are expressive in the way SID
instruments were expressive — via a per-frame table VM, not via LFOs.

Status: **P0 closed, P1/P2/P3 built.** P0's CPU budget in §9 is real
breadboard_rp2350 numbers, not static estimates (history_chip.md §14a); target
configuration settled at **20 voices, 4 filter buses**. P1's engine skeleton is
in `engines/chip/`, and both halves of §11.1's validation (the `CHIP_STRICT`
spectral harness and the exact control-plane diff, both missing until now
despite history_chip.md §14a's earlier claim otherwise) are built and passing
— see history_chip.md §14b. P2 adds the filter buses themselves: binding
(§5.2), degrade-to-unfiltered, and the per-bus idle skip §5.2 flagged as a P2
TODO — see history_chip.md §14c. P3 adds the frame table VM (§6) —
wave/pulse/filter tables, vibrato, gate-off timer — and resolves open question
1 — see history_chip.md §14d. **P1, P2 and P3 have each had a by-ear pass on
real breadboard_rp2350 hardware since §14a** — P2's CC18 filter-toggle bug
(history_chip.md §14c.2) and P3's seven instrument bugs (history_chip.md
§14d.5) were both found this way. **None of P1/P2/P3 have had their CPU cost
re-measured on hardware since §14a**, though (history_chip.md §14c.3 flags
P2's dynamic bus binding specifically as still owed a bench check); every
cycle number since history_chip.md §14a is still the last hardware-measured
state.

---

## 1. Scope & phasing

| Phase | Deliverable |
|-------|-------------|
| **P0** | **Measurement gate.** Primitives built standalone + host harness; measure per-voice cost, filtered-voice cost *including bus round trip*, and sync at 1× vs 2× oversampling. No engine, no VM. *(Closed — §14a. Hardware measured on breadboard_rp2350; 20 voices / 4 buses confirmed at 86.6% worst-case load.)* |
| **P1** | Engine skeleton: `engines/chip/`, `VoiceType` dispatch, static MIDI-channel→voice map, register-stream playback path. Prove a SID voice sounds right against reSID. *(Built — §14b. Device engine, `CHIP_STRICT` spectral harness, and the exact control-plane diff (`sid_ctl_diff.py`) all in and passing.)* |
| **P2** | Filter buses + 6581 model (cutoff LUT + saturation). Bus binding, degrade-to-unfiltered. *(Built — §14c.)* |
| **P3** | Frame table VM on Core 1: wave/pulse/filter tables, vibrato, arpeggio, hard restart, gate-off timer. This is where instruments become expressive. *(Built — §14d. Not yet heard on hardware.)* |
| **P4** | Instrument import: GoatTracker `.ins` host converter; hand-authored text format → header. Dynamic voice allocation. *(Built — §14e. Not yet heard on hardware.)* |
| **P5** | Speaker simulation output stage (§10). LCD UI. *(Built — §14f. Not yet heard on hardware.)* |
| **P6** | 8580 model (table swap). Combined-waveform LUTs. *(Deferred by the author's call — the combined-waveform LUT's only available source data is reSID's own sampled tables (`tools/sid_ref/resid/wave*.h`, GPL-2, deliberately gitignored/not vendored, same reasoning as the DAC tables at §14a.7). Unlike the DAC ladder, combined-waveform behavior has no known clean closed-form derivation to independently re-derive from — reSID's own tables come from resistor/leakage-level SPICE modeling, not a formula. Raised as a licensing question rather than guessed at; the author chose to defer P6 entirely rather than resolve it now. AND stays the 6581/8580 combined-waveform approximation, with §14a.4's documented ~200 dB error on the pulse combinations unchanged.)* |
| **later** | Other chips (§12): AY/YM2149, SN76489, NES 2A03, GB DMG. *(AY-3-8910/YM2149 P0-P3 (§12.1-§12.4) built, all hardware-verified -- P2's vibrato had a real bug, found and fixed from an on-hardware report (§12.3). First real AY CPU measurement: ~44 c/f/voice (§9, §12.4), retiring the old 10-25 c/f estimate.)* |

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
exists anywhere in the firmware. `module_groovebox.md` §5.2 already calls this pattern
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
cabinet, bypass. Built — §14f. All five share one HP→peak→LP→soft-clip chain
(`speaker_sim.h`); per-preset character is corner frequencies plus a drive
scalar into one fixed clip curve, not five different signal paths.

**Backports well beyond this module:** 808 through a boombox, Amiga through a TV,
and especially the speech engine — a toy-speaker curve in front of the Das Boot
bitcrusher does more for that character than further bit reduction would.

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

### 12.1 AY-3-8910/YM2149 P0 (measurement gate, primitives only)

**Hardware-verified.** The author, on real breadboard_rp2350: "everything
sounds fine and voices have quite low duty cycle" -- confirms the P0
primitives and P1 engine wiring (§12.2) both by ear and, qualitatively, on
CPU cost. AY's own primitives are cheaper than SID's by construction (no
filter, no combined-waveform AND, no DAC-table envelope lookup, just a
period counter, a 17-bit LFSR and a 32-step ramp), so a low duty cycle is
the expected shape of the result, not a surprise -- a real number (not just
"quite low") is still owed before it goes in §9-style budget language.

The author picked AY as the next chip: simple (3 tone + noise, no filter model),
and — unlike SID's combined-waveform problem (§14a.4), which needed real
chip-measurement data under a GPL license that couldn't be vendored — no
equivalent licensing wall. The best available reference,
[ayumi](https://github.com/true-grue/ayumi) (Peter Sovietov), is
**MIT-licensed**, confirmed via GitHub's API and its own `LICENSE` file
directly, not recalled. Unlike `tools/sid_ref/resid/` (GPL-2, gitignored,
never shipped — §14a.7), `tools/ay_ref/ayumi/` vendors `ayumi.c`/`ayumi.h`
outright, `LICENSE` committed alongside.

**Read in full before writing anything.** Three facts came directly out of
`ayumi.c`, not out of memory or a datasheet paraphrase:

- **Noise LFSR**: 17-bit, feedback `bit0 = bit0 XOR bit3` of the pre-shift
  register, fed into the vacated bit 16. Confirmed against ayumi's own
  `update_noise()`, the same "read the reference, don't guess the taps"
  discipline that caught SID's own tap error at F0 (`sid_osc.h`'s own
  header comment).
- **Envelope shape table**: ayumi's `Envelopes[16][2]` — 16 raw 4-bit codes,
  10 musically distinct shapes — ported verbatim into an enum/switch
  (`AY_ENVELOPE_SEGMENTS`, `src/chip/ay_envelope.h`) rather than
  ayumi's function-pointer table. Same information, this project's style.
- **Internal tick rate**: real hardware documents a clock/16 divider for the
  tone generator and clock/256 for the envelope generator (two different
  base rates) — but reconciling that prose against ayumi's own calibration
  did not resolve cleanly by hand (see `ay_osc.h`'s header comment for the
  arithmetic that didn't add up). Rather than trust a derivation that
  wouldn't close, this went with what could be *proven*: ayumi ticks every
  generator (tone, noise, envelope) once per call to its own internal
  `update_mixer()`, at a rate of clock/8 — derived directly from
  `ayumi_configure()`'s own `step` calibration, cross-checked against the
  textbook tone-frequency formula (clock/16/period) with no unaccounted
  factor, and then actually proven correct empirically rather than argued
  for: `tools/ay_ctl_diff.py` compares every tick of t00t's own primitives
  against ayumi's, bit-exact.

**Files**: `src/chip/ay_osc.h` (`AyTone`, `AyNoise`), `src/chip/ay_envelope.h`
(`AyEnvelope`, DAC tables, `ay_mix()`) — topology-free per §4's rule, same
as `sid_osc.h`/`env_sid.h`. Real hardware has 3 tone channels sharing *one*
noise generator and *one* envelope generator; these primitives don't assume
that sharing, so a future engine can give each dynamically-allocated voice
its own noise + envelope instead (trading the real chip's shared-envelope
trick for full 32-voice independence) — the same kind of topology
reinterpretation the SID engine already does versus its own `CHIP_STRICT`
harness. The **validation harness** (below) wires the real shared topology,
since that's what ayumi itself models and what a bit-exact diff needs.

**Host validation** (`tools/ay_ref/`, `tools/ay_ctl_diff.py`,
`tools/host_render/render_ay.cpp` + `t00t_ay_dump.cpp`) mirrors §11.1's
`CHIP_STRICT` split exactly:

| Domain | Result |
|---|---|
| tone (toggle sequence, 6 periods) | **PASS**, 4624/4624 ticks exact |
| noise (LFSR sequence) | **PASS**, 200000/200000 shifts exact |
| envelope (all 16 shape codes) | **PASS**, 2048/2048 points exact |
| dac (AY + YM tables) | **PASS**, 64/64 entries within float32 precision (1e-6) |

Every domain here is bit-exact — no sample-quantisation tolerance the way
SID's `env` domain needs one (§14a's `diff_env`), because this is a plain
ramp counter, not a piecewise-exponential one.

**Spectral comparison** (`tools/sid_compare.py`, reused unmodified — it's
generic over any two float32 WAVs) against `ayumi_render`'s real oversampled/
decimated/DC-filtered output shows the same *category* of gap SID's own P0
found and accepted (§14a.4): `render_ay.cpp` generates directly at 44.1 kHz
with no band-limiting, same as `render_sid.cpp` always has, so a `centroid_
ratio` around 2.0-2.4 across every stream is aliasing, not a logic bug —
the control-plane table above already proves the logic. Not yet priced in
dBc the way §14a.4 did for SID; worth doing before treating the number as
settled, same "measure it, don't assume it's fine" standard the rest of
this doc holds itself to.

**One real, non-aliasing gap, found not assumed**: the `mixer_combos`
stream (tone-disabled + noise-disabled at once — a documented AY quirk that
outputs a constant level, not silence) scored far worse than every other
stream (`coactive_frac` 0.61 against 1.0 everywhere else, `envelope_mae_db`
28.4 against ~1). Cause: `ayumi_remove_dc()` is a DC-blocking filter ayumi's
own reference renderer applies, which drives a sustained constant level
toward silence over time; `render_ay.cpp` has no equivalent stage, so its
output for that combo stays flat where ayumi's decays. Real AY-3-8910
hardware is very likely AC-coupled at its actual analog output the same
way — this is closer to "a free-bonus stage the primitives don't have yet"
(§10's SidBoardFilter precedent) than an error in the digital logic
itself, but it is a real difference and a future engine integration should
know about it rather than rediscover it by ear.

At the time this was written, P0's own scope stopped at the primitives and
host validation -- no engine, no device wiring. §12.2 (below) is that next
step, built the same session and now hardware-confirmed alongside it.

### 12.2 AY-3-8910/YM2149 P1 (engine integration)

Wires the P0 primitives into the real engine. `VT_AY` added alongside
`VT_SID` (`engine.h`) in the *same* `MAX_VOICES` pool and the *same*
`voice_alloc` dynamic allocation P4 already built for SID -- unlike SID's
own P1 (which needed a static channel map because dynamic allocation
didn't exist yet), AY-P1 skips straight to the mature infrastructure §12
promised ("additional VoiceTypes in this module, not new modules"). No new
allocator, no new MIDI note-routing plumbing.

**Instrument format** (`ay_instrument.h`/`ay_instruments.h`) is
deliberately static, no frame table -- the same shape SID's own P1 had
("no instrument system yet... one fixed default patch", history_chip.md §14b.1)
grown to three hand-authored patches instead of one, since even a static
format needed something to exercise §12's table row (tone-only lead,
envelope-driven "buzzer bass" at shape 8 -- a repeating sawtooth decay,
tuned to a ~50 Hz buzz -- and a noise-only percussive hit using shape 9's
one-shot decay-to-silence as its own release, no frame table required for
that much). A per-frame table (arpeggio, software volume/mixer envelopes)
is AY-P2's job, mirroring SID's own P1 -> P3 gap.

**One combined instrument-selection space** spans both chip types
(`midi_controller.cpp`'s `TOTAL_INSTRUMENT_COUNT = INSTRUMENT_COUNT +
AY_INSTRUMENT_COUNT`) rather than a separate chip-type selector -- a player
picks a patch, not a silicon. `CC_INSTRUMENT`/Program Change band against
the combined total; index `< INSTRUMENT_COUNT` is SID, the rest is AY.

**Real, load-bearing bug caught before it shipped**: the existing
`render_mask`/frame-tick/bus-feed loops all gated purely on
`render_mask & (1 << v)`, which now also includes `VT_AY` voices. Left
alone, SID's own frame-VM loop would have read `INSTRUMENTS[]` with an
AY-table index on an AY voice slot, and the bus-feed loop would have called
`SidVoice::tick()` on a slot that was never a `SidVoice` this trigger --
garbage audio, not a crash, so nothing would have flagged it short of
noticing the output was wrong. Fixed by adding an explicit `vp.voices[v].
type != VT_SID` guard to both loops before they were ever run.

**Real AY-3-8910 hardware has no gate/release concept at the chip level at
all** -- every tracker's "note off" is a software fiction. AY-P1 doesn't
invent one yet either: the mixed output mutes the instant MIDI gate goes
false, envelope-driven or not, so `active_mask`/`render_mask` for `VT_AY`
voices track `p.gate` directly rather than a decaying counter the way
`VT_SID`'s do -- there's nothing to decay until AY-P2's frame table can
ramp a release the way real AY tracker instruments always have to fake one
in software.

**Mix scaling, not yet calibrated**: AY's own mixer/DAC output
(`ay_envelope.h`'s `ay_mix()`) is normalized and effectively unipolar
(silence is a literal 0, not a centred value) -- summed into `dry[]`
as-is it would inject a real DC bias into an otherwise-bipolar bus SID's
`(w - wave_zero()) * amp` voices already populate correctly. Fixed for the
*common* case by treating the mixer's gate bit as bipolar (0/1 -> -1/+1)
before scaling, which recovers real AC content for every ordinary tone/
noise combination; the one case this doesn't fully fix is the documented
"both tone and noise disabled" AY quirk (§12.1's own finding, there as a
literal constant on real hardware) landing as a half-scale residual DC
instead of a full one. `AY_MIX_SCALE` (the constant mapping AY's [0,1]
DAC output onto SID's raw `dry[]` magnitude) is a first guess against
SID's own typical peak, not a calibrated one -- same status P3's vibrato
constants had before the author's by-ear pass found them 4x off. Needs a real
listen, same as everything below.

**Hardware-verified** (§12.1's note applies here too -- "everything sounds
fine and voices have quite low duty cycle", the author, breadboard_rp2350):
confirms the mix-scaling/bipolar-gate fix above actually sums correctly
against SID voices in practice, not just in theory, and that the abrupt
gate-mute (no release tail) doesn't read as broken by ear even though it's
a known, real limitation.

**Not built**: AY-P2's frame table (arpeggio, software envelope/mixer
automation), YM2149 model selection (hardcoded to AY8910's DAC table --
`AyInstrument` has no model field yet), AY voices in `display.cpp`'s
per-voice grid (SID-only for now, a known gap not a design decision).

### 12.3 AY-3-8910/YM2149 P2 (frame table)

Built on the hardware-confirmed P1 base (§12.2) without touching it: an
instrument with no tone/volume table rows behaves exactly as it did before
this phase, since `ay_vm_frame_tick()` only steps a table that has rows.

**`ay_instrument.h` grows a tone table (`AyToneTable`/`AyToneRow`,
arpeggio) and reuses `instrument.h`'s `SweepTable`/`SweepRow` verbatim for
a software volume envelope** -- the real mechanism every AY tracker
instrument has always needed, since the chip itself has no release. Real
hardware's `e_on` bit is exclusive with manual volume, so the format keeps
that: `use_envelope` instruments never read the volume table at all, same
as they never read `initial_volume` past the first frame. Two more
instruments demonstrate the new rows, same "one documented feature each"
pattern as before: `AY_INS_ARP` (major-triad arpeggio + light vibrato) and
`AY_INS_PLUCK` (a fast volume-table decay plus `gate_off_timer` -- AY's
actual answer to "no hardware release", finally built).

**Arpeggio is exact, vibrato is a documented approximation, and the two
are deliberately not the same math.** AY's period register is *inversely*
proportional to pitch (`note_freq.h`'s `ay_hz_to_period`), unlike SID's
`freq_reg`, so reusing SID's additive-ratio approach verbatim would wobble
asymmetrically -- sharper on one side of centre than the other. Arpeggio
divides the base period by the same Q16 `semitone_ratio_q16[]` table SID's
own vibrato already multiplies by (multiplying Hz by a ratio is dividing
period by it -- no inverted copy of the table needed), which is exact.
Vibrato stays additive-on-period, a small-angle approximation of that same
correction, chosen because neither implementation claims to be physically
calibrated yet (SID's own vibrato_depth carries the identical "raw wobble
scale, not cents" caveat, history_chip.md §14d.5 finding 2) -- not worth a second,
more expensive code path to fix an approximation nobody has tuned by ear
regardless.

**Trigger handling mirrors SID's own P3 fix, not its P1 mistake.**
`ay_tone[v].set_period()` is set unconditionally only once, at trigger
(raw base period, immediate) -- not every buffer. `ay_vm_frame_tick()`
becomes the ongoing authority on it from the next frame tick onward,
exactly SID's pattern for exactly the same reason: setting it every buffer
would stomp arpeggio/vibrato on every buffer that doesn't also contain a
frame tick, the precise bug SID's P3 by-ear pass found and fixed (§14d.5
finding 3). Building AY-P2 with that lesson already in hand rather than
rediscovering it a second time is the actual payoff of writing findings
like that down instead of just fixing them and moving on.

**Not built**: YM2149 model selection, AY voices in the LCD grid (both
carried over from P1, unchanged).

**Heard on hardware, and the vibrato bug this section warned about turned
out to be real, not just theoretical.** The author, on real hardware: "the arp
messes up after a couple of laps (3-4) ... more pronounced ... the higher the
base pitch."
Exactly the failure mode the additive-on-period design above should have
been checked against before it shipped, not after: `vibrato_delay = 30`
frames is ~3-4 laps of `AY_INS_ARP`'s 8-row table, and a *fixed absolute*
delta subtracted from period is a tiny relative pitch shift at a large
(low-pitch) period and a huge one at a small (high-pitch) period, since
period is inversely related to pitch. Traced on host before touching the
code again (same discipline as SID's PWM-sweep and osc.inc bugs, §14d.5):
at a 880 Hz base, period went 84 -> 38 -> 26 across three frames the
instant vibrato armed -- a jump from ~1319 Hz to ~4263 Hz, not a wobble.
Fixed by scaling the delta by the *current* period (`(period * tri *
depth) >> 24`) so the fractional deviation stays constant across the pitch
range instead of the absolute one -- the same fix arpeggio's own divide-
by-ratio already had right two paragraphs up; vibrato just didn't get it
the first time. Re-traced at both a low (110 Hz) and high (880 Hz) base
after the fix: both now show a bounded, comparable *proportional* wobble
(a few percent either side of each arpeggio note), not a runaway one.
`vibrato_depth`'s scale is unchanged (still "raw, not cents," same
uncalibrated status as before) -- only the structural bug is fixed, not
promoted to a claim of precision it doesn't have.

### 12.4 AY-3-8910/YM2149 P3 (YM2149 model, named display)

Two of §12.2/§12.3's own "not built" gaps, both small and well-understood
enough not to need their own phase name: YM2149 model selection, and AY
voices actually appearing in the LCD.

**`AyInstrument` grows a `model` field** (`AyModel`, `ay_envelope.h`) --
per-instrument, not per-song or per-build, since a patch already owns its
mixer/envelope settings and a real tune was authored for one specific
chip. `AY_INS_LEAD_YM` (`AY_INS_LEAD`, byte-for-byte, model swapped) exists
to prove the one real, documented AY8910-vs-YM2149 difference (the DAC
tables) is actually reachable through an instrument, not just declared in
a struct nothing reads. `audio_engine.cpp`'s mix loop reads `ins.model`
instead of the P1/P2 hardcoded `AY_MODEL_AY8910`.

**Display: the author's own ask** ("improve the display with proper instrument
names ... instrument # as well as name, preferably on the same line") --
the INSTR row (the *currently selected* instrument, one line) was a bare
combined index before this. First pass also renamed the per-voice grid's
cells to names; the author's own correction: names for the one-line INSTR row
only, the grid stays numeric -- a name doesn't fit eight cells at once the
way it fits one summary line, and that was never the ask. The grid also
now shows `VT_AY` voices (still SID-only through P2, §12.2's own
flagged gap), same numeric `voice:instrument/table-row` shape as before,
just able to report an AY voice's own state instead of always reporting
inactive.

- `chipgen.py` now emits `INSTRUMENT_NAMES[]` alongside the existing
  `ChipInstrumentId` enum -- it already parses each `instrument NAME`
  block's name, this just also writes it out. `ay_instruments.h` gets the
  hand-written equivalent (`AY_INSTRUMENT_NAMES[]`), same reasoning
  `ay_instruments.h`'s own header comment already gives for staying
  hand-written rather than generated. (Named lookup is currently only
  exercised by the INSTR row, per the correction above -- the tables stay,
  ready for wherever a name is actually wanted next.)
- `ChipVoiceUiState` grows a `type` field -- without it, display.cpp had
  no way to know whether a voice's `instrument` index meant `INSTRUMENTS[]`
  or `AY_INSTRUMENTS[]`, which is exactly why AY voices reported the
  all-zero/inactive state through P1 and P2 rather than real telemetry.
- Every instrument number shown anywhere on the screen -- the INSTR row,
  every grid cell -- is the *combined* CC16/Program Change number
  (§12.2), not either table's own per-chip index. One number means
  one thing everywhere on this screen, the same principle the combined
  selection space itself was built on.

**Hardware-verified.** The author, on real hardware: "sounds fine. Very
similar to [AY_INS_LEAD], but I think the differences are subtle" -- comparing
`AY_INS_LEAD` against `AY_INS_LEAD_YM`, exactly the outcome the real, documented AY8910-vs-
YM2149 difference should produce (a real but subtle DAC-curve difference,
not a night-and-day one -- ayumi.c's own two tables, `ay_envelope.h`,
never claimed more than that).

**First real AY duty-cycle measurement, `AY_INS_LEAD`, no speaker filter,
no FX:**

| Voices | Duty cycle |
|---|---|
| idle | 2.8% |
| 1 | 4.5% |
| 4 | 8.6% |
| 8 | 14.1% |
| 16 | 23.7% |
| 24 | not enough keys to hold at once to measure |

Per-voice slope (idle -> 16v, the widest span, least sensitive to single-
measurement rounding): (23.7 - 2.8) / 16 = **1.31 pp/voice**, ~**44 c/f**
at the 3401 c/f = 100% (150 MHz / 44.1 kHz) baseline §9 already uses.
Narrower pairs agree within a few c/f (8v->16v: 40.8 c/f; 4v->8v: 46.8
c/f; 1v->4v: 46.5 c/f) -- consistent, not just a two-point average.

This retires §9's placeholder: "a PSG voice (AY/SN/NES, est. 10-25 c/f...)
is roughly a third of a SID voice's cost" was a guess made before any PSG
existed in this codebase to measure. The real number, ~44 c/f, is higher
than that estimate by roughly 2-4x -- the same shape of miss (estimates
wrong in both directions, "up to 3.7x", §9's own words) SID's own P0
already found for itself, now confirmed to not have been a fluke specific
to SID. Real AY is still cheaper than the measured SID voice (~108 c/f,
§9) by a wide margin, so the qualitative conclusion the estimate was
guessing at -- "SID remains the most expensive chip in the family" --
holds; only the specific multiple was wrong.

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

## 9. CPU budget

Baseline: 3401 cycles/frame = 100% at 150 MHz / 44.1 kHz.
Measured references — subtractive voice **5.9%** (`history_subtractive.md`),
speech voice **93.5 c/f (2.75%)** (`history_speech.md`), reverb **~8%** and
idle **~0.6%** (`history_subtractive.md`).

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

**Comparison for context:** ~~a PSG voice (AY/SN/NES, est. 10–25 c/f,
0.3–0.7%) is roughly a third of a SID voice's *estimated* cost~~ —
**measured, not estimated, as of AY-P3 (§12.4): ~44 c/f (~1.3%), from real
breadboard_rp2350 duty-cycle sweeps of `AY_INS_LEAD` at 1/4/8/16 voices.**
Higher than the struck-through guess by roughly 2–4×, the same shape of
miss (both directions, "up to 3.7×") §14a.9 already found for SID's own
voice cost — not a fluke specific to one chip's estimate. Still
comfortably cheaper than the measured SID voice (~108 c/f) by a wide
margin, so SID remains the most expensive chip in the family; only the
specific multiple was wrong, not the conclusion.

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
8. **Glossary stays in this doc**, as a trailing appendix (§16), rather than
   moving to `architecture.md` as first proposed — the voice/channel/track
   collision predates this module, but no other module doc carries a
   glossary either, so there was nowhere shared to merge it into.
9. **Primitives are topology-free from the first commit** (§4).
10. **Hard restart is an instantaneous envelope reset**, zero latency (§4.3).
11. **>32 voices rejected**; tag-bit approach recorded but not built (§8).

---

## 15. Open questions

1. ~~**Sub-block size vs. frame tick.** Speech cuts sub-blocks per voice; the
   frame tick is global here. Does the VM tick get its own cut point, or ride
   the existing `SUBBLOCK` boundary with ≤1.45 ms of quantisation? (1.45 ms on
   a 20 ms frame is 7% — likely fine, unlike Core 0's 29%. Confirm at P3.)~~
   **Resolved (`history_chip.md` §14d): rides `CHIP_SUBBLOCK`.** Built as the doc's own lean
   predicted -- no separate sample-exact cut point. Not yet confirmed by ear.
2. **Multispeed default.** 50 Hz PAL only at v1, or 2× from the start? Affects the
   instrument data format, so it wants deciding before assets are generated.
3. ~~**Telemetry struct contents** for the LCD — how much VM state does the display
   actually want, and does it justify widening the reverse channel beyond the
   bitmap?~~ **Resolved (`history_chip.md` §14f): `ChipVoiceUiState` (`instrument`, `wave_pos`,
   `held`)** — exactly the two pieces of VM state §6.1's own text named
   ("current table row, active instrument"), same shape as speech's per-voice
   telemetry. Does not widen the Core1→Core0 reverse channel's *bitmap*
   (`multicore_fifo`) itself — a separate polled getter, same mechanism
   speech already uses for the same reason.
4. ~~**Speaker stage placement vs. master FX** — confirmed downstream of the insert,
   but does it sit before or after the final `__ssat` clip?~~ **Resolved
   (`history_chip.md` §14f): before.** The stage's own soft clip is the cone-breakup
   *character* (a deliberate tonal choice, varying by preset), not a second
   safety clamp — it runs, then the existing final `__ssat` still guards the
   int16 conversion regardless of what the speaker stage's clip did.
5. ~~**`FILTER_BUS_COUNT` after P0** — 4 is a guess. If voices measure cheap, is 6
   better spent than 6 more voices?~~ **Resolved (§9, `history_chip.md` §14a.9): no.** Voices
   measured *more* expensive than buses (~108 vs ~80 c/f), the opposite of what
   this question assumed — a bus is the cheaper thing to add, not the pricier
   one. 4 buses / 20 voices confirmed on hardware with real headroom (86.6%
   worst case); trading buses for voices (22v/2f) measured *worse*, not
   better (89.0%).

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

## 16. Glossary

Terms have been overloaded across this module's design discussion and the existing
docs. Proposed definitions, grounded in current code usage:

| Term | Meaning | Note |
|---|---|---|
| **Engine** | Build-time synthesis module, `src/engines/<name>/` | One per firmware image; already unambiguous |
| **Voice** | A Core 1 render slot: index `0..MAX_VOICES-1`, one bit in the active bitmap, one `VoiceParams` entry | **Existing meaning in code — do not redefine** |
| **Channel** | MIDI channel, and nothing else | Currently overloaded three ways |
| **Track** | A tracker column | `module_tracker.md` says "channel N is voice N"; "track" removes the collision |
| **Note** | One played event; occupies exactly one voice | True under §4.4 |
| **Preset** | The patch definition (`VoicePreset`, `presets.h`) | t00t-wide term |
| **Instrument** | Chip-module synonym for preset | Alias; do not rename existing code |
| **Chip** | A *tonal profile* selecting oscillator/envelope/LFSR/filter models | Not a container, not an allocation unit |
| **Filter bus** | A pooled, typed filter instance that voices route into | This module only |
| **Frame** | One tick of the chip control clock (default 50 Hz) | Distinct from "buffer" and from "sample" |
