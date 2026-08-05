# T00T — Speech Synthesis Module (Implementation Plan)

Companion to `architecture.md` and `tracker.md`. This document assumes the tracker
module has been built first and that its infrastructure work is already merged.

---

## Prerequisites

This plan depends on work landing in earlier modules. None of it is speech-specific.

| Prerequisite | Source | Status |
|---|---|---|
| Option A per-engine `VoiceParams` + CMake engine selection | `architecture.md` | required |
| Sub-block rendering with boundary-cut `min()` | tracker | required |
| Duration carried per control block, not as a global | tracker | required |
| True stereo output stage; `pan` in `VoiceNoteBase` | tracker | required |
| Host-side data converter tooling pattern | tracker | reused, not required |
| `res2p.h` two-pole resonator in common layer | see Backporting | required |

If the tracker is not yet done, the two hard blockers are sub-block rendering and
stereo output. Everything else can be worked around.

---

## Scope

**Build the formant engine first. Treat LPC as a sibling engine that shares two of
its three blocks.**

The decomposition:

```
excitation  →  vocal tract filter  →  resample  →  (existing effects chain)
      ↑                ↑
      └── segment sequencer (sub-audio rate)
```

The excitation source, the segment sequencer, and the resampler are method-agnostic.
Only the tract filter differs — a cascade of resonators for formant, a lattice for
LPC. Building formant first means the LPC engine is one new file plus a data pipeline.

**Why formant first:**

- Parametric. All state is formant frequency/bandwidth, so tract length, formant
  shift and bandwidth become MIDI-controllable. It is a playable instrument, not
  just a talker.
- Data-light. ~64 phonemes × ~16 bytes of targets plus transition rules speaks
  arbitrary text in real time.
- No encoder pipeline. LPC needs either an encoder (`BlueWizard`, `python_wizard`)
  or an allophone-indexed LPC set before it can say anything new.
- No IP baggage. Lifting TI ROM data has real licensing problems; own-recorded
  LPC encoding does not, but it is a separate project.

---

## Architecture Placement

```
src/engines/speech/
  engine.h        VoiceParams, SpeechSegment, engine constants
  engine.cpp      render pass, sub-block loop, per-voice segment clock
  tract.h         formant resonator cascade + coefficient computation
  excitation.h    glottal pulse train, LFSR noise, jitter/shimmer
  sequencer.h     per-voice segment advance, target interpolation
  phonemes.h      GENERATED — phoneme target table
  phrases.h       GENERATED — utterance phoneme strings
  presets.h       SpeechPreset, voice_apply_preset(), preset table

src/res2p.h       COMMON — two-pole resonator (shared with groovebox)
src/noise.h       COMMON — LFSR noise (shared with groovebox/subtractive)
```

### Control plane: latest-wins, unchanged

**The speech module keeps the standard `ParamExchange` and `voice_alloc`.** It does
*not* adopt the tracker's ordered TickBlock ring. This is a deliberate divergence
and the reason matters:

The tracker has **one global tick clock**. Polyphonic speech has **N independent
segment clocks** — voice 0 is three phonemes into one utterance while voice 1 has
just started another. Four independent ordered rings with four boundary counters is
not a generalisation of the tracker IPC, it is a different mechanism wearing its
clothes.

Instead, the segment sequencer runs on Core 1 inside the render pass, per voice.
This is affordable: per-segment work is a table read plus coefficient computation,
roughly 1000 cycles at ~100 Hz, i.e. ~0.07% of a core per voice. Phoneme tables are
small enough to live permanently in SRAM, so the XIP-thrash argument that pushed the
XM player to Core 0 does not apply.

`VoiceParams` therefore carries only genuinely latest-wins-safe fields — utterance
id, pitch, formant shift, rate — which is exactly what that mechanism is good at.

**Exception:** if a monophonic-only speech build is wanted later, the tracker's
ordered ring fits perfectly, and its one-block lookahead becomes *coarticulation*
lookahead (formant targets legitimately depend on the following phoneme). Worth
noting as a future option; not the v1 target.

