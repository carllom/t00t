# T00T — Groovebox Mode: TB-303 + TR-808/909 (Design & Implementation Plan)

A design document for a "groovebox" variant of the t00t engine: a mono/duo
**TB-303-style acid bass** synth alongside an **808/909-style analog/digital
hybrid drum machine**. It is a *mode* — a build-time engine variant that replaces
the general subtractive engine, not something that runs in parallel with it.

Status: **design draft.** No code written yet. Numbers for CPU are estimates
derived from the measured figures in `engine.md`.

---

## 1. Scope & phasing

| Phase | Deliverable |
|-------|-------------|
| **P0** | Groovebox engine skeleton: per-voice `VoiceType` dispatch, fixed voice map, MIDI drive |
| **P1** | TB-303 voice: saw/square osc, 4-pole ladder LP, dual envelope (amp + filter), accent, slide |
| **P2** | 808 analog drums: BD, SD, toms/congas, clap, cowbell, rim, hats, cymbal |
| **P3** | 909 hybrid: retuned analog drums + **sample-based** hats/cymbals/crash/ride (reuses existing sample playback) |
| **X1** | Optional: second TB-303 voice |
| **X2** | Optional: free sample-trigger pads (falls out of P3 for free) |
| **later** | Sequencer + LCD UI on Core 0 (out of scope here; drops into the existing input layer) |

**Start with plain instrument MIDI control** — no sequencer. The sequencer is a
Core-0 input source added later that calls the same voice-trigger entry points
MIDI uses (this is already how the architecture is designed; see `engine.md` §
"Adding a Sequencer").

---

## 2. What we already have (reuse inventory)

The existing engine already provides most of the raw DSP. Mapped against what the
three target machines need:

| Existing component | File | Reused for |
|--------------------|------|------------|
| Sine (wavetable, interp) | `osc/sine.*` | BD/tom/conga body, snare shell tones, clave |
| Square (variable duty) | `osc/square.h` | 303 pulse, cowbell/hats/cymbal metal bank |
| Saw (raw + BLEP) | `osc/saw*.h` | 303 sawtooth |
| Triangle | `osc/triangle.h` | tom/conga body alt |
| Noise (16-bit LFSR) | `osc/noise.h` | snare snappy, clap, hats/cymbal noise component |
| Sample playback (int8 PCM, looped, interp) | `osc/sample.h` | **909 hats/cymbals/crash/ride**, sample-trigger pads |
| ADSR envelope | `envelope.*` | amp contours (extended → one-shot decay mode) |
| SVF multimode (LP/BP/HP/notch, res→self-osc) | `filter.h` | snare/clap/hat/cymbal band/high-pass; 303 filter building block |
| Sine LFO, 4 destinations | in `audio_engine.cpp` | not central to drums; available for 303 mod if wanted |
| Delay + Freeverb FX (global, post-mix) | `fx/delay.h`, `fx/reverb.h` | groovebox master FX (unchanged) |
| Dynamic voice allocator | `voice_alloc.*` | *bypassed* in groovebox (fixed voice map) — see §6 |
| ParamExchange double-buffer IPC | `engine.h` | unchanged mechanism, new `VoiceParams` payload |
| MIDI transports + controller | `midi/` | note/velocity/CC → drum & 303 triggers |

**Key insight:** 909's metallic voices are *samples*, and we already have a
mature sample player. So 909 hats/cymbals are largely done once P0 gives us a
`DRUM_SAMPLE` voice type — and they are **cheaper** than 808's analog hats.

---

## 3. Gap analysis per machine

### 3.1 TB-303

The 303 is a simple mono subtractive synth whose character comes from four things
we do *not* fully have:

