# T00T — Groovebox Module: Development History

The original design document for the groovebox engine, preserved here with
its "as-built" annotations against what actually shipped. Unlike
`module_fm.md`/`module_chip.md`, this was never split into a phase-by-phase
build log at the time — the design-draft text and the as-built corrections
were written together, in place, as each part of the plan was checked
against the real code. See `module_groovebox.md` for the current spec.

CPU numbers throughout are estimates derived from the measured figures in
`history_subtractive.md`, not from measuring this module directly — no
profiling-pin measurement of groovebox itself has been taken.

---

## 1. Scope & phasing

| Phase | Deliverable |
|-------|-------------|
| **P0** | Groovebox engine skeleton: per-voice `VoiceType` dispatch, fixed voice map, MIDI drive |
| **P1** | TB-303 voice: saw/square osc, 4-pole ladder LP, dual envelope (amp + filter), accent, slide |
| **P2** | 808 analog drums: BD, SD, toms/congas, clap, cowbell, rim, hats, cymbal (as-built: rim was never implemented — see §3.2/§6) |
| **P3** | 909 hybrid: retuned analog drums + **sample-based** hats/cymbals/crash/ride (reuses existing sample playback) |
| **X1** | Optional: second TB-303 voice |
| **X2** | Optional: free sample-trigger pads (falls out of P3 for free) |
| **later** | Sequencer + LCD UI on Core 0 (out of scope here; drops into the existing input layer) (as-built: the MIDI-clock 303 sequencer part now exists, ahead of this row — see §9) |

**Start with plain instrument MIDI control** — no sequencer. The sequencer is a
Core-0 input source added later that calls the same voice-trigger entry points
MIDI uses (this is already how the architecture is designed; see `architecture.md` §
"Input Layer: Adding a Sequencer / Mod Source").

**Status when last reviewed against the code: P0–P2 built** (engine skeleton,
the TB-303 voice, and the 808 drum voices); **P3 not built**. A basic
MIDI-clock-driven step sequencer (`patterns.h`) also exists, ahead of §9's
"future, out of scope" framing below.

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
| SVF multimode (LP/BP/HP/notch, res→self-osc) | `filter.h` | snare/clap/hat/cymbal band/high-pass (as-built: the 303 does *not* use the SVF — it went straight to the dedicated 4-pole ladder in §4.4/Option B) |
| Sine LFO, 4 destinations | in `audio_engine.cpp` | not central to drums; available for 303 mod if wanted |
| Delay + Freeverb FX (global, post-mix) | `fx/delay.h`, `fx/reverb.h` | groovebox master FX (unchanged) |
| Dynamic voice allocator | `voice_alloc.*` | its `allocate()`/`release()` search is *bypassed* (fixed voice map) — see §6, but `voice_alloc.cpp` is still linked into the groovebox build and its Core-0 `update()`/`active_mask()` telemetry path is reused as-is to feed the LCD's voice-activity display (as-built: see §11a, which originally proposed excluding it entirely) |
| ParamExchange double-buffer IPC | `engine.h` | unchanged mechanism, new `VoiceParams` payload |
| MIDI transports + controller | `midi/` | note/velocity/CC → drum & 303 triggers |

**Key insight:** 909's metallic voices are *samples*, and we already have a
mature sample player. So 909 hats/cymbals are largely done once P0 gives us a
`DRUM_SAMPLE` voice type — and they are **cheaper** than 808's analog hats.

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