### Sub-block cut point moves inside the voice loop

The tracker computes `k = min(frames_left, tick_remaining, SUBBLOCK)` once globally.
Speech needs it per voice:

```cpp
for (int v = 0; v < active_voices; ++v) {
    SpeechVoice &sv = voices[v];
    uint32_t n = 0;
    while (n < frames) {
        if (sv.seg_remaining == 0) sequencer_advance(sv);      // next phoneme
        uint32_t k = min3(frames - n, sv.seg_remaining, SUBBLOCK);
        tract_update_coeffs(sv);                               // ramp F/B, recompute
        render_voice(accum + n, sv, k);
        n += k;
        sv.seg_remaining -= k;
    }
}
```

Voice-major rendering already suits this — the tracker's `mix_voice(accum, n)` shape
carries over directly.

---

## Timing Domains

At 44.1 kHz output, 150 MHz, speech core running at 22.05 kHz native:

| Domain | Period | Work |
|---|---|---|
| Segment | 10–200 ms, **variable per phoneme** | Advance phoneme, load new F/B/amp targets, set duration |
| DMA buffer | 256 frames (5.8 ms) | IRQ, buffer swap, wake Core 1 |
| Sub-block | ≤64 frames (1.45 ms) | Ramp F/B toward target, recompute resonator coefficients |
| Sample | 1 frame | Excitation, N resonator passes, accumulate |

### Segment duration must be per-block

In the tracker, `samples_per_tick` lives in the block rather than as a global because
of `Fxx` tempo changes — real but occasional. **In speech this is mandatory, not an
optimisation.** Phoneme durations differ by design: a plosive burst is ~10 ms, a
diphthong glide 150 ms+. Votrax phoneme durations vary by roughly 5× across the set.
Every segment carries its own duration.

`SpeechSegment.duration_samples` is computed at segment-advance time from the
phoneme's nominal duration scaled by the voice's `rate` parameter.

### Underrun policy

Inherited from the tracker: on any inconsistency, render **silence**, not stale
parameters. Stale parameters here mean a frozen vowel — characterful, but far harder
to spot on the profiling pin than a dropout.

---

## Data Structures

```cpp
// engines/speech/engine.h

constexpr uint32_t SPEECH_RATE      = 22050;   // native core rate, SAMPLE_RATE / 2
constexpr uint32_t SPEECH_FORMANTS  = 5;       // F1..F5
constexpr uint32_t MAX_SPEECH_VOICES = 4;

enum SpeechMode : uint8_t {
    SPEECH_ONESHOT,   // utterance runs to completion, ignores note-off
    SPEECH_GATED,     // note-off jumps to release segment
    SPEECH_LOOP,      // utterance repeats while gate held
    SPEECH_HOLD,      // single phoneme, sustained while gate held (phase 1)
};

struct VoiceParams : VoiceNoteBase {   // phase_inc = glottal pitch, amplitude, gate, trigger
    EnvConfig env;
    uint16_t  utterance;        // index into phrase table
    uint8_t   phoneme;          // used when mode == SPEECH_HOLD
    SpeechMode mode;
    uint8_t   rate;             // Q4.4 scale on all segment durations, 1.0 = nominal
    int16_t   formant_shift;    // Q8.8 multiplier on all F, tract length / "gender"
    int16_t   bandwidth_scale;  // Q8.8, low = resonant/robotic, high = breathy
    uint8_t   jitter;           // pitch-period randomisation, 0 = perfectly robotic
    uint8_t   shimmer;          // amplitude randomisation
    float     lfo_rate, lfo_depth;   // vibrato on glottal pitch
};
```

`formant_shift` is the field that makes this an instrument rather than a talking
clock. Expose it on a CC by default.