| 303 feature | Have? | Gap |
|-------------|-------|-----|
| 1 VCO, saw **or** square (~50% pulse) | ✅ | none — pick `WAVE_SAW`/`WAVE_SQUARE` (BLEP for clean, raw for grit) |
| **24 dB/oct (4-pole) resonant LP** | ❌ | current SVF is 2-pole/12 dB. **Need a 4-pole ladder filter** (the defining "squelch") |
| **Dedicated filter envelope** (env mod, own decay) | ⚠️ | engine has one ADSR shared via `filter_env_amount`. 303 wants a *second* envelope so filter decay ≠ amp decay |
| Amp envelope = fast attack + decay (no real sustain) | ⚠️ | expressible with ADSR (sustain≈0) but a one-shot AD is cleaner |
| **Accent** (louder + snappier + more env mod) | ❌ | new: velocity-triggered boost of amp, filter-env depth, and a faster env |
| **Slide/glide** (legato portamento) | ❌ | new: per-voice pitch glide toward target `phase_inc` |
| Mono, last-note priority | ⚠️ | needs mono voice logic (currently poly allocator) |

**New for 303:** 4-pole ladder filter, second per-voice envelope, accent logic,
portamento generator, mono note-priority handling.

### 3.2 TR-808 (fully analog synthesis)

Every 808 voice is analog synthesis — no samples. Broken down by generator:

| 808 voice | Synthesis recipe | Have? | Gap |
|-----------|------------------|-------|-----|
| **Bass drum** | sine + **downward pitch env** + amp decay; tone/decay knobs | ⚠️ | need **pitch-envelope** generator (decay scaling `phase_inc`) |
| **Snare** | 2 sine shell tones (~185 + 330 Hz) + noise→BP; "snappy" balance | ⚠️ | need **multi-oscillator voice** (2 osc + noise summed) |
| **Toms / congas** (lo/mid/hi) | like BD, higher tuned, pitch env + decay | ⚠️ | same pitch-env generator |
| **Clap** | noise→BP with **multi-burst envelope** (3–4 fast retriggers + tail) | ⚠️ | need **clap envelope** (small state machine) |
| **Cowbell** | 2 square osc (~540 + 800 Hz) → BP → decay | ⚠️ | subset of the metal bank (§4.7) |
| **Rimshot / claves** | short pulse/ring, fast decay | ✅-ish | short AD on pulse/sine |
| **Hi-hats (CH/OH)** | **6 square osc** at inharmonic ratios → BP+HP → decay (short/long) | ❌ | need **metal oscillator bank** (6 squares) — the expensive one |
| **Cymbal** | same 6-osc metal bank, different filtering, long decay | ❌ | same metal bank |

**New for 808:** pitch-envelope generator, multi-oscillator drum voices, clap
envelope, the 6-square "metal" oscillator bank.

### 3.3 TR-909 (analog + digital hybrid)

909 shares the 808's *analog* section (BD, SD, toms, rim, clap — different circuit
values, punchier BD with a click transient) but its **hi-hats, crash, and ride are
6-bit PCM samples**.

| 909 voice | Source | Gap |
|-----------|--------|-----|
| BD (punchier, click transient) | analog | reuse P2 BD + add short **click/attack transient** |
| SD, toms, rim, clap | analog | reuse P2 generators, retuned |
| **CH / OH / crash / ride** | **PCM sample** | **reuse existing sample player** — just needs sample data + `DRUM_SAMPLE` type |

**New for 909:** essentially just the BD click transient and a handful of ROM
samples. The heavy lifting is already done by P0's sample voice type and the
existing `osc/sample.h`. This is the cheapest of the three to finish.

---

## 4. New DSP components (the actual build list)

Nine additions cover all three machines. Each is small and self-contained,
consistent with the existing `osc/` + `filter.h` header style.

### 4.1 One-shot decay envelope (`DecayEnv`)

Drums and 303 are trigger-driven one-shots. The current `Envelope` is gate-driven
ADSR. Add a lightweight AD/AR contour: linear (or instant) attack → exponential
decay to zero, ignores gate, runs to completion on trigger.

- Could be a **mode** of the existing `Envelope` (sustain=0, self-releasing) to
  avoid a new type — recommended, minimal churn. Add `env_config_decay(attack_ms, decay_ms)`.
- Cost: identical to current envelope (~free).

### 4.2 Second per-voice envelope

Today each voice owns one `Envelope[v]`. The 303 needs **amp env + filter env**
independently; drums need **amp env + pitch env**. Add a second envelope array on
Core 1: `Envelope env_a[v]` (amp) and `Envelope env_b[v]` (filter/pitch).