**As-built (`src/engines/groovebox/engine.h`), this landed differently:**
`VoiceParams` is a **flat struct**, not a union — every field for every voice
type sits inline (unused fields per type are simply ignored by that type's
render path); with only 16 voices the extra bytes are free and the
field-by-field init is far less error-prone than a union. The shipped
`VoiceType` enum is also `VT_SILENT` (explicit default, = 0), `VT_TB303`,
`VT_DRUM_BD`, `VT_DRUM_TOM`, `VT_DRUM_SNARE`, `VT_DRUM_HAT`, `VT_DRUM_METAL`,
`VT_DRUM_CLAP` — no `VT_DRUM_SAMPLE` (909 sample playback is still just a
`// Future:` comment in the enum, not an actual value yet) and no
`VT_DRUM_RIM` (rimshot was never implemented — no kit entry, no render
path; see §3.2/§6). There's also a `VT_DRUM_HAT` type (noise → HP + decay,
its own `render_noise_drum` path) that this design sketch never mentions —
in the shipped 808 kit it's unused: both closed and open hi-hat are routed
through `VT_DRUM_METAL` instead (the 6-square metal bank), matching this
doc's original hi-hat design rather than the simpler noise-HP alternative
the enum also provides. One more difference: `VoiceParams` does **not**
inherit from a `VoiceNoteBase` — that base struct is a cross-engine proposal
in `architecture.md`, not something `engine_base.h` actually defines;
`type`, `trigger`, `gate`, `amplitude`, etc. are all declared directly on
groovebox's `VoiceParams`, same as this doc's own §11a "Prerequisite
refactor" section still describes `engine_base.h` as providing it.

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

**As-built:** the switch dispatches to five distinct per-type functions
rather than one shared `render_drum` — `render_303`, `render_tonal_drum`
(BD/tom), `render_snare`, `render_noise_drum` (the unused `VT_DRUM_HAT`
path), `render_metal`, and `render_clap` — one focused loop per voice
recipe, in `audio_engine.cpp`.

### 5.3 IPC unchanged

`ParamExchange` mechanism (double-buffer, `commit()`/`active()`, `__sev()`) is
untouched — only the `VoiceParams` payload type changes, exactly as
`architecture.md` anticipates. Reverse FIFO active-voice bitmap: unchanged.

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

**As-built:** every item in this table is now built. `src/ladder.h` implements
the dedicated Option B 4-pole ladder (Stilson/Smith variant) directly — not
the Option A SVF-cascade prototype §4.4 suggested starting with; `aux_env`
(`audio_engine.cpp`) is the second per-voice envelope (filter env for the
303); accent, slide, and mono last-note-priority (with legato re-trigger on
release) all live in `src/engines/groovebox/midi_controller.cpp`'s
`play_303()` / held-note stack.

### 3.2 TR-808 (fully analog synthesis)

Every 808 voice is analog synthesis — no samples. Broken down by generator:

| 808 voice | Synthesis recipe | Have? | Gap |
|-----------|------------------|-------|-----|
| **Bass drum** | sine + **downward pitch env** + amp decay; tone/decay knobs | ⚠️ | need **pitch-envelope** generator (decay scaling `phase_inc`) |
| **Snare** | 2 sine shell tones (~185 + 330 Hz) + noise→BP; "snappy" balance | ⚠️ | need **multi-oscillator voice** (2 osc + noise summed) |
| **Toms / congas** (lo/mid/hi) | like BD, higher tuned, pitch env + decay | ⚠️ | same pitch-env generator |
| **Clap** | noise→BP with **multi-burst envelope** (3–4 fast retriggers + tail) | ⚠️ | need **clap envelope** (small state machine) |
| **Cowbell** | 2 square osc (~540 + 800 Hz) → BP → decay | ⚠️ | subset of the metal bank (§4.7) |
| **Rimshot / claves** | short pulse/ring, fast decay | ❌ | short AD on pulse/sine |
| **Hi-hats (CH/OH)** | **6 square osc** at inharmonic ratios → BP+HP → decay (short/long) | ❌ | need **metal oscillator bank** (6 squares) — the expensive one |
| **Cymbal** | same 6-osc metal bank, different filtering, long decay | ❌ | same metal bank |

**New for 808:** pitch-envelope generator, multi-oscillator drum voices, clap
envelope, the 6-square "metal" oscillator bank.