```cpp
// Per-voice render state, Core 1 only, never crosses ParamExchange
struct SpeechVoice {
    Res2p    formant[SPEECH_FORMANTS];
    Res2p    nasal;
    Res2p    fric;
    uint32_t seg_remaining;       // samples until next segment
    uint16_t seg_index;           // position within utterance
    uint16_t utterance;
    float    F[SPEECH_FORMANTS], B[SPEECH_FORMANTS];        // current, ramped
    float    F_tgt[SPEECH_FORMANTS], B_tgt[SPEECH_FORMANTS];// segment target
    float    F_step[SPEECH_FORMANTS], B_step[SPEECH_FORMANTS]; // per sub-block delta
    float    av, af, an;          // voiced / fricative / nasal amplitudes
    uint32_t glottal_phase;
    uint32_t noise_state;         // LFSR
    uint8_t  resample_acc;        // ZOH counter, 22.05k -> 44.1k
};
```

```cpp
// Generated data — phonemes.h
struct PhonemeDef {
    uint16_t F[SPEECH_FORMANTS];  // Hz
    uint8_t  B[SPEECH_FORMANTS];  // Hz / 4, range 0-1020
    uint8_t  av, af, an;          // voiced, fricative, nasal amplitude (0-255)
    uint8_t  duration_ms;
    uint8_t  flags;               // PLOSIVE, STOP_CLOSURE, TRANSITION_FAST
};
```

16 bytes per phoneme. A 64-phoneme set is 1 KB. Utterances are byte strings of
phoneme indices; a 20-word phrase is well under 200 bytes.

---

## DSP Detail

### Native rate: 22.05 kHz, ZOH ×2

Formant F5 sits around 4.5 kHz, so 11.025 kHz (Nyquist 5.5 kHz) is uncomfortably
tight. 22.05 kHz is exactly `SAMPLE_RATE / 2`, which makes the resampler a bare
integer doubling — no fractional accumulator, no phase drift, and the ZOH imaging is
a genuine part of the lo-fi character rather than a defect.

Halving the render rate also halves per-voice cost, which is where the budget below
comes from.

(LPC at 8 kHz native *does* need a fractional accumulator: 44100/8000 = 5.5125. That
lands in the LPC phase, not here.)

### Resonator and the stability rule

**Never interpolate biquad coefficients directly.** Walking between two phonemes'
coefficient sets can push poles outside the unit circle and produce a burst of noise.

The rule for this engine: **ramp F and B per sub-block, recompute coefficients from
them per sub-block, never interpolate the coefficients themselves.**

```cpp
// res2p.h — common layer
struct Res2p { float a1, a2, b0, s1, s2; };

static inline void res2p_set(Res2p &r, float f, float bw, float fs) {
    float rr    = res2p_radius(bw, fs);          // exp(-PI*bw/fs)
    float theta = 2.0f * (float)M_PI * f / fs;
    r.a1 = -2.0f * rr * cosf(theta);
    r.a2 = rr * rr;
    r.b0 = 1.0f + r.a1 + r.a2;                   // unity DC gain
}

static inline float res2p_tick(Res2p &r, float x) {
    float y = r.b0 * x - r.a1 * r.s1 - r.a2 * r.s2;
    r.s2 = r.s1; r.s1 = y;
    return y;
}
```

`res2p_radius()` should avoid `expf`: for bandwidths in the 50–400 Hz range the
radius sits in a narrow 0.97–0.993 band, so a 32-entry LUT with linear interpolation
or a two-term series is indistinguishable and much cheaper.

Sub-block rate is the right place for this. At ~345 Hz (64 frames at 22.05 kHz),
7 `cosf` per sub-block per voice is roughly 2.4k transcendentals/sec/voice — fast
enough that coarticulation sounds continuous, cheap enough to disappear in the noise.

### Cascade vs parallel

Use a **cascade** for the voiced path (F1→F2→F3→F4→F5). It gets relative formant
amplitudes right automatically from the bandwidths, which is what makes vowels sound
natural without per-formant amplitude data. Use a **parallel** branch for fricatives
and the nasal pole, mixed in by `af` and `an`.