- Pure Core-1 state; no IPC change. `VoiceParams` carries two `EnvConfig`s (or a
  compact per-type config; see §5).
- Cost: one extra `advance()` per active sample (~negligible, a few float ops).

### 4.3 Pitch-envelope modulator

BD, toms, congas, and snare shells get their "boom → thud" from a fast downward
pitch sweep. Implement as: `env_b` (decay) scales an offset added to `phase_inc`:

```
eff_phase_inc = phase_inc * (1 + pitch_env_depth * env_b_level)
```

Reuses `env_b`; the only new thing is applying it to pitch instead of cutoff.
Cost: a couple of multiplies per sample on drum voices only.

### 4.4 4-pole ladder low-pass filter (`LadderFilter`) — the 303 centerpiece

The single most important new component. Two implementation options:

| Option | Recipe | Character | Cost |
|--------|--------|-----------|------|
| **A. Cascade 2× SVF** | run `filter.h` LP twice per sample | 24 dB/oct, easy, reuses tested code | ~2× current filter ≈ 3–4%/voice |
| **B. Dedicated ladder** (recommended) | 4 one-pole stages + resonance feedback (Moog/diode-ladder topology, fixed-point) | authentic 303 squelch, self-oscillation | ~4–6%/voice |

Recommendation: **prototype with Option A** (fast path to sound), then implement
**Option B** for authenticity. A transistor-ladder approximation in Q15/Q31 with
a `tanh`-ish saturation on the feedback path gives the acid growl; the RP2350's
`SMULL` (already used in `filter.h`) handles the 64-bit intermediates.

Only the 303 voice(s) use it, so the cost lands on 1–2 voices, not all 16.

### 4.5 Portamento / slide generator (303)

On a slid (legato) note, pitch glides from the previous note to the target instead
of jumping. Per-voice Core-1 state:

```
cur_inc += (target_inc - cur_inc) * slide_rate   // one-pole glide, per sample or per buffer
```

`VoiceParams` gains `target_phase_inc` + a `slide` flag/rate. When `slide` is off,
`cur_inc` snaps to target on trigger (normal 303 behavior). Trivial cost.

### 4.6 Accent (303)

Not a DSP block — a control-plane + scaling behavior. On an accented note
(velocity above a threshold, or a dedicated accent flag from the sequencer later):

- boost amp (VCA) by a fixed accent amount,
- increase filter-envelope depth (more "wow"),
- optionally shorten the filter-env decay for snap.

Implemented as fields in the 303 `VoiceParams` set by Core 0 at note-on; Core 1
just reads them. Trivial.

### 4.7 Metal oscillator bank (`osc_metal`) — 808 hats/cymbal/cowbell

A bank of **N square oscillators** at fixed inharmonic frequency ratios, summed:

```
out = Σ osc_square(phase_k, 50%)   for k in 0..N-1
phase_k advances at ratio_k * base_inc
```

- **Cowbell**: N=2 (~540/800 Hz).
- **Hats & cymbals**: N=6 (classic 808 inharmonic ratio set).
- Output feeds SVF band-pass then high-pass, then the decay env (short for CH,
  long for OH/cymbal).

This is the **most expensive drum voice** (6 oscillators + 2 filter passes). See
CPU budget (§7). Mitigations: it's one-shot and short; CH/OH are mutually
exclusive (hi-hat "choke"); and in **909 these become samples** (much cheaper).

### 4.8 Multi-oscillator snare voice

Snare = 2 sine/triangle shell tones (with fast pitch/amp decay) + noise through a
band-pass, mixed by a "snappy" balance. Not a new oscillator — a **voice recipe**
that sums two `osc_sine` calls + one `osc_noise`→SVF-BP within a single voice slot.
Establishes the "voice can contain several generators" pattern that the metal bank
and clap also use.

### 4.9 Clap envelope (`ClapEnv`)

The 808 clap is noise→BP shaped by a **multi-burst** contour: ~3 short bursts
~10 ms apart, then a longer decaying tail. Implement as a tiny state machine /
stepped amplitude table driving `env_a`. Only the clap voice uses it. Trivial cost.

---

