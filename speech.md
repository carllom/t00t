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
constexpr uint32_t MAX_SPEECH_VOICES = 8;  // #31 P2 profiling decision, was 4

enum SpeechMode : uint8_t {
    SPEECH_ONESHOT,   // utterance runs to completion, ignores note-off
    SPEECH_GATED,     // note-off jumps to release segment — DEFAULT for new presets, see #30
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
// Generated data — phonemes.h (src/engines/speech/phoneme_def.h, #32)
struct PhonemeDef {
    uint16_t F[SPEECH_FORMANTS];      // Hz
    uint16_t fric_F, nasal_F;          // Hz — parallel-branch targets (#29)
    uint8_t  B[SPEECH_FORMANTS];      // Hz / 4, range 0-1020
    uint8_t  fric_B, nasal_B;          // Hz / 4
    uint8_t  av, af, an;                // voiced, fricative, nasal amplitude (0-255)
    uint8_t  duration_ms;
    uint8_t  flags;                     // bit 0 PLOSIVE, bit 1 STOP_CLOSURE, bit 2 TRANSITION_FAST,
                                         // bit 3 SUSTAINABLE (reserved by #30, unread until P4 — see
                                         // "Singing mode" under Settled Decisions)
};
```

Extended by #32 from the sketch above (fric_F/fric_B/nasal_F/nasal_B added): this
struct predates #29's parallel fricative/nasal branches, which need a per-phoneme
fricative/nasal pole (e.g. /s/ vs /sh/'s `fric_F` is their whole distinguishing
feature) that the original 16-byte sketch had no field for.

26 bytes per phoneme (`sizeof(PhonemeDef)`, no padding — see phoneme_def.h). #32's
initial CSV-authored set is 48 phonemes, 1248 bytes; still negligible in flash at
double that. Utterances are byte strings of phoneme indices; a 20-word phrase is
well under 200 bytes.

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

### Singing mode / `SUSTAINABLE` (#30 decision)

**Yes, reserve the flag now.** Bit 3 of `PhonemeDef.flags` is `SUSTAINABLE`, marking
phonemes (the five vowels, and eventually the nasals) whose segment can be held open
under gate instead of advancing at its nominal duration. Nothing reads this bit until
the sequencer's `SPEECH_GATED`/singing behaviour lands in P4 — it is reserved, not
implemented, by this decision.

The reasoning is entirely about *when* the flag is cheap versus expensive, not about
whether singing mode itself is P3 or P4 work. Right now `PhonemeDef` exists only as
the struct definition above; no CSV, no generated header, no consumer. Reserving a
bit costs nothing. Once `tools/speechgen.py` exists and a phoneme CSV has been
hand-authored and is in use, adding a column means back-filling every row and
regenerating every downstream header — annoying, and exactly the kind of churn this
issue exists to avoid (see the issue's own framing: decide now, because it is much
cheaper than retrofitting).

This does not commit the sequencer to any particular singing-mode algorithm —
`SUSTAINABLE`'s only guarantee at this point is that a bit is available for it to use
when P4 gets there.

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

**CC map, settled by #36.** Five performance CCs plus the selection CCs they sit
alongside. "Live" pushes straight into every currently-held voice on the channel
(`midi_controller.cpp`'s per-CC loop over `voice_held[]`), same as CC21/22 already did;
"next note" only takes effect on the next `NOTE_ON`.

| CC | Name | Engine field | Live? |
|---|---|---|---|
| 1 | Mod wheel (GM standard) | `lfo_depth` (vibrato depth) | live |
| 10 | Pan (GM standard) | `pan` | next note |
| 16 | Preset select (#38) | `presets[]` (`SpeechPreset`, `presets.h`) | next note |
| 20 | Phoneme select | `phoneme` (#28 keyboard) | next note |
| 21 | Formant shift | `formant_shift` | live |
| 22 | Bandwidth scale | `bandwidth_scale` | live |
| 23 | Utterance/phrase select | `utterance` (0 = off/keyboard, 1..N = `SPEECH_PHRASES`) | next note |
| 24 | Rate | `rate` | live |
| 25 | Jitter | `jitter` | live |
| 26 | Shimmer | `shimmer` | live |
| 27 | Mode select | `mode` (3 bands: ONESHOT/GATED/LOOP) | live |
| 28 | Phrase-bank toggle | note number selects the phrase instead of PC/CC23 | next note |
| 76 | Vibrato rate (GM standard) | `lfo_rate` | live |

Program Change selects an utterance too (`ev.data1 % SPEECH_PHRASE_COUNT`), the same
value CC23 writes -- #36 repoints Program Change from phoneme selection (its #28/#29
job) to utterance selection, since a single PC message can't mean both and "pick a
phrase to speak" is the more natural reading of "patch change" for an utterance-based
instrument. Phoneme selection for the #28 keyboard is CC20-only from here on. CC16/
CC20-28 continue the same contiguous Arturia BeatStep Pro absolute-CC-encoder block
(CC16-31), for the reason given in `midi_controller.cpp`'s header comment; CC1 and
CC76 are real GM-standard assignments ("mod wheel" and "Vibrato Rate"), not
project-specific choices, following CC1/CC10's own precedent in the subtractive engine.

CC16 (#38, "Preset table" below) is deliberately next-note-only, same as CC20/CC23:
a preset can set `utterance`/`mode`/`phoneme`, and pushing those into an
already-sequencing voice would be exactly the kind of note-off-adjacent glitch #30's
release-segment mechanism exists to avoid. Loading a preset bulk-writes the same
per-channel state CC21/22/24-27 individually own (`midi_controller.cpp`'s
`speech_load_preset()`), so those CCs still work exactly as before -- they tweak away
from whatever the preset loaded rather than from a fixed power-on default, and
selecting a different preset overwrites those tweaks again.

### Voice lifetime and note-off (#30 decision)

An utterance has its own duration independent of gate. The `SpeechMode` field decides
what note-off means. This was settled before writing the sequencer (this issue was
P3's blocker) because it determines whether `seg_remaining` and the active-voice
bitmap can diverge.

The active-voice bitmap needs no change under any mode: hold the bit set until the
phoneme sequence completes, regardless of gate. `voice_alloc` reclaims on bitmap
clear, which is already correct behaviour. What changes per mode is *when* the
sequence counts as complete relative to note-off.

**Default for new presets: `SPEECH_GATED`.** Every other engine in this project
treats note-off as something a player can hear happen — the subtractive engine's
ADSR release, the groovebox's one-shot decay envelopes. A default that ignores
note-off (`SPEECH_ONESHOT`) would make the gate on a freshly-created speech preset
appear inert, which is the wrong first impression for an instrument whose whole
pitch is "playable, not just a talking clock" (see Scope). `SPEECH_LOOP` and
`SPEECH_HOLD` are both real modes but neither is a sane *default*: LOOP repeating
indefinitely while held is a specialised drone/vocoder behaviour a preset should opt
into, and HOLD sustains a single phoneme rather than sequencing an utterance at all,
so it doesn't apply to utterance presets. GATED is the one mode whose behaviour
matches what a player expects from "press a key, word plays; let go, word stops"
without actually clipping the utterance mid-formant.

**How GATED bounds `seg_remaining` against note-off.** Note-off does not zero
`seg_remaining` directly — that would cut the tract mid-coefficient-ramp, the same
click the resonator stability rule (see "Resonator and the stability rule") exists to
avoid. Instead it jumps `seg_index` to the utterance's designated release segment
(a phrase-data field, defaulting to the utterance's last segment when a phrase
doesn't mark one explicitly) and lets the normal per-segment clock run from there.
**Maximum time a voice may outlive its note-off is therefore bounded to the release
segment's own duration** — phrase authoring should keep release segments short
(same order as a plosive burst, tens of ms, not a glide), but the mechanism itself
guarantees termination: once the release segment's `seg_remaining` reaches 0 with no
further segment queued, the bitmap clears and `voice_alloc` reclaims exactly as it
does today.

`SPEECH_ONESHOT` and `SPEECH_LOOP` bound the same way, for consistency:
- **ONESHOT** ignores note-off entirely for audio purposes; the bitmap clears
  whenever `seg_remaining` naturally reaches 0, gate state notwithstanding.
- **LOOP** ignores note-off *within* a pass, but does not start a new pass after
  it: reaching the end of the utterance while gate is already low degrades to
  ONESHOT completion instead of looping again. This caps LOOP's note-off overhang
  at one utterance length, not unbounded repetition.

**A new note-on for a voice still finishing an utterance always wins immediately.**
This is not a new policy — it is `tract_retrigger()`'s existing rule (snap to the
new phoneme's targets, no glide, reset filter/phase state) applied at the utterance
level instead of the phoneme level. The alternative — queuing the note-on until the
old utterance's release segment finishes — would make a fast retrigger on a 4-voice
polyphonic instrument feel like the busiest voice is stuck, which is a worse failure
mode than clipping a release tail. It's the same tradeoff the tracker's underrun
policy already makes: prefer a clean, audible cut over blocking on stale state (see
"Underrun policy").

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

**#32 landed the first half**: `speechgen.py gen` parses `tools/speech_phonemes.csv`
(48 phonemes — vowels, fricatives, nasals, plosive closure/burst pairs, affricates,
approximants) into `phonemes.h`, failing loudly on any row that would wrap a
`uint8_t`/`uint16_t` rather than truncating silently. Verification reuses
`tools/host_render/render_speech.cpp`: it renders every row to a WAV in one command,
and for the vowels, cross-checks the measured F1/F2 against
`tools/host_render/vowel_reference.h` (generated from `speech_vowel_reference.csv`,
an *independently*-committed published-values table — comparing a measurement
against the same number that authored it proves nothing).

**#35 landed the second half**: `speechgen.py gen-phrases` parses a plain-text
phrase list (`tools/speech_phrases.txt`, `NAME: word word word` per line) into
`phrases.h` (`enum SpeechPhraseId`, `SPEECH_PHRASES[]` — reusing `sequencer.h`'s
`SpeechUtterance` struct, same shape as `utterance.h`'s hand-picked fixtures — and
`SPEECH_PHRASE_TEXT[]` for debugging/WAV-naming). Each word runs through
`tools/nrl_rules.py`, a host-side NRL-style (Elovitz et al. 1976) letter-to-sound
engine: ordered, context-sensitive rules per starting letter, tried longest-pattern-
first, plus a short whole-word exception list for irregular high-frequency function
words (THE, OF, ONE, ...). It is a curated few dozen rules, not the original
report's ~400 — deliberately: a word the rules get wrong is fixed with an attached
override (`machine{M AX SH I N}`, replacing just that word's pronunciation while
keeping "machine" as its spelled form in `SPEECH_PHRASE_TEXT[]`) rather than by
growing the rule table to chase one exception, per this section's own long-standing
guidance. Every emitted phoneme symbol (rule-derived or overridden) is checked
against `speech_phonemes.csv`'s own symbol column before `phrases.h` is written, so
a typo fails the build instead of emitting an out-of-range phoneme index. No rules
engine, rule table, or text parsing links into the device build — only the
generated phoneme-byte arrays do, same host/device split as `phonemes.h`.

Verification extends `render_speech.cpp` the same way #32 did: `run_phrase_renders()`
renders every entry in `SPEECH_PHRASES[]` to a WAV in one command (checking the audio
is finite, non-clipping, and that the sequencer actually reaches completion — not
that the *pronunciation* is correct, which no host check can verify). The demo bank
(`tools/speech_phrases.txt`) is 6 phrases / 121 bytes in flash (73 phoneme bytes +
48 bytes of `SPEECH_PHRASES[]` table). **Blind intelligibility spot-check: pending —
needs a human listener** (this repeats #32's own acknowledgment that a host check
can confirm a phrase renders, not that it sounds like the intended word); the WAVs
land in `tools/host_render/build/speech_phrase_*.wav` for whoever does that listen.

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

**Measured (#31, engine.md "Speech Engine P2 Profiling"): ~93.5 cycles/frame/voice
(2.75%), flat from 1 to 8 voices** — about 25–55% over this table's top end, all of
it in the excitation/resonator/ZOH bucket rather than coefficient recompute (which
landed close to predicted). See engine.md for the full breakdown and the
`MAX_SPEECH_VOICES = 8` decision under Settled Decisions below.

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
rather than on the table above. **Done (#31):** ~93.5 cycles/frame/voice measured,
`MAX_SPEECH_VOICES = 8`.

### P3 — Segment sequencer

Per-voice segment clock inside the sub-block loop. Variable per-segment duration.
Linear F/B ramping. Plosive closure/burst handling. Host tool generating `phonemes.h`
from a CSV.

*Exit:* a hardcoded phoneme string is intelligible as a word. **Done (#34):**
`sequencer.h` implements the per-voice segment clock exactly as sketched above
(`k = min3(frames left, seg_remaining, SPEECH_SUBBLOCK)`, moved inside
`speech_render_voice_seq()`'s per-voice loop, render.h). Two hand-picked utterances
("HELLO", "CAT" — `utterance.h`, not generated; text-to-phoneme stays P4) exercise
variable per-segment duration, F/B ramping across a phoneme boundary, and the
plosive closure/burst pair. See "Settled Decisions" below for the scope this
landed with vs. the full sketch in "Data Structures".

### P4 — Utterances and MIDI integration

Phrase table, `SpeechMode` policies, program change selection, CC map, vibrato LFO,
jitter/shimmer. Host tool generating `phrases.h` from text.

*Exit:* a MIDI sequence plays a sung phrase at correct pitch. **Done (#36):**
`audio_engine.cpp` now sequences `phrases.h`'s generated `SPEECH_PHRASES[]` (#35)
instead of `utterance.h`'s two P3 fixtures; Program Change and CC23 both select a
phrase, and CC28's phrase-bank mode maps note number to phrase directly ("playing the
words themselves" vs. Program Change's "playing a line"). `SpeechMode`'s three values
were already implemented by #34; #36 adds a live CC27 mode select so all three (plus
the structurally-represented `SPEECH_HOLD`, unchanged from #28) are reachable without a
rebuild. `rate`/`jitter`/`shimmer` (CC24-26) and the vibrato LFO (CC1/CC76) are new --
see "MIDI Mapping"'s CC table above for the full settled map and `excitation.h` for the
DSP (vibrato resampled once per sub-block; jitter/shimmer drawn once per glottal cycle,
detected by phase-accumulator wraparound, not per sample). `utterance.h`'s HELLO/CAT
fixtures stay in place for `tools/host_render/render_speech.cpp`'s #34-era regression
checks, which need their exact known phoneme strings, not whatever `speech_phrases.txt`
currently says. Host-verified (`render_speech.cpp`): jitter/shimmer at zero measure as
exactly periodic (0.0 coefficient of variation on both cycle period and peak amplitude)
and visibly perturbed at max; vibrato produces a measurable, sub-block-rate F0 swing;
a generated phrase's measured pitch tracks three notes spanning two octaves. MIDI-layer
correctness (Program Change/CC parsing, phrase-bank note mapping) has no host-side
harness -- `midi_controller.cpp` *does* have a pico-sdk dependency (`voice_alloc`/
`midi_parser`), unlike `render.h` -- so it is compile-verified (both `ENGINE=speech`
and `ENGINE=speech SPEECH_PROFILE=1` build clean) but not yet confirmed by ear on real
`breadboard_rp2350` hardware, including the four-`SpeechMode` "verified by ear" bullet
this issue's acceptance criteria call for. `PHONEME_FLAG_SUSTAINABLE` (#30) stays
reserved but unread -- true singing mode (holding a vowel segment open under gate
instead of advancing at its nominal duration) is a real gap against the sketch's
"Singing mode" note, but it isn't in #36's own acceptance criteria, so it's deferred
rather than silently dropped.

### P5 — Effects and polish

Route through the existing delay/reverb/overdrive chain. Stereo placement per voice.
Preset table.

*Exit:* module is playable and ships. **Landed (#38):** delay/reverb linking and
per-voice pan (`pan` field) turned out to already be in place -- both shipped with
the #27 skeleton and #11's stereo-output work respectively, before any P3/P4
sequencer or MIDI-mapping code existed, and both are already hardware-verified (#28's
closing note: "notes, pan, vowel select, both delay and reverb" confirmed on
`breadboard_rp2350`). Overdrive was never actually part of "the existing effects
chain" anywhere in the codebase -- checked: `EffectType` (`engine_base.h`) only ever
had `FX_DELAY`/`FX_REVERB`; "delay/reverb/overdrive" traces back to an *Open
Question* in `tracker.md`, not a built feature -- so it's explicitly deferred rather
than implemented here; see "Settled Decisions" below for the reasoning against
building it as part of this issue. What #38 actually added: `presets.h`
(`SpeechPreset`, `voice_apply_preset()`, 9 presets -- one per `SpeechMode`
(`SPEECH_HOLD` via the phoneme keyboard, `ONESHOT`, `GATED`, `LOOP`), robotic,
breathy, tract-shift up/down, and a robot-chorus preset spreading up to
`MAX_SPEECH_VOICES` simultaneously-held notes across the stereo field with a small
per-voice detune -- see "MIDI Mapping" above for the exit criterion this closes:
`speech.md`'s own "the range the parameters actually cover"), CC16 preset select
(`midi_controller.cpp`), and a CMake fix (`CMakeLists.txt`'s VGA-board button
controller was gated on "does this engine ship a presets.h" -- true only by accident
for the subtractive engine until this issue gave speech one too; re-gated on
`T00T_ENGINE STREQUAL "subtractive"`, what the controller's actual `VoicePreset`/
`WAVE_SAMPLE` dependency always required). Builds clean on all four engines
(`make`/`make ENGINE=groovebox`/`make ENGINE=tracker`/`make ENGINE=speech`, plus
`SPEECH_PROFILE=1`). Not yet done: hardware listening confirmation of the preset
table (particularly the robot-chorus preset's stereo spread) and the profiling-pin
measurement of effects-on cost for `engine.md` -- both need Carl at the bench, same
as #36's MIDI-wiring gap.

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
- [x] Default `SpeechMode` for new presets is `SPEECH_GATED` (#30): note-off jumps to
      a bounded release segment rather than being ignored (`ONESHOT`) or requiring an
      opt-in loop/hold mode. See "Voice lifetime and note-off (#30 decision)"
- [x] `SUSTAINABLE` flag reserved as bit 3 of `PhonemeDef.flags` (#30): cheap now
      while the table is docs-only, expensive to retrofit once a CSV exists. Not read
      until P4. See "Singing mode / `SUSTAINABLE` (#30 decision)"
- [x] `PhonemeDef` extended with `fric_F`/`fric_B`/`nasal_F`/`nasal_B` (#32): the
      original 16-byte sketch predates #29's parallel branches and had no field for
      a per-phoneme fricative/nasal pole. 26 bytes/phoneme now; still negligible.
      See "Generated data — phonemes.h" above.
- [x] `MAX_SPEECH_VOICES = 8` (#31): measured ~93.5 cycles/frame/voice (2.75%),
      flat from 1 to 8 voices on real `breadboard_rp2350` hardware, with both the
      fricative branch and a live formant_shift/bandwidth_scale sweep costing
      nothing extra over that flat per-voice number (both are already
      unconditional in the code). 8 voices is 22% of Core 1 alone, 30% with
      reverb on top — comfortably inside budget, and makes a "robot chorus" preset
      a real option for P5. See engine.md "Speech Engine P2 Profiling (#31)" for
      the full breakdown and the discrepancy explanation against this doc's
      predicted 60-75 cycles/frame/voice budget.
- [x] Segment sequencer landed scoped to what P3's exit criterion actually needs
      (#34), deferring the rest of "Data Structures"' `VoiceParams`/`SpeechMode`
      sketch to P4:
      - `SpeechMode` has three values (`SPEECH_MODE_ONESHOT/GATED/LOOP`), not the
        sketch's four. `SPEECH_HOLD` is represented structurally instead —
        `VoiceParams::utterance == SPEECH_NO_UTTERANCE` routes a voice through the
        unchanged #28 phoneme-keyboard path (`speech_render_voice()`) rather than
        through the sequencer at all, since HOLD has no segments to advance
        through. `sequencer.h` has the full reasoning.
      - Utterances are `utterance.h`'s hand-picked `SPEECH_UTTERANCES` table
        (phoneme strings only, no phrase text), not a generated `phrases.h` —
        letter-to-sound and program-change phrase selection are P4 "Utterances and
        MIDI integration" work, unchanged from this doc's phasing.
      - `VoiceParams` gained exactly `utterance`/`mode`/`rate`; `EnvConfig`,
        `jitter`, `shimmer`, `lfo_rate`/`lfo_depth` are still P4 — no sequencer
        claim depends on them.
      - `rate` (Q4.4, `VoiceParams::rate`) exists and is exercised by the segment
        clock, but isn't CC-mapped yet — MIDI CC23 (`midi_controller.cpp`) picks
        an utterance for on-device testing; the full CC map (rate/jitter/shimmer)
        stays P4.
      - Note-off (#30) is implemented for all three modes, not just the
        `SPEECH_GATED` default: `SPEECH_MODE_LOOP` degrades to `ONESHOT`
        completion once gate drops rather than starting a new pass, and
        `SPEECH_MODE_ONESHOT` ignores note-off outright, matching #30's spec for
        both.
      - Malformed/empty utterance data renders silence rather than dereferencing
        it (this doc's "Underrun policy" extended to sequencer data, not just
        DMA/tick inconsistency) — `tools/host_render/render_speech.cpp` verifies
        by deliberately constructing one.
- [x] Letter-to-sound (#35) is a curated few dozen rules plus a whole-word exception
      list, not the ~400-rule original NRL report — the override syntax
      (`word{SYM SYM ...}`) is the intended fix for anything it gets wrong, so the
      rule table only grows when a real phrase needs it. `phrases.h` reuses
      `sequencer.h`'s `SpeechUtterance` struct (same shape as `utterance.h`'s
      hand-picked fixtures, not a new type) — device-side selection (program change,
      CC map) is unchanged, still P4. See "Host Tooling" above.
- [x] Program Change now selects an utterance, not a phoneme (#36): a single PC
      message can't mean both, and CC20 already covers phoneme selection for
      controllers (this project's BeatStep Pro) that can't send real PC anyway. This
      changes #28's original PC behaviour but doesn't remove any capability — phoneme
      selection is still fully reachable, just CC20-only from here on.
      See "MIDI Mapping"'s CC table.
- [x] Phrase-bank mode (CC28, #36) is a per-channel toggle, not a separate `SpeechMode`
      value: it changes *which utterance* a note-on selects (note number instead of
      PC/CC23), not how note-off behaves, so it composes with all four `SpeechMode`
      values rather than being one itself. `utterance = note % SPEECH_PHRASE_COUNT` —
      no base-note offset/config, since "reachable without a rebuild" only requires
      that every phrase be reachable from *some* note, not a specific keyboard layout.
- [x] Vibrato (#36) is resampled once per sub-block, matching engine.md's "recompute
      coefficients from ramped parameters, not per sample" convention this module
      already applies to F/B — not per glottal cycle (that's jitter's job) and not
      per output sample (unnecessary cost for something this slow relative to audio
      rate). Depth is semitones (`VIBRATO_MAX_SEMITONES`, excitation.h), the same
      "musical, not raw-Hz" mapping every other pitched/timbral CC in this engine uses.
- [x] Jitter/shimmer (#36) are drawn once per glottal cycle, detected by phase-
      accumulator wraparound in `render.h`'s inner sample loop, not once per sample
      and not on a separate timer: a real glottis can't change its period mid-pulse,
      and per-sample randomisation would read as FM/AM noise riding the pulse train
      rather than the period-to-period wobble that makes a voice sound less
      mechanically perfect. `jitter == shimmer == 0` skips the LFSR draw entirely
      (not just multiplies by zero), so a jitter/shimmer-off render stays bit-exact
      with pre-#36 behaviour — verified by `render_speech.cpp`'s zero-crossing-period
      check measuring exactly 0.0 coefficient of variation at the default settings.
- [x] Overdrive deferred, not built as part of #38 (speech.md P5 "Effects and
      polish"): #38's own wording ("route through the existing delay/reverb/
      overdrive chain ... nothing about it needs rework, just wiring") assumed
      overdrive was already a member of the shared post-mix effects chain
      (`EffectType`, `engine_base.h`) the way delay/reverb are. It never was —
      grepping the whole repo, "overdrive" only ever appears (a) as an *Open
      Question* in `tracker.md` ("the existing delay/reverb/overdrive could run as
      a stereo send" — itself hypothetical) and (b) as the groovebox's unrelated
      per-voice 303 ladder-filter `drive` parameter, a different mechanism with the
      same name. Adding a real `FX_OVERDRIVE` would mean extending `EffectType`
      and CC74's band-select in the *shared* `engine_base.h`, which changes CC74's
      behaviour for the subtractive and groovebox engines too, not just speech —
      a cross-engine feature, not speech-specific wiring. Decided with Carl:
      defer it and document the gap here rather than build a shared three-engine
      feature inside a speech-branch issue. Delay and reverb — the two effects
      that actually exist — were already linked (#27) and are unchanged by #38.
- [x] Preset table (#38, "Preset table" above): `SpeechPreset`/`voice_apply_preset()`
      (`presets.h`) follow the subtractive engine's `presets.h` shape, but with one
      structural difference forced by speech having *live* per-field CCs
      (formant_shift/bandwidth_scale/jitter/shimmer/rate/mode, CC21/22/24-27) for
      several fields a preset also sets — something the subtractive engine's
      presets don't contend with, since none of its preset-owned fields have their
      own live CC. Applying a preset directly to a fresh voice at every note-on
      (the subtractive engine's exact pattern) would mean those per-field CCs stop
      affecting future notes the moment a preset was selected once, silently
      regressing #29/#36's already-hardware-verified "CC tweak also becomes the new
      per-channel default" behaviour. Resolved by having preset *selection*
      (CC16, next-note-only) bulk-write the same per-channel state those CCs
      individually own, routed through `voice_apply_preset()` via a scratch
      `VoiceParams` so the preset-to-field mapping is still written in exactly one
      place — not by having every note-on re-derive from the preset table
      directly. See "MIDI Mapping"'s CC16 paragraph above for the full reasoning.
      Robot chorus (per-voice pan spread + small detune, keyed by the allocated
      voice slot, deterministic, no new state) is the "if #31 allowed more than
      four voices" payoff the Scope section named — #31 raised `MAX_SPEECH_VOICES`
      to 8, so it's in the table (`PRESET_ROBOT_CHORUS`).
- [x] `src/controller.cpp` (the VGA-board 3-button demo) was gated in
      `CMakeLists.txt` on "does this engine's directory contain a `presets.h`" —
      true only for the subtractive engine until #38 gave speech one too, which
      pulled the demo into the speech build and broke it (the demo is hardcoded to
      subtractive's `VoicePreset`/`WAVE_SAMPLE`/`osc_sample_phase_inc`, not a
      generic preset-button interface). Re-gated on `T00T_ENGINE STREQUAL
      "subtractive"` — what the demo's actual code dependency always was. No
      behaviour change for any existing engine (subtractive still gets it,
      groovebox and tracker still don't — they never shipped a `presets.h`
      either); speech now correctly doesn't, matching its buttonless
      `breadboard_rp2350` target (`HAS_BUTTONS 0`) same as the groovebox.

---

## Open Questions

1. **Coarticulation depth.** v1 does simple linear F/B ramping between adjacent
   segment targets. Proper transition rules (target undershoot, locus equations)
   need lookahead to the *following* phoneme. Deferred — but note the monophonic
   ordered-ring variant gets this almost free.
2. **Display.** What does the LCD show for a speech module? Current phoneme and a
   formant-space plot is the obvious answer and is cheap at ~10 Hz redraw, but it is
   low priority.
3. **#38 hardware verification: pending.** `make ENGINE=speech` (and `SPEECH_PROFILE=1`)
   build clean and `make`/`make ENGINE=groovebox`/`make ENGINE=tracker` are confirmed
   unaffected by the `CMakeLists.txt` re-gate, but nothing in #38 has been heard on
   real `breadboard_rp2350` hardware yet: the preset table (all 9 `presets[]` entries,
   particularly `PRESET_ROBOT_CHORUS`'s per-voice pan/detune spread through the
   PCM5122's two channels) and CC16 preset-select's "switchable live without
   glitching an in-flight utterance" claim (true by construction — `speech_load_
   preset()` never writes `shadow.voices[]`, so it cannot touch an already-sounding
   voice — but not yet confirmed by ear). Also pending: the effects-on cost
   measurement for `engine.md`'s performance table (needs the profiling pin, i.e.
   Carl at the bench, same as #31/#37's numbers).