This is the Klatt split, reduced. Do not attempt a full Klatt implementation — the
cascade plus one fricative resonator plus one nasal pole covers everything the SC-01
could do and most of what it could not.

### Excitation

```
voiced:     glottal pulse train at phase_inc, with jitter and shimmer
unvoiced:   LFSR noise (shared noise.h with groovebox hats/snare)
mixed:      both, scaled by av and af (voiced fricatives: /z/, /v/, /ʒ/)
```

A two-slope triangular pulse (rising ramp, faster falling ramp) is enough — the
spectral tilt matters more than the exact shape, and a Rosenberg or LF model buys
nothing at this fidelity target. Expose the open-quotient as a preset field if you
want breathy/pressed voice quality.

`jitter` and `shimmer` at zero give the perfectly periodic, unmistakably robotic
1978 sound. Non-zero gives something closer to human. Both are useful; make it a
front-panel control.

### Plosives

Plosives need a silent closure segment followed by a short burst. The `PLOSIVE` and
`STOP_CLOSURE` flags in `PhonemeDef` drive this: closure sets `av = af = 0` for its
duration, burst is a short high-`af` segment with `TRANSITION_FAST` so the formant
ramp is near-instant rather than glided. Without this, plosives sound like fricatives
and intelligibility collapses.

---

## MIDI Mapping

**Phase 1 (`SPEECH_HOLD`):** one MIDI note = one sustained phoneme. `phase_inc` sets
glottal pitch, gate sustains, phoneme selected by preset. A "phoneme keyboard". This
is how people actually play SC-01 chips as instruments, and it needs zero sequencer
work — it drops straight into the existing note-per-voice model.

**Phase 2 (utterances):** note-on triggers an utterance; note pitch sets the glottal
pitch it is spoken/sung at. Utterance selected by program change, or by note number
in a dedicated "phrase bank" preset mode.

**Phase 3 (optional):** arbitrary text over sysex, converted on-device by an NRL
letter-to-sound rules engine (~400 rules, 6–8 KB). Defer this — a compile-time phrase
table generated by the host tool covers most use cases.

Recommended CC map: formant shift, bandwidth scale, rate, jitter, shimmer. These five
are what make it performable.

### Voice lifetime and note-off

An utterance has its own duration independent of gate. The `SpeechMode` field decides
what note-off means; **this must be settled before writing the sequencer** because it
determines whether `seg_remaining` and the active-voice bitmap can diverge.

The active-voice bitmap needs no change: hold the bit set until the phoneme sequence
completes, regardless of gate. `voice_alloc` reclaims on bitmap clear, which is
already correct behaviour.

---

## Host Tooling

Follows the tracker converter pattern. A Python tool, `tools/speechgen.py`:

- Input: a phoneme target table (CSV, hand-editable) and a phrase list (plain text
  with optional explicit phoneme override syntax).
- Letter-to-sound: NRL rules run **on the host**, so the device never needs them.
- Output: `phonemes.h` and `phrases.h` as `const` arrays in flash.
- Verification: renders each phrase through a host build of the same synth core and
  writes WAVs for listening, so phrase data can be iterated without a reflash.

Keeping the rules engine host-side is the same decision as keeping the XM loader
host-side, and for the same reason.

---

## Performance Budget

Per voice, per output frame at 44.1 kHz (150 MHz M33, single-precision FPU):

| Item | Cost |
|---|---|
| 7 resonator passes @ 22.05 kHz | ~35–50 cycles/output frame |
| Excitation (pulse + LFSR + jitter) | ~10 cycles/output frame |
| ZOH ×2 upsample + accumulate | ~5 cycles/output frame |
| Coefficient recompute, amortised | ~10 cycles/output frame |
| **Total** | **~60–75 cycles/output frame** |

Budget is 3401 cycles/frame at 44.1 kHz. That is **~2% per voice**, comparable to one
aliased-engine voice and cheaper than a subtractive voice. Four speech voices ≈ 8% of
Core 1, leaving the rest for effects.