## 5. Architecture integration

### 5.1 Groovebox as a build-time engine variant

Follows `architecture.md`'s **Option A** (compile-time engine selection): a new
`engines/groovebox/` directory selected by `T00T_ENGINE=groovebox`, defining its
own `VoiceParams`, `audio_engine_run()`, and preset/kit tables. It **replaces**
the subtractive engine for that build — no runtime coexistence, matching the
"synth is *set into* a mode" framing.

> Runtime mode-switch (subtractive ↔ groovebox without reflash) is possible later
> but costs RAM (both param layouts resident) and a dispatch layer. Not
> recommended for the first version.

### 5.2 Per-voice `VoiceType` dispatch (contained Option B, *within* this engine)

Unlike the melodic engine (all voices identical, dispatched only by waveform), the
groovebox is **heterogeneous**: a 303 voice and a snare voice run fundamentally
different code. So *inside* the groovebox engine, each voice carries a type tag and
the render loop dispatches per voice:

```cpp
enum VoiceType : uint8_t {
    VT_TB303,        // saw/square + ladder LP + dual env + slide/accent
    VT_DRUM_BD,      // sine + pitch env + amp decay (+ click for 909)
    VT_DRUM_SNARE,   // 2 tone + noise→BP
    VT_DRUM_TOM,     // sine/tri + pitch env (lo/mid/hi via tune)
    VT_DRUM_CLAP,    // noise→BP + clap env
    VT_DRUM_METAL,   // N-square metal bank → BP/HP (cowbell/hats/cymbal)
    VT_DRUM_SAMPLE,  // PCM playback (909 hats/cymbals, sample pads)
    VT_DRUM_RIM,     // short pulse/ring
};

struct VoiceParams : VoiceNoteBase {   // phase_inc, amplitude, trigger, gate
    VoiceType type;
    union {
        Tb303Params  tb303;
        BdParams     bd;
        SnareParams  snare;
        TomParams    tom;
        ClapParams   clap;
        MetalParams  metal;   // N, ratios[], filter, decay
        SampleParams sample;  // const SampleDef*
        RimParams    rim;
    };
};
```

The `union` is sized to the largest member (`MetalParams` with its ratio table, or
just store a pointer to a const ratio set to keep it small). With only 16 voices
this is a few hundred bytes per param block — negligible.

Render loop:

```
for each voice v:
    handle trigger/gate/slide
    switch (p.type):
        case VT_TB303:  render_303(v, p);   break;
        case VT_DRUM_*: render_drum(v, p);  break;
```

Each `render_*` is a focused inner loop. The compiler keeps them separate and
optimizes each; no per-sample megaswitch inside the hot path (the switch is once
per voice per buffer, not per sample).

### 5.3 IPC unchanged

`ParamExchange` mechanism (double-buffer, `commit()`/`active()`, `__sev()`) is
untouched — only the `VoiceParams` payload type changes, exactly as
`architecture.md` anticipates. Reverse FIFO active-voice bitmap: unchanged.

---

## 6. Voice allocation & control mapping

A drum machine has a **fixed instrument set**, not dynamic polyphony. So in
groovebox mode we **bypass the dynamic allocator** and use a static voice map:

| Voice | Instrument | Notes |
|-------|-----------|-------|
| 0 | TB-303 #1 | mono, last-note priority, slide/accent |
| 1 | (opt) TB-303 #2 | X1 |
| 2 | BD | one-shot |
| 3 | SD | one-shot |
| 4 | Low tom / conga | |
| 5 | Mid tom | |
| 6 | Hi tom | |
| 7 | Clap | |
| 8 | Cowbell / rim | |
| 9 | Closed hat | chokes open hat |
| 10 | Open hat | choked by CH |
| 11 | Cymbal / crash | |
| 12–15 | sample pads (X2) / spare | |

Retriggering a drum voice just bumps its `trigger` counter — the existing
trigger/gate mechanism already handles "re-fire even if the previous hit is still
ringing." No allocator search needed. **CH↔OH choke**: firing CH sets OH's env to
a fast release (mimics the shared 808 hi-hat circuit).

### MIDI control (P0–P3, pre-sequencer)