**As-built:** BD/toms (pitch-env via `aux_env` scaling `phase_inc`, shared
`render_tonal_drum`), snare (`render_snare`, exactly the 2-shell-tone +
noise→BP recipe described), clap (`ClapEnv`/`render_clap`, 3 bursts ~10 ms
apart then a decaying tail), cowbell (`VT_DRUM_METAL` with `metal_first=4,
metal_count=2` → the ~540/800 Hz pair), and hats/cymbal (`VT_DRUM_METAL`,
all 6 oscillators) are all built and shipped in `kit_808` (`kit.h`).
**Rimshot/claves is corrected here from the original ✅-ish to an honest
❌**: it was never actually implemented — no `VT_DRUM_RIM` type, no kit
entry, no dedicated render path (§6's voice map still lists it; that's
stale too).

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

**As-built:** the recommended route was taken — no separate `DecayEnv` type.
`audio_engine.cpp`'s `env_oneshot()` helper snaps an existing `Envelope` to
`level=1.0, state=ENV_RELEASE` (release-only decay), driven by an `EnvConfig`
built the same way as any other release contour. Named `env_config_decay()`
was never added as a separate function — call sites just build the
`EnvConfig` with zero attack/decay/sustain and the desired release time
(`kit.h`'s `apply_kit()`).

### 4.2 Second per-voice envelope

Today each voice owns one `Envelope[v]`. The 303 needs **amp env + filter env**
independently; drums need **amp env + pitch env**. Add a second envelope array on
Core 1: `Envelope env_a[v]` (amp) and `Envelope env_b[v]` (filter/pitch).

- Pure Core-1 state; no IPC change. `VoiceParams` carries two `EnvConfig`s (or a
  compact per-type config; see §5).
- Cost: one extra `advance()` per active sample (~negligible, a few float ops).

**As-built:** shipped as `amp_env[]` + `aux_env[]` (matching this section's
`env_a`/`env_b` naming intent, just renamed) — `VoiceParams` carries `amp_env`
and `aux_env` as two separate `EnvConfig` fields, exactly as sketched.

### 4.3 Pitch-envelope modulator

BD, toms, congas, and snare shells get their "boom → thud" from a fast downward
pitch sweep. Implement as: `env_b` (decay) scales an offset added to `phase_inc`:

```
eff_phase_inc = phase_inc * (1 + pitch_env_depth * env_b_level)
```

Reuses `env_b`; the only new thing is applying it to pitch instead of cutoff.
Cost: a couple of multiplies per sample on drum voices only.

**As-built:** matches this formula exactly — `render_tonal_drum()` and
`render_snare()` in `audio_engine.cpp` compute `eff_inc` from `aux_env`'s
level and `pitch_env_depth`, applied per-sample to `phase_inc`.

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

**As-built:** went straight to **Option B** — `src/ladder.h`'s `LadderFilter`
is the dedicated 4-pole design (Stilson/Smith variant off musicdsp.org #26),
not an Option A SVF-cascade prototype first. It differs from this sketch in
two respects worth noting: coefficients are recomputed per sample in
**floating point** (not the fixed-point Q15/Q31 this section describes) so
the filter envelope can sweep continuously without a fixed-point
resonance-compensation table, and the costly per-sample `expf()` for
resonance compensation is replaced by a cubic Taylor approximation (<6%
error) rather than eliminated by fixed-point tricks. Self-oscillation is
bounded by a cubic soft-clip on the feedback path plus a hard `±1.5` clamp
as a divergence safety net.

### 4.5 Portamento / slide generator (303)

On a slid (legato) note, pitch glides from the previous note to the target instead
of jumping. Per-voice Core-1 state:

```
cur_inc += (target_inc - cur_inc) * slide_rate   // one-pole glide, per sample or per buffer
```

`VoiceParams` gains `target_phase_inc` + a `slide` flag/rate. When `slide` is off,
`cur_inc` snaps to target on trigger (normal 303 behavior). Trivial cost.

**As-built:** simpler than sketched — no separate `target_phase_inc` field.
`VoiceParams.phase_inc` itself *is* the target; `audio_engine.cpp` keeps the
gliding value in a Core-1-only `glide_inc[]` array and blends it toward
`phase_inc` each sample when `p.slide` is set (one-pole, `SLIDE_COEFF`,
~20 ms time constant), snapping instantly when not sliding — same behavior,
one field instead of two.

### 4.6 Accent (303)

Not a DSP block — a control-plane + scaling behavior. On an accented note
(velocity above a threshold, or a dedicated accent flag from the sequencer later):

- boost amp (VCA) by a fixed accent amount,
- increase filter-envelope depth (more "wow"),
- optionally shorten the filter-env decay for snap.

Implemented as fields in the 303 `VoiceParams` set by Core 0 at note-on; Core 1
just reads them. Trivial.

**As-built:** the first two bullets are built — `midi_controller.cpp`'s
`play_303()` boosts amplitude and filter-envelope depth (`filter_env_amount`)
by an accent fraction derived from velocity above `ACCENT_VEL_THRESHOLD`
(scaled by a CC-controlled depth), and additionally boosts ladder-filter
drive (`vp.drive`) — a fourth accent dimension this section didn't mention.
**The third bullet (shortening the filter-env decay for snap) was never
implemented** — accent currently leaves `aux_env`'s decay time untouched.

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

**As-built:** `src/osc/metal.h` implements `osc_metal()` against a fixed
6-entry `METAL_FREQS[]` table; a voice picks its subset via `metal_first` +
`metal_count` (2 for cowbell, 6 for hats/cymbal) rather than a compile-time
`N` per voice type — one oscillator bank, sliced differently per instrument.
`render_metal()` (`audio_engine.cpp`) does band-pass then optional
high-pass exactly as described, with a makeup-gain multiplier to compensate
filter loss. CH/OH "choke" as-built is not a dedicated cross-voice mechanism
— see §6, both hi-hats share one physical voice slot, so a re-trigger
naturally cuts the ringing hit. 909 sample-based hats remain unbuilt (§3.3).

### 4.8 Multi-oscillator snare voice

Snare = 2 sine/triangle shell tones (with fast pitch/amp decay) + noise through a
band-pass, mixed by a "snappy" balance. Not a new oscillator — a **voice recipe**
that sums two `osc_sine` calls + one `osc_noise`→SVF-BP within a single voice slot.
Establishes the "voice can contain several generators" pattern that the metal bank
and clap also use.

**As-built:** matches — `render_snare()` sums two `osc_sine()` calls (tones 1
and 2, independently pitch-enveloped) plus one `osc_noise()` through the
shared `SVFilter` in band-pass mode, mixed by `tone_level`/`noise_level`.

### 4.9 Clap envelope (`ClapEnv`)

The 808 clap is noise→BP shaped by a **multi-burst** contour: ~3 short bursts
~10 ms apart, then a longer decaying tail. Implement as a tiny state machine /
stepped amplitude table driving `env_a`. Only the clap voice uses it. Trivial cost.

**As-built:** matches, with one difference — `ClapEnv` (`src/clap.h`) is its
own small standalone struct with its own `level`/`counter`/`burst`/`active_`
state, not implemented as a mode of `env_a`/`Envelope`. `render_clap()` reads
`clap_env[]` instead of `amp_env[]` for its voice slot. `CLAP_BURSTS = 3`,
`CLAP_INTERVAL ≈ 10 ms`, tail decay ≈130 ms — matches the description exactly.

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

**As-built, this table and choke description are stale in three ways**
(`kit.h`'s `GrooveVoice` enum + `kit_808`): (1) voice **8 is cowbell only** —
rimshot was never implemented (§3.2), so there's nothing at 8 to share the
slot with; (2) **closed and open hi-hat are not two voices** — both map to
the *same* slot, `GV_HAT = 9`; voice 10 is simply unused/spare, not a
separate open-hat voice; (3) the choke mechanism is correspondingly simpler
than "firing CH sets OH's env to a fast release" — there's only one physical
voice, so a closed-hat hit re-triggers it (bumps `trigger`, restarts the
envelope) and naturally cuts whatever the open hat was still ringing, the
same way any other drum retrigger works. No cross-voice envelope write
happens anywhere in the code.

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

**As-built:** the drum note→voice map and 303 note/accent/slide handling are
built exactly as described (`DrumNote` enum + `kit_find()`; `play_303()`'s
`ACCENT_VEL_THRESHOLD` + held-note legato stack in `midi_controller.cpp`).
The CC claim is only partly true, though: **live CCs currently only reach
the 303** (`CC_303_CUTOFF/RESO/ENVMOD/DECAY/ACCENT/WAVE/DRIVE`, plus the
shared FX CCs) — there is no per-drum tune/decay/tone/snappy CC control yet;
drum voices are entirely fixed by the kit table at trigger time. And only
the **808 kit ships** (`kit_808` in `kit.h`) — no `kit_909` exists yet
(matches §3.3/P3's not-yet-built status); `MIDI_PROGRAM_CHANGE` is parsed
but its handler is a no-op (kit switching not implemented).

CC assignments follow the Arturia BeatStep Pro's fixed absolute-mode 16-encoder
layout (CC16-31) — see `module_groovebox.md`'s MIDI Mapping table for the
current live assignments.

---

## 7. CPU budget

Baseline anchors from `history_subtractive.md`: RP2350, 1 voice+LFO ≈ 6%, filter ≈ 1–2%/voice,
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

## 9. Sequencer (originally scoped as future, out of scope)

When added, the sequencer lives on **Core 0** as another input source in the main
loop (alongside `usb_midi_poll` / `uart_midi_poll`), calling the same
voice-trigger path. It owns step state + timing (a hardware timer or the audio
buffer count as a clock), generates note/accent/slide events per step, and writes
the shadow block. The **LCD** (`src/wslcd/`, already Core-0-owned at low priority)
renders the pattern/step UI. No engine or IPC change is required — this is the
design already sketched in `architecture.md`'s "Input Layer: Adding a Sequencer /
Mod Source".

**As-built, this section turned out partly stale:** a basic MIDI-clock-driven
step sequencer already exists (`patterns.h` + the `seq_*` state/functions in
`midi_controller.cpp`), ahead of this section's "future" framing. As built
it's narrower than the general design above: **303-only** (no drum
sequencing), driven by incoming MIDI clock (24 PPQN) rather than a
freestanding hardware timer, with 3 fixed preset patterns (no
record/edit) selected by note-on on a dedicated pattern channel. It does
call the same `play_303()` voice-trigger path MIDI note-on uses, matching
this section's "no engine or IPC change" claim. The **LCD pattern/step UI
described here is still unbuilt** — `display.cpp` currently shows
voices/CPU/last-note/mode/FX only, no step grid.

---

## 11. Open questions / decisions made

Most of these were settled by what shipped; left in place as the original
questions, with the as-built answer noted per item.

1. **Ladder filter fidelity vs. cost** — ship Option A (cascaded SVF) for v1, or
   go straight to the dedicated ladder (Option B)? (Recommend A→B.)
   **As-built: went straight to Option B** — `ladder.h`'s dedicated 4-pole
   ladder is what ships; no SVF-cascade prototype stage in the current code.
2. **Mode selection** — compile-time engine variant only (recommended), or invest
   in runtime subtractive↔groovebox switching?
   **As-built: compile-time only**, per the recommendation (`T00T_ENGINE=groovebox`
   at CMake configure time; no runtime mode switch exists).
3. **Metal bank size** — 6 oscillators (authentic 808) or 4 (cheaper) as default?
   **As-built: 6** — `METAL_OSC_COUNT = 6` in `osc/metal.h`, not currently
   configurable down to 4.
4. **Accent source** — velocity threshold now; dedicated accent flag once the
   sequencer exists?
   **As-built: both, effectively** — live MIDI notes use the velocity
   threshold (`ACCENT_VEL_THRESHOLD`); the sequencer (§9, now built) instead
   sets a `SEQ_ACCENT` per-step flag that maps to a fixed high velocity
   (122) before going through that same threshold check — so the sequencer
   doesn't bypass the threshold, it just always clears it on accented steps.
5. **808 vs 909 as separate build kits or one switchable kit** at runtime via a CC
   / program change?
   **Still open** — moot until the 909 kit itself exists; only `kit_808` is
   built (§3.3/§6).
6. **Envelope reuse** — extend `Envelope` with a one-shot decay mode (recommended)
   or add a separate lightweight `DecayEnv` type?
   **As-built: extended `Envelope`** (via `env_oneshot()`), per the
   recommendation — see §4.1. (A separate `ClapEnv` type was still added,
   but only for the clap's distinct multi-burst shape, not as a general
   one-shot-decay alternative to this question.)

---

## 10. Recommended build order

1. **P0 skeleton** — `engines/groovebox/`, `VoiceType` + union `VoiceParams`,
   fixed voice map, groovebox note-routing in a new controller path, kit table
   stub. Prove one sine BD triggers from MIDI.
   (as-built: flat struct, not a union — see §5.2.)
2. **Drums first, cheap ones** — BD, toms (pitch-env generator), clap (clap env),
   snare (multi-osc voice). These validate the multi-generator-per-voice pattern.
   (built — §3.2/§4.)
3. **Metal bank** — cowbell → hats → cymbal; measure CPU on the 6-osc path.
   (built — §4.7. No CPU measurement of this module has actually been taken;
   §7's numbers are still estimates.)
4. **TB-303** — osc + cascaded-SVF ladder (Option A) to get sound, then the
   dedicated ladder (Option B); add dual envelope, accent, slide, mono priority.
   (built, but skipped the Option A staging step — went straight to the
   dedicated ladder; see §4.4/§11 Q1.)
5. **909 kit** — sample-based hats/cymbals (`VT_DRUM_SAMPLE`) + BD click; retune
   analog voices. Ship 808 + 909 kits.
   (**not built** — still P3.)
6. **Extensions** — 2nd 303 (X1), sample pads (X2) as budget allows.
   (**not built** — still open, per §8.)
7. **Later** — sequencer + LCD UI on Core 0.
   (**partly built** — a basic MIDI-clock 303 sequencer exists; the LCD
   step UI doesn't. See §9's as-built note.)

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

**As-built, the last two rows didn't land this way.** `CMakeLists.txt`
picks `src/engines/${T00T_ENGINE}/midi_controller.cpp` **wholesale** if it
exists for that engine, else falls back to the shared `src/midi/midi_controller.cpp`
— groovebox's is a full ~374-line standalone file (parser feed, CC scaling,
note routing, the 303 sequencer, `commit()`, all of it), not a thin shared
shell delegating to a per-engine routing hook. There is no shared-shell/
forked-hook split anywhere in the actual code. And `voice_alloc.cpp` is
**not** excluded from the groovebox build — `CMakeLists.txt` links it for
every engine except `tracker` (which has no dynamic allocation at all); the
groovebox build only skips *calling* `voice_alloc_allocate()`/`release()`
(its own fixed-map routing never calls them), while still using
`voice_alloc_init()`/`update()`/`active_mask()` for the LCD's voice-activity
telemetry (see §2's reuse-inventory note above).

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

**As-built:** the split happened and `src/engine_base.h` is real and shared
by every engine, but it does not define `VoiceNoteBase` — that stayed a
proposal (`architecture.md`'s sketch), never implemented. `engine_base.h`
instead defines `MAX_VOICES`'s companion pieces (`PROFILE_PIN`, `Waveform`,
`FilterMode`, `EffectParams`, `VoiceParamBlockT`/`ParamExchangeT` templates,
plus `FilterBusParams`/`FilterModel` for the chip module) — each engine's
own `VoiceParams` is still a from-scratch flat struct with no shared base
class, exactly as groovebox's is (§5.2's as-built note above).

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

**As-built, this tree differs in a few places:** the `engine_base.h` split
and `osc/metal.h` / `ladder.h` locations are exactly as proposed here. But
there is no `route.cpp` anywhere (subtractive or groovebox) — CMake selects
whichever engine's full `midi_controller.cpp` exists instead (see the
per-file-disposition note above); `clap.h` actually lives at top-level
`src/clap.h`, a shared-tier position, not under `engines/groovebox/`; and
there is no `drum.h` — the BD/tom/snare/hat/metal "voice recipes" are inline
`render_*` functions directly in groovebox's own `audio_engine.cpp` (§5.2),
not split into a separate header.

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

**As-built:** this shared-shell/routing-hook design was not what shipped —
see the per-file-disposition note above. The real selection mechanism is
simpler than this snippet: `CMakeLists.txt` checks whether
`src/engines/${T00T_ENGINE}/midi_controller.cpp` exists and uses it in full
if so, else falls back to `src/midi/midi_controller.cpp` — no routing-hook
header, no `route.cpp`, and (per the same note) `voice_alloc.cpp` is linked
for groovebox same as every non-tracker engine.

### Overlap summary

- **Shared verbatim:** ~75–80% (all DSP primitives, envelope/SVF/FX, output,
  samples, MIDI transports + parser, LCD).
- **Forked (small):** `engine.h` VoiceParams, `audio_engine.cpp` render,
  presets→kit, `route.cpp` note-routing ≈ 500–600 lines total.
  (as-built: no separate `route.cpp` — note-routing is part of the fully
  forked `midi_controller.cpp`, ~374 lines on its own; see above.)
- **Net-new (touches nothing existing):** `ladder.h`, `osc/metal.h`, `clap.h`,
  `drum.h`, groovebox `engine.h`/`kit.h`/render.
  (as-built: `clap.h` is shared-tier, not groovebox-only; no `drum.h` was
  added — see above.)
- **One-time refactor cost:** split `engine.h` → `engine_base.h` + per-engine
  `engine.h`; extract subtractive note-routing into `subtractive/route.cpp`. Both
  are mechanical and benefit the subtractive engine too (cleaner separation).
  (as-built: the `engine_base.h` split happened and is engine-agnostic today,
  used by every engine — but the second half didn't: `src/engines/subtractive/`
  has no `route.cpp` and no `midi_controller.cpp` of its own; it still uses
  the shared `src/midi/midi_controller.cpp` fallback with the poly allocator
  inline, unchanged from before this design.)

---

## 12. Summary — the actual new-code list

**New DSP:** `LadderFilter` (303) · second per-voice envelope · pitch-env modulator
· portamento glide · metal oscillator bank (N-square) · clap envelope · one-shot
decay-env mode. Drum "voices" (BD/SD/tom/clap/metal/rim) are *recipes* combining
existing oscillators + filter + envelopes, not new primitives.
(as-built: all of this is built and shipped, except rim — see §3.2/§6, no
`VT_DRUM_RIM` exists.)

**New structure:** `engines/groovebox/` with `VoiceType`-tagged union `VoiceParams`
and per-voice dispatch; groovebox note-routing (fixed voice map, mono 303 priority,
CH/OH choke, accent/slide) replacing the poly allocator; 808 + 909 kit tables.
(as-built: `VoiceParams` is a flat struct, not a union — see §5.2; CH/OH
"choke" is one shared voice slot, not a cross-voice envelope write — see §6;
only the 808 kit table exists, no 909 kit yet.)

**Reused unchanged:** all oscillators, sample player, SVF, LFO, ADSR core, delay +
reverb FX, ParamExchange IPC, MIDI transports, dual-core structure.

**909's metal voices and sample-trigger pads are essentially free** once the
sample voice type exists — the existing sample player already does the work.