Do not trust these numbers. The tracker taught that estimated per-voice costs can be
wrong by a factor of five in either direction depending on surrounding machinery.
**Measure with the profiling pin at the end of P2 before scaling voice count.**

Memory: phoneme table ~1 KB, phrases <4 KB, per-voice state ~200 bytes. Negligible
against the tracker's sample RAM pressure.

---

## Phased Plan

### P0 — Common component extraction

Land `res2p.h` and `noise.h` in the common layer. Wire `res2p.h` into the groovebox
first (see Backporting) — it is independently verifiable there and it means the
resonator is proven before any speech code exists.

*Exit:* 808 toms and congas rebuilt on `res2p.h`, output diffed against previous
build, no audible change.

### P1 — Phoneme keyboard (`SPEECH_HOLD`)

Static formant targets, no sequencer, no transitions. One note = one sustained
phoneme. Cascade + excitation only; skip the fricative and nasal branches.

*Exit:* the five cardinal vowels are recognisable as vowels when played from a
keyboard, and pitch tracks note number.

### P2 — Full tract + profiling

Add fricative branch, nasal pole, mixed excitation, formant shift and bandwidth
scale as live parameters. Measure with the profiling pin.

*Exit:* measured per-voice cost, and a decision on `MAX_SPEECH_VOICES` based on it
rather than on the table above.

### P3 — Segment sequencer

Per-voice segment clock inside the sub-block loop. Variable per-segment duration.
Linear F/B ramping. Plosive closure/burst handling. Host tool generating `phonemes.h`
from a CSV.

*Exit:* a hardcoded phoneme string is intelligible as a word.

### P4 — Utterances and MIDI integration

Phrase table, `SpeechMode` policies, program change selection, CC map, vibrato LFO,
jitter/shimmer. Host tool generating `phrases.h` from text.

*Exit:* a MIDI sequence plays a sung phrase at correct pitch.

### P5 — Effects and polish

Route through the existing delay/reverb/overdrive chain. Stereo placement per voice.
Preset table.

*Exit:* module is playable and ships.

### P6 — LPC sibling engine (optional, later)

New tract filter only: a 10-stage lattice replacing the resonator cascade. Excitation,
sequencer, resampler and control plane are unchanged. Native rate drops to 8 kHz,
which introduces the fractional resampler.

Note the convergence: the TMS5220 interpolated its reflection coefficients eight times
per 25 ms frame — every 25 samples at 8 kHz. That is literally sub-block rendering,
decided in 1978 for the same reasons. The lattice is also unconditionally stable under
coefficient interpolation as long as |k| < 1, which is *why* TI used it, and means the
P3 ramping code can interpolate lattice coefficients directly where it could not
interpolate biquad coefficients.

Data pipeline is the real work here, not DSP. Encode own recordings; do not lift ROMs.

---

## Testing

There is no `openmpt123` equivalent — no canonical reference renderer for a formant
synth. Compensate with:

- **Host build of the same core.** Same source, compiled for the host, rendering to
  WAV. Diff against device output to separate DSP bugs from embedded bugs (fixed-point
  errors, buffer-boundary errors, DMA races). This is the single most valuable test
  and should exist from P1.
- **Coefficient sanity assertions.** After every `res2p_set()`, assert `a2 < 1.0f` in
  debug builds. Catches the interpolation-instability class immediately.
- **Vowel formant chart.** Plot F1/F2 of sustained vowels from rendered output against
  published vowel-space values. Objective, and catches coefficient errors that the ear
  forgives.
- **Intelligibility spot-check.** Rendered phrases, listened to blind by someone who
  does not know what they say. Crude but it is the actual success criterion.

---

## Backporting to Other Modules

Every item below is a component this module needs anyway; the point is to land them
where they are independently verifiable.

### To the groovebox (808/909/303)

**`res2p.h` — highest value, do this in P0.**
The 808's toms, congas, rimshot and cowbell are all ringing two-pole resonators.
Whatever they currently use, replacing it with the shared `res2p.h` gets the resonator
proven against a sound you already know, before any speech code depends on it. A
resonator's impulse response is easy to verify objectively.