- **Drums on a drum channel** (GM-style, e.g. MIDI ch 10): fixed **note → voice**
  map (BD=36, SD=38, CH=42, OH=46, etc.). Velocity → hit level.
- **303 on its own channel**: note → pitch; **accent** = velocity ≥ threshold (or
  a note in a high velocity band); **slide** = overlapping/legato notes (note-on
  before previous note-off) engage portamento.
- **CCs** map to per-instrument knobs: 303 cutoff/resonance/env-mod/decay/accent;
  per-drum tune/decay/tone/snappy. Reuses the existing CC plumbing in
  `midi_controller.cpp` (the FX CC block shows the pattern).
- A **"kit" table** (analogous to the current `presets[]`) holds the per-instrument
  synthesis params; ships an **808 kit** and a **909 kit**.

The `midi_controller` still writes shadow `VoiceParams` and commits — the only
change is a groovebox-specific note-routing table instead of the poly allocator.

---

## 7. CPU budget

Baseline anchors from `engine.md`: RP2350, 1 voice+LFO ≈ 6%, filter ≈ 1–2%/voice,
16-voice max ≈ 75–80%. Estimated groovebox costs:

| Voice | Est. cost | Notes |
|-------|-----------|-------|
| TB-303 (osc + 4-pole ladder + 2 env + slide) | ~8–10% | ladder filter dominates |
| BD / tom (sine + pitch env + amp env) | ~3% | |
| Snare (2 osc + noise + BP) | ~6% | |
| Clap (noise + BP + clap env) | ~4% | |
| Cowbell (2-square metal + BP) | ~4% | |
| **808 hat/cymbal (6-square metal + BP+HP)** | ~8–10% | most expensive; one-shot, short |
| **909 hat/cymbal (sample)** | ~4–6% | cheaper than 808 analog |
| Sample pad | ~4–6% | as measured for Fairlight-class samples |

**Realistic simultaneous worst case** (2×303 sustaining + kick + snare + hat +
cowbell): ≈ 20 + 3 + 6 + 9 + 4 ≈ **~42%**, plus global FX. Comfortable headroom —
drums are mostly short one-shots that don't all sustain at once, and only the
303(s) run continuously.

**Watch item:** the 808 6-square hat/cymbal is the pricey path. If a dense pattern
(closed hat every 16th + open hat + cymbal + full 303s) overruns a buffer,
mitigations in order of preference: (1) use the **909 sample** hats, (2) reduce the
metal bank to 4 oscillators, (3) precompute a metal wavetable, (4) cap
simultaneous metal voices. None are needed until measured.

---

## 8. Optional extensions

- **X1 — Second TB-303:** trivial structurally — assign voice 1 as a second mono
  303 on its own MIDI channel. Cost: another ~8–10% (two ladder filters). Fine
  within budget. Classic dual-acid-line setup.
- **X2 — Sample-trigger pads:** falls out of P3 for free. The `VT_DRUM_SAMPLE`
  voice type built for 909 cymbals is exactly a sample pad; map spare voices 12–15
  to `SampleDef`s from the existing `samples/` library. CPU permitting (§7),
  several can run at once.

---

## 9. Sequencer (future, out of scope here)

When added, the sequencer lives on **Core 0** as another input source in the main
loop (alongside `usb_midi_poll` / `uart_midi_poll`), calling the same
voice-trigger path. It owns step state + timing (a hardware timer or the audio
buffer count as a clock), generates note/accent/slide events per step, and writes
the shadow block. The **LCD** (`src/wslcd/`, already Core-0-owned at low priority)
renders the pattern/step UI. No engine or IPC change is required — this is the
design already sketched in `engine.md`.

---

## 10. Recommended build order

1. **P0 skeleton** — `engines/groovebox/`, `VoiceType` + union `VoiceParams`,
   fixed voice map, groovebox note-routing in a new controller path, kit table
   stub. Prove one sine BD triggers from MIDI.
2. **Drums first, cheap ones** — BD, toms (pitch-env generator), clap (clap env),
   snare (multi-osc voice). These validate the multi-generator-per-voice pattern.
3. **Metal bank** — cowbell → hats → cymbal; measure CPU on the 6-osc path.
4. **TB-303** — osc + cascaded-SVF ladder (Option A) to get sound, then the
   dedicated ladder (Option B); add dual envelope, accent, slide, mono priority.
5. **909 kit** — sample-based hats/cymbals (`VT_DRUM_SAMPLE`) + BD click; retune
   analog voices. Ship 808 + 909 kits.
6. **Extensions** — 2nd 303 (X1), sample pads (X2) as budget allows.
7. **Later** — sequencer + LCD UI on Core 0.

---

## 11. Open questions / decisions to make

1. **Ladder filter fidelity vs. cost** — ship Option A (cascaded SVF) for v1, or
   go straight to the dedicated ladder (Option B)? (Recommend A→B.)
2. **Mode selection** — compile-time engine variant only (recommended), or invest
   in runtime subtractive↔groovebox switching?
3. **Metal bank size** — 6 oscillators (authentic 808) or 4 (cheaper) as default?
4. **Accent source** — velocity threshold now; dedicated accent flag once the
   sequencer exists?
5. **808 vs 909 as separate build kits or one switchable kit** at runtime via a CC
   / program change?
6. **Envelope reuse** — extend `Envelope` with a one-shot decay mode (recommended)
   or add a separate lightweight `DecayEnv` type?

---

## 11a. Code layout & integration strategy

**Take:** directory-per-engine selected by CMake, **not** `#ifdef`s on the big
divergences. Share DSP primitives verbatim (zero duplication), fork the ~3 truly
engine-specific units, and split the one awkward seam (the MIDI controller) into a
shared shell + forked routing. Estimated **~75–80% of existing code is shared
unchanged**; the fork surface is only ~500–600 lines, plus net-new DSP.

### Per-file disposition

| Tier | Files (existing) | Strategy |
|------|------------------|----------|
| **Shared DSP/infra** (~1,600 ln + LCD) | `osc/*` primitives, `envelope.*`, `filter.h` (SVF), `fx/*`, `output.*`, `samples.*`, `audio_common.h`, `midi/` transports + `midi_parser.h`, `wslcd/*` | share verbatim — no `#ifdef`, no dup |
| **Engine-specific** (~500 ln) | `engine.h` (VoiceParams part), `audio_engine.cpp` (render), `presets.h` (→ kit) | fork into `engines/groovebox/`; CMake selects one |
| **Awkward seam** (~236 ln) | `midi_controller.cpp` | thin shared shell (parser feed, CC scaling, commit) + **forked note-routing** per engine |
| **Unused in groovebox** | `voice_alloc.*` | not compiled into the groovebox build; unchanged |

**Why not `#ifdef` the intersections:** two ~270-line render loops (or two
`VoiceParams`) threaded through `#if/#else` in one file is unreadable and lets a
change to one engine break the other's build. `#ifdef` stays reserved for the small
board seams that already use it (`HAS_BUTTONS`, `MIDI_UART`). **Why not duplicate
DSP primitives:** they're identical and stable — duplication only earns its keep
for the ~60 lines of MIDI parse/commit boilerplate, where forking beats
`#ifdef`-threading shared routing logic.

### Prerequisite refactor: split `engine.h`

The enabling move. `engine.h` today mixes shared items (`MAX_VOICES`,
`PROFILE_PIN`, `Waveform`, `FilterMode`, `EffectParams`, `ParamExchange` skeleton)
with the engine-specific `VoiceParams`. Shared DSP headers include the whole thing
just for an enum. Split into:

- `src/engine_base.h` — shared: `MAX_VOICES`, `PROFILE_PIN`, `VoiceNoteBase`,
  `Waveform`, `FilterMode`, `EffectParams`, `ParamExchange` (templated on `VP`, or
  a typedef seam). Shared DSP includes **only this**.
- `src/engines/<engine>/engine.h` — that engine's `VoiceParams` / `VoiceParamBlock`.

After the split the entire shared DSP tier compiles against `engine_base.h` and
never sees a concrete `VoiceParams` — which is what removes the entanglement.

### Proposed layout