**`noise.h` LFSR.**
Shared with hats and snare. Consolidate whatever the groovebox uses today; the speech
fricative branch wants the same generator and the same seeding behaviour.

**Formant cascade as a filter option — "talking acid".**
Once the cascade exists, routing a 303 saw through it with two or three formants gives
a vowel/talkbox filter. Costs nothing new: it is `res2p.h` plus a small vowel-target
table already generated by `speechgen.py`. Morphing between vowel targets on an LFO or
the accent envelope is an obvious and very cheap feature.

**Jitter/shimmer as analog drift.**
The excitation jitter model is a generic "randomise this parameter slightly per cycle"
primitive. Applied to 808 oscillator pitch and 303 envelope timing it produces the
unit-to-unit variation that makes analog emulations feel less rigid.

### To the subtractive engine

**Formant filter mode.**
Add `FILTER_FORMANT` to `FilterMode` alongside LP/BP/HP/notch, backed by the same
cascade. Vowel-morphing pads are a well-worn but genuinely good sound, and this is
close to free once P2 lands.

**Coefficient-recompute-per-sub-block as the standard pattern.**
The tracker backport already moves LFO, ADSR and SVF `F_half` to sub-block rate. The
speech module adds the rule that makes it safe generally: recompute coefficients from
ramped *parameters* rather than interpolating the coefficients. Worth writing into
`architecture.md` as a project-wide convention, because the SVF has the same latent
instability at high resonance with fast cutoff sweeps.

**Bandwidth as a first-class parameter.**
`bandwidth_scale` is a more musical control than resonance for anything resonant.
Consider exposing it on the SVF too.

### Already inherited from the tracker

Listed for completeness; no new work.

- Sub-block rendering skeleton
- Per-block duration rather than a global
- True stereo output and `pan` in `VoiceNoteBase`
- Underrun renders silence, not stale parameters
- Host-side data converter pattern

---

## Settled Decisions

- [x] Formant engine first; LPC as a sibling sharing excitation + sequencer + resampler
- [x] Latest-wins `ParamExchange` retained; the tracker's ordered ring is *not* adopted
- [x] Segment sequencer runs on Core 1, per voice, inside the render pass
- [x] Sub-block cut point moves inside the voice loop
- [x] Native rate 22.05 kHz, ZOH ×2 to output — integer, no fractional accumulator
- [x] Cascade for voiced path, parallel branch for fricative + nasal
- [x] Ramp F/B per sub-block, recompute coefficients from them; never interpolate
      coefficients
- [x] Segment duration carried per segment, not global
- [x] Letter-to-sound rules run on the host; device ships a phrase table
- [x] `res2p.h` lands in the groovebox first, before any speech code depends on it

---

## Open Questions

1. **Note-off policy default.** Which `SpeechMode` is the sensible default for a new
   preset? Affects how the active-voice bitmap and `seg_remaining` interact and is
   much cheaper to decide before P3 than to retrofit.
2. **Voice count.** Four is the working assumption. Confirm after P2 profiling; the
   answer may be higher than expected, in which case a "robot chorus" preset with
   per-voice detune and stereo spread becomes attractive.
3. **Coarticulation depth.** v1 does simple linear F/B ramping between adjacent
   segment targets. Proper transition rules (target undershoot, locus equations)
   need lookahead to the *following* phoneme. Deferred — but note the monophonic
   ordered-ring variant gets this almost free.
4. **Display.** What does the LCD show for a speech module? Current phoneme and a
   formant-space plot is the obvious answer and is cheap at ~10 Hz redraw, but it is
   low priority.
5. **Singing mode.** Should sustained vowels hold indefinitely under gate while
   consonants pass at normal rate? That is how a vocoder-style "sung" utterance works
   and it needs a per-phoneme `SUSTAINABLE` flag. Cheap to add in P3 if decided early.