```
src/
  engine_base.h            ← NEW: shared enums, MAX_VOICES, VoiceNoteBase, ParamExchange
  audio_common.h  output.* samples.*                 ← shared, unchanged
  envelope.*  filter.h                               ← shared DSP (SVF); envelope gains decay mode
  fx/delay.h  fx/reverb.h                            ← shared, unchanged
  osc/                                               ← shared primitives, unchanged
    sine square triangle saw noise *_blep sample …
    metal.h            ← NEW: N-square metal bank (cowbell/hats/cymbal)
  ladder.h             ← NEW: 4-pole ladder LP (303)
  midi/                ← transports + parser: shared, unchanged
    midi_controller.*  ← shared shell; delegates note-routing to the engine
  wslcd/               ← shared LCD, unchanged
  voice_alloc.*        ← used by subtractive build only

  engines/
    subtractive/
      engine.h         ← current VoiceParams
      audio_engine.cpp ← current render loop (lightly moved)
      presets.h        ← current preset table
      route.cpp        ← poly-allocator note routing (extracted from midi_controller.cpp)
    groovebox/
      engine.h         ← VoiceType-tagged union VoiceParams (§5.2)
      audio_engine.cpp ← per-voice dispatch render loop; render_303 / render_drum
      kit.h            ← 808 + 909 kit tables (analog of presets.h)
      route.cpp        ← fixed voice map, mono-303 priority, CH/OH choke, accent/slide
      clap.h  drum.h   ← NEW: clap envelope + drum voice recipes

  main.cpp             ← includes "engine.h" (resolves to selected engine dir)
```

### CMake selection

`.cpp`s can't be swapped by include-path alone, so CMake adds the engine's sources
and its include dir:

```cmake
set(T00T_ENGINE "subtractive" CACHE STRING "subtractive | groovebox")
target_sources(t00t PRIVATE
    src/engines/${T00T_ENGINE}/audio_engine.cpp
    src/engines/${T00T_ENGINE}/route.cpp)
target_include_directories(t00t PRIVATE src/engines/${T00T_ENGINE} src src/midi src/wslcd)
if(T00T_ENGINE STREQUAL "subtractive")
    target_sources(t00t PRIVATE src/voice_alloc.cpp)   # groovebox uses a fixed map
endif()
```

`midi_controller.cpp` stays shared and calls an engine-provided routing hook
(declared in a small shared header, defined in each engine's `route.cpp`) for
note-on/off/CC. Parser feed, CC scaling, `shadow`/`commit` stay in the shared file.

### Overlap summary

- **Shared verbatim:** ~75–80% (all DSP primitives, envelope/SVF/FX, output,
  samples, MIDI transports + parser, LCD).
- **Forked (small):** `engine.h` VoiceParams, `audio_engine.cpp` render,
  presets→kit, `route.cpp` note-routing ≈ 500–600 lines total.
- **Net-new (touches nothing existing):** `ladder.h`, `osc/metal.h`, `clap.h`,
  `drum.h`, groovebox `engine.h`/`kit.h`/render.
- **One-time refactor cost:** split `engine.h` → `engine_base.h` + per-engine
  `engine.h`; extract subtractive note-routing into `subtractive/route.cpp`. Both
  are mechanical and benefit the subtractive engine too (cleaner separation).

---

## 12. Summary — the actual new-code list

**New DSP:** `LadderFilter` (303) · second per-voice envelope · pitch-env modulator
· portamento glide · metal oscillator bank (N-square) · clap envelope · one-shot
decay-env mode. Drum "voices" (BD/SD/tom/clap/metal/rim) are *recipes* combining
existing oscillators + filter + envelopes, not new primitives.

**New structure:** `engines/groovebox/` with `VoiceType`-tagged union `VoiceParams`
and per-voice dispatch; groovebox note-routing (fixed voice map, mono 303 priority,
CH/OH choke, accent/slide) replacing the poly allocator; 808 + 909 kit tables.

**Reused unchanged:** all oscillators, sample player, SVF, LFO, ADSR core, delay +
reverb FX, ParamExchange IPC, MIDI transports, dual-core structure.

**909's metal voices and sample-trigger pads are essentially free** once the
sample voice type exists — the existing sample player already does the work.
