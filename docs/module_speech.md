# T00T — Speech Synthesis Module

A formant (Klatt-reduced) speech synthesis engine: a phoneme keyboard,
utterance/phrase playback, and a reserved-but-unimplemented singing mode.
See `engine.md` for the shared dual-core architecture; `architecture.md`
for the cross-engine `VoiceParams`/CMake pattern this module follows;
`history_speech.md` for build-phase results and full performance
measurements.

## Overview

Speaks phonemes and short phrases through a formant resonator cascade,
playable as an instrument (formant shift, bandwidth, rate, vibrato/jitter/
shimmer are all live MIDI controls) rather than just a talking clock.

### Specifications

- **Voices**: 8, standard dynamic allocation (`voice_alloc`), latest-wins
  `ParamExchange` — not the tracker's ordered tick ring (see Architecture
  for why)
- **Synthesis**: formant cascade (5 resonators, F1–F5) for the voiced path,
  plus a parallel fricative resonator and a parallel nasal pole, mixed by
  per-segment amplitude weights
- **Excitation**: glottal pulse train (two-slope triangular) for voicing,
  LFSR noise for frication, both together for voiced fricatives; jitter
  (pitch-period randomisation) and shimmer (amplitude randomisation), both
  live and both off by default (perfectly periodic)
- **Native render rate**: 22.05 kHz (`SAMPLE_RATE / 2`), zero-order-hold
  ×2 to the shared 44.1 kHz output — an exact integer doubling, no
  fractional resampler
- **Modes** (`SpeechMode`, per voice): `SPEECH_HOLD` (phoneme keyboard —
  represented structurally, not as a `SpeechMode` value, see Architecture),
  `ONESHOT`, `GATED` (default), `LOOP`
- **Data**: 48 phonemes (`phonemes.h`, generated from a hand-authored CSV),
  a 6-phrase demo bank (`phrases.h`, generated from plain text via a
  letter-to-sound engine); two hand-picked fixtures (`utterance.h`) used
  only by host regression tests
- **Presets**: 9 factory presets (`presets.h`) — phoneme keyboard,
  one-shot/gated/looped phrase examples, robotic and breathy timbres,
  tract-length shift up/down, and a robot-chorus preset spreading up to 8
  simultaneously-held voices across the stereo field with per-voice detune
- **Effects**: shared post-mix insert (delay or reverb) — no overdrive (see
  Future/TODO)
- No LPC sibling engine yet (optional future work — the excitation,
  sequencer, and control plane are designed to be reused by one)

### MIDI Mapping (Input Capabilities)

Notes trigger voices through the standard allocator; pitch sets glottal
frequency. CC assignments follow the Arturia BeatStep Pro's fixed
absolute-mode 16-encoder block (CC16–31); CC1 and CC76 are the real
GM-standard "mod wheel" and "vibrato rate" assignments, matching the
subtractive engine's own CC1/CC10 precedent.

| CC | Name | Field | Live? |
|---|---|---|---|
| 1 | Mod wheel (GM) | Vibrato depth | live |
| 10 | Pan (GM) | Pan | next note |
| 16 | Preset select | Loads a `presets[]` entry (bulk-writes the CCs below) | next note |
| 20 | Phoneme select | Phoneme keyboard index | next note |
| 21 | Formant shift | Tract length / "gender" | live |
| 22 | Bandwidth scale | Resonance / breathiness | live |
| 23 | Utterance/phrase select | Which phrase (0 = off, phoneme keyboard) | next note |
| 24 | Rate | Segment-duration scale | live |
| 25 | Jitter | Pitch-period randomisation | live |
| 26 | Shimmer | Amplitude randomisation | live |
| 27 | Mode select | `SpeechMode` (3 live bands: ONESHOT/GATED/LOOP) | live |
| 28 | Phrase-bank toggle | Note number selects the phrase directly, instead of Program Change/CC23 | next note |
| 76 | Vibrato rate (GM) | LFO rate | live |

**Program Change** selects an utterance (same value CC23 writes) —
phoneme selection is CC20-only. "Live" CCs push directly into every
currently-held voice on the channel, not just the per-channel default for
future notes.

### Display (Presentation Capabilities)

A per-voice phoneme grid (one cell per active voice) plus an F1/F2
formant-space plot oriented like a conventional IPA vowel chart, redrawn
at ~10 Hz. Both are driven by each voice's live, ramped formant values —
the same numbers the tract renders from — so a plotted dot moves
continuously as a segment glides, not just when it steps.

## Technical Overview

### Source Layout

```
src/engines/speech/
  engine.h          VoiceParams, MAX_VOICES, engine constants
  audio_engine.h    audio_engine_run()/audio_engine_load() declarations
  audio_engine.cpp  render pass entry point, effects mix, profiling rig
  render.h          per-voice render core (phoneme-keyboard + sequenced
                     paths) — no pico-sdk dependency, shared with host tooling
  tract.h           formant resonator cascade + coefficient computation,
                     SpeechVoice state
  excitation.h      glottal pulse train, jitter/shimmer/vibrato
  sequencer.h       per-voice segment advance, SpeechMode
  phoneme_def.h     PhonemeDef struct, phoneme_unpack()
  phonemes.h        GENERATED — phoneme target table
  phrases.h         GENERATED — utterance phoneme strings
  utterance.h       two hand-picked fixtures (HELLO/CAT), used only by
                     host regression checks
  presets.h         SpeechPreset, voice_apply_preset(), preset table
  midi_controller.cpp  MIDI parsing, CC map, program change
  display.cpp       LCD status, per-voice phoneme grid, F1/F2 formant plot
```

Also draws on common-layer files this module motivated: `src/res2p.h`
(two-pole resonator — see Future/TODO for its still-pending groovebox
backport) and `src/osc/noise.h` (LFSR noise, shared with the groovebox).

### Build

Build with `make ENGINE=speech`. `SPEECH_PROFILE=1` builds an alternate
`audio_engine_run()` that replaces the normal MIDI-driven loop with a
self-cycling, pin-only measurement rig (no display, no stdio) — see
`history_speech.md` for what it measured.

### Tools

`tools/speechgen.py` — host-side generator, no dedicated `README.md`:

```
speechgen.py gen           # tools/speech_phonemes.csv -> phonemes.h
speechgen.py gen-phrases   # tools/speech_phrases.txt  -> phrases.h
```

`tools/nrl_rules.py` — a curated, host-only NRL-style (Elovitz et al.
1976) letter-to-sound engine used by `gen-phrases`: ordered,
context-sensitive rules per starting letter, tried longest-pattern-first,
plus a short whole-word exception list. A word the rules mispronounce is
fixed with an inline override (`machine{M AX SH I N}`) rather than by
growing the rule table. Neither the rules engine nor any text parsing
links into the device build — only the generated phoneme-byte arrays do.

`tools/host_render/render_speech.cpp` (via `make host`) renders every
phoneme and every phrase to a WAV in one command, and cross-checks vowel
F1/F2 against `tools/host_render/vowel_reference.h` (an independently
sourced published-values table).

## Architecture

### Control Plane: Latest-Wins, Not the Tracker's Ordered Ring

This module keeps the standard `ParamExchange` and `voice_alloc` — it does
not adopt the tracker's ordered `TickBlock` ring. The tracker has one
global tick clock; polyphonic speech has up to 8 independent segment
clocks (voice 0 three phonemes into one utterance while voice 1 has just
started another), so an ordered ring generalizes badly here — four
independent rings with four boundary counters would be a different
mechanism wearing the tracker's clothes, not a reuse of it.

Instead, the segment sequencer runs on Core 1 inside the render pass, per
voice: per-segment work is a table read plus coefficient computation,
cheap enough (see Performance) that this is affordable, and the phoneme
tables are small enough to live permanently in SRAM, so the XIP-thrash
concern that pushed the tracker's player to Core 0 doesn't apply here.
`VoiceParams` therefore carries only genuinely latest-wins-safe fields —
utterance id, pitch, formant shift, rate, and so on — which is exactly
what that mechanism is good at.

If a monophonic-only build were ever wanted, the tracker's ordered ring
would fit, and its one-block lookahead would double as *coarticulation*
lookahead (formant targets legitimately depend on the following phoneme) —
noted as a possible future option, not pursued.

### Sub-Block Cut Point Is Per-Voice

The tracker computes its sub-block cut point once, globally, per buffer.
Speech needs it per voice, since each voice's segment clock is
independent:

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

### Timing Domains

At 44.1 kHz output, 150 MHz, speech core running at 22.05 kHz native:

| Domain | Period | Work |
|---|---|---|
| Segment | 10–200 ms, variable per phoneme | Advance phoneme, load new F/B/amp targets, set duration |
| DMA buffer | 256 frames (5.8 ms) | IRQ, buffer swap, wake Core 1 |
| Sub-block | ≤64 frames (1.45 ms) | Ramp F/B toward target, recompute resonator coefficients |
| Sample | 1 frame | Excitation, N resonator passes, accumulate |

Segment duration is carried per segment, not as a global, for the same
reason the tracker carries `samples_per_tick` per tick block — except here
it's mandatory rather than an occasional correction: phoneme durations
differ by design (a plosive burst is ~10 ms, a diphthong glide 150 ms+).

**Underrun policy**, inherited from the tracker: on any inconsistency,
render silence, not stale parameters. A frozen vowel is far harder to spot
on the profiling pin than a dropout.

### Data Structures

```cpp
// engines/speech/engine.h — latest-wins over ParamExchange
struct VoiceParams {
    uint32_t   phase_inc;         // Q32 glottal phase increment, at SPEECH_RATE
    int16_t    amplitude;
    uint8_t    trigger;
    bool       gate;
    int16_t    pan;                // Q15
    uint8_t    phoneme;             // phoneme keyboard index (phonemes.h)
    int16_t    formant_shift;       // Q8.8, 256 = 1.0x
    int16_t    bandwidth_scale;     // Q8.8, 256 = 1.0x
    uint8_t    utterance;           // SPEECH_NO_UTTERANCE routes through the
                                     // phoneme keyboard instead of the sequencer
    SpeechMode mode;
    uint8_t    rate;                // Q4.4 segment-duration scale, 16 = 1.0x
    uint8_t    jitter, shimmer;     // 0-255, 0 = perfectly periodic
    float      lfo_rate, lfo_depth; // vibrato on glottal pitch
};
```

There is no `EnvConfig`/`Envelope` field — unlike every other engine's
voices, gate on/off runs through `render.h`'s own exponential `cur_amp`
smoothing, a simpler declick filter rather than the shared ADSR model.

```cpp
// tract.h — per-voice render state, Core 1 only, never crosses ParamExchange
struct SpeechVoice {
    Res2p    formant[SPEECH_FORMANTS];
    Res2p    fric, nasal;
    float    F[SPEECH_FORMANTS], B[SPEECH_FORMANTS];         // current, ramped
    float    F_tgt[SPEECH_FORMANTS], B_tgt[SPEECH_FORMANTS]; // segment target
    float    fric_F, fric_B, fric_F_tgt, fric_B_tgt;
    float    nasal_F, nasal_B, nasal_F_tgt, nasal_B_tgt;
    float    av, af, an, av_tgt, af_tgt, an_tgt;              // branch amplitudes
    float    formant_shift, formant_shift_tgt;
    float    bandwidth_scale, bandwidth_scale_tgt;
    uint32_t glottal_phase;
    uint16_t noise_lfsr;
    float    cur_amp;              // smoothed toward gate target, declicks on/off
    uint8_t  last_trigger, last_phoneme;  // force a retrigger/target-load on first render
    float    lfo_phase;            // vibrato LFO phase, 0..1
    uint32_t glot_cycle_inc;       // this glottal cycle's jittered phase increment
    float    glot_cycle_amp;       // this glottal cycle's shimmer amplitude multiplier
    uint16_t jitter_lfsr;          // separate seed from noise_lfsr
    uint16_t seg_index;            // position within the current utterance
    uint32_t seg_remaining;        // native samples left in the current segment
    bool     seq_done;             // utterance complete; voice still allocated but silent
    bool     gate_prev;            // previous call's gate, to find the note-off edge
    bool     active;               // still sounding
};
```

`F`/`B` are ramped toward `_tgt` once per sub-block by a fixed coefficient
(`tract_advance_subblock()`), not by a precomputed per-sub-block delta —
see Resonator and the Stability Rule for why coefficients are never
interpolated directly. Vibrato/jitter/shimmer state resets on retrigger,
not on a plain segment/phoneme change — it tracks the *note*, the same
lifetime as `glottal_phase`.

```cpp
// phoneme_def.h — generated data, 26 bytes/phoneme, no padding
struct PhonemeDef {
    uint16_t F[SPEECH_FORMANTS];   // Hz
    uint16_t fric_F, nasal_F;       // Hz — parallel-branch targets
    uint8_t  B[SPEECH_FORMANTS];   // Hz / 4, range 0-1020
    uint8_t  fric_B, nasal_B;       // Hz / 4
    uint8_t  av, af, an;             // voiced, fricative, nasal amplitude (0-255)
    uint8_t  duration_ms;
    uint8_t  flags;                  // bit 0 PLOSIVE, bit 1 STOP_CLOSURE,
                                      // bit 2 TRANSITION_FAST, bit 3 SUSTAINABLE
                                      // (reserved, unread — see Future/TODO)
};
```

48 phonemes at 26 bytes is 1248 bytes in flash. Utterances are byte
strings of phoneme indices; the demo phrase bank is 121 bytes total (73
phoneme bytes + 48 bytes of table).

### Native Rate: 22.05 kHz, ZOH ×2

Formant F5 sits around 4.5 kHz, so 11.025 kHz (Nyquist 5.5 kHz) would be
uncomfortably tight. 22.05 kHz is exactly `SAMPLE_RATE / 2`, which makes
the resampler a bare integer doubling — no fractional accumulator, no
phase drift, and the zero-order-hold imaging is a genuine part of the
lo-fi character rather than a defect. Halving the render rate also halves
per-voice cost.

### Resonator and the Stability Rule

Never interpolate biquad coefficients directly — walking between two
phonemes' coefficient sets can push poles outside the unit circle and
produce a burst of noise. The rule this engine follows: ramp F and B per
sub-block, recompute coefficients from them per sub-block, never
interpolate the coefficients themselves.

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

`res2p_radius()` avoids `expf`: for the 50–400 Hz bandwidth range the
radius sits in a narrow 0.97–0.993 band, cheap enough to approximate with
a small LUT or a short series instead.

### Cascade vs Parallel

A cascade (F1→F2→F3→F4→F5) for the voiced path gets relative formant
amplitudes right automatically from the bandwidths, which is what makes
vowels sound natural without per-formant amplitude data. A parallel branch
handles fricatives and the nasal pole, mixed in by `af` and `an`. This is
the Klatt model, reduced to a cascade plus one fricative resonator plus
one nasal pole — deliberately not a full Klatt implementation.

### Excitation

```
voiced:     glottal pulse train at phase_inc, with jitter and shimmer
unvoiced:   LFSR noise (shared noise.h with groovebox hats/snare)
mixed:      both, scaled by av and af (voiced fricatives: /z/, /v/, /ʒ/)
```

The pulse is a two-slope triangular shape (rising ramp, faster falling
ramp) — spectral tilt matters more than exact shape at this fidelity
target. `jitter` and `shimmer` at zero give a perfectly periodic,
deliberately robotic sound; nonzero moves toward something closer to
human.

Vibrato is resampled once per sub-block (matching the same "recompute from
ramped parameters, not per sample" rule used for F/B), not per glottal
cycle and not per output sample. Jitter and shimmer are drawn once per
glottal cycle instead, detected by phase-accumulator wraparound in the
inner sample loop — a real glottis can't change its period mid-pulse, and
per-sample randomisation would read as noise riding the pulse train rather
than period-to-period wobble. When both are zero the LFSR draw is skipped
entirely (not just multiplied by zero), so an off render stays bit-exact
with the un-jittered path.

### Plosives

Plosives need a silent closure segment followed by a short burst. The
`PLOSIVE`/`STOP_CLOSURE` flags in `PhonemeDef` drive this: closure sets
`av = af = 0` for its duration; the burst is a short high-`af` segment
flagged `TRANSITION_FAST` so the formant ramp is near-instant rather than
glided. Without this, plosives sound like fricatives and intelligibility
collapses.

### Voice Lifetime and Note-Off

An utterance has its own duration independent of gate; `SpeechMode`
decides what note-off means. The active-voice bitmap needs no
mode-specific handling: it holds set until the phoneme sequence completes,
regardless of gate, and `voice_alloc` reclaims on bitmap clear as usual.
What changes per mode is *when* the sequence counts as complete relative
to note-off:

- **`SPEECH_MODE_GATED`** (default): note-off does not zero
  `seg_remaining` directly (that would cut the tract mid-coefficient-ramp
  — the same click the stability rule above exists to avoid). Instead it
  jumps `seg_index` to the utterance's designated release segment
  (defaulting to the utterance's last segment when one isn't marked
  explicitly), and the normal per-segment clock runs from there. Maximum
  time a voice may outlive its note-off is bounded to the release
  segment's own duration.
- **`SPEECH_ONESHOT`**: ignores note-off entirely for audio purposes; the
  bitmap clears whenever `seg_remaining` naturally reaches 0.
- **`SPEECH_MODE_LOOP`**: ignores note-off within a pass, but does not
  start a new pass after it — reaching the end of the utterance while gate
  is already low degrades to one-shot completion, capping the note-off
  overhang at one utterance length.
- **`SPEECH_HOLD`** is not a fourth `SpeechMode` value — it's represented
  structurally: `VoiceParams::utterance == SPEECH_NO_UTTERANCE` routes a
  voice through the unchanged phoneme-keyboard render path
  (`speech_render_voice()`) instead of the sequencer at all, since HOLD
  has no segments to advance through.

A new note-on for a voice still finishing an utterance always wins
immediately — the existing `tract_retrigger()` rule (snap to the new
target, no glide, reset filter/phase state) applied at the utterance level
instead of the phoneme level, same tradeoff as the tracker's underrun
policy: prefer a clean, audible cut over blocking on stale state.

### Singing Mode Reservation

Bit 3 of `PhonemeDef.flags` is `SUSTAINABLE`, reserved for phonemes (the
five vowels, eventually the nasals) whose segment could be held open under
gate instead of advancing at its nominal duration. Nothing reads this bit
yet — see Future/TODO.

## Status and Plan

### Performance

~93.5 cycles/frame/voice (2.75%), flat from 1 to 8 voices, on real
`breadboard_rp2350` hardware — the fricative/nasal branches and a live
formant-shift/bandwidth-scale sweep cost nothing extra over that flat
number (both already run unconditionally). 8 voices alone: 22% of Core 1;
with reverb (mutually exclusive with delay): ~30%. Full measurement
breakdown: `history_speech.md`.

### Future / TODO

- **LPC sibling engine** (optional) — a lattice-filter tract replacing the
  resonator cascade, sharing excitation/sequencer/control-plane unchanged.
  Native rate would drop to 8 kHz, introducing a fractional resampler.
- **Singing mode** — the `SUSTAINABLE` flag is reserved (see Architecture)
  but nothing reads it yet; holding a vowel segment open under gate
  instead of advancing at its nominal duration is unbuilt.
- **Coarticulation depth** — segment transitions are simple linear F/B
  ramps between adjacent targets; proper transition rules (target
  undershoot, locus equations) need lookahead to the *following* phoneme,
  deferred.
- **`res2p.h` groovebox backport** — the decided order was to land
  `res2p.h` in the groovebox first, before any speech code depended on
  it, so the resonator would be proven against a known sound. That
  backport never happened: speech's own work went ahead without waiting
  on it, and the groovebox's toms/congas still use their own
  pitch-envelope generator, not `res2p.h`. Open gap, not a design
  question.
- **Overdrive effect** — deferred. Adding a real overdrive would mean
  extending the shared `EffectType` (`engine_base.h`) and CC74's
  band-select for every engine, not speech-specific wiring; out of scope
  for a speech-only change.
- **Hardware verification pending** on several already-built features: the
  preset table by ear (particularly `PRESET_ROBOT_CHORUS`'s stereo
  spread), CC16 preset-select's glitch-free-switching claim, MIDI-layer
  correctness (Program Change/CC parsing, phrase-bank note mapping — only
  compile-verified so far), and the profiling-pin measurement of
  effects-on cost.

## Decision Record

1. **Formant engine built first; LPC deferred as a sibling** that shares
   excitation, sequencer, and resampler — formant synthesis is
   parametric (MIDI-controllable tract length/shift/bandwidth) and
   data-light (no encoder pipeline, no ROM licensing concerns), where LPC
   needs either an encoder or an allophone-indexed set before it can say
   anything new.
2. **Latest-wins `ParamExchange` kept; the tracker's ordered ring is not
   adopted** — polyphonic speech has independent per-voice segment
   clocks, not one global tick clock (see Architecture).
3. **Segment sequencer runs on Core 1, per voice, inside the render
   pass** — affordable at measured cost, and phoneme tables are small
   enough to stay resident in SRAM, so the XIP-thrash concern that moved
   the tracker's player to Core 0 doesn't apply here.
4. **Coefficients are recomputed from ramped F/B every sub-block, never
   interpolated directly** — see Resonator and the Stability Rule.
5. **`MAX_VOICES = 8`** — measured per-voice cost is flat across the
   whole tested range, and 8 voices plus reverb lands comfortably under
   budget. See `history_speech.md` for the measurement.
6. **Default `SpeechMode` for new presets is `GATED`**, not `ONESHOT` —
   every other engine in this project treats note-off as audible (ADSR
   release, one-shot decay envelopes); a mode that ignores note-off by
   default would make a fresh preset's gate appear inert, the wrong first
   impression for an instrument meant to be played, not just triggered.
   `LOOP` and `HOLD` are both real modes but neither suits a default: LOOP
   repeating indefinitely is a specialised drone behaviour a preset should
   opt into, and HOLD doesn't sequence an utterance at all.
7. **`SUSTAINABLE` reserved as a flag bit now, before any phoneme CSV
   exists** — cheap to reserve while `PhonemeDef` is still just a struct
   definition with no data or consumers; expensive to retrofit once a CSV
   and generated headers are in active use (a new column means
   back-filling every row and regenerating every downstream header).
8. **Letter-to-sound is a curated set of rules plus a whole-word
   exception list**, not the ~400-rule original NRL report — a mispronounced
   word gets an inline override rather than growing the rule table to
   chase one exception.
9. **Program Change selects an utterance, not a phoneme** — a single PC
   message can't mean both, and CC20 already covers phoneme selection for
   controllers (this project's BeatStep Pro) that can't reliably send
   real Program Change anyway.
10. **Phrase-bank mode (CC28) is a per-channel toggle, not a `SpeechMode`
    value** — it changes which utterance a note-on selects, not how
    note-off behaves, so it composes with all modes instead of being one.
11. **Preset selection (CC16) bulk-writes the same per-channel state the
    live CCs individually own**, rather than every note-on re-deriving
    from the preset table directly — applying a preset straight to a
    fresh voice at every note-on (the subtractive engine's pattern) would
    make live CC tweaks stop affecting future notes the moment a preset
    was selected once, since several preset-owned fields (formant shift,
    bandwidth, jitter, shimmer, rate, mode) also have their own live CC —
    something the subtractive engine's presets never have to contend
    with.
12. **The VGA-board button demo (`src/controller.cpp`) is gated on
    `T00T_ENGINE STREQUAL "subtractive"`**, not on "does this engine ship
    a `presets.h`" — the demo is hardcoded to the subtractive engine's own
    preset/sample types, so gating on presets.h's mere existence
    incorrectly pulled it into the speech build once speech got its own
    preset table.

## Glossary

- **Formant**: a resonant frequency band of the vocal tract; vowels are
  distinguished mainly by their first two or three formants' frequencies.
- **Phoneme**: the smallest distinct sound unit this engine synthesizes —
  one row of target formant/amplitude/duration data.
- **Segment**: one phoneme's sounding duration within a sequenced
  utterance; the sequencer advances one segment at a time.
- **Utterance**: a sequence of phonemes (a word or phrase) played by the
  segment sequencer.
- **Glottal**: relating to the vocal folds — the voiced excitation source
  ("glottal pulse train").
- **Jitter / shimmer**: cycle-to-cycle randomisation of the glottal pulse
  period (jitter) or amplitude (shimmer); zero gives a perfectly periodic,
  deliberately robotic sound.
- **Cascade / parallel**: two ways of chaining resonators — cascade feeds
  one resonator's output into the next (used for the voiced formant
  path); parallel sums separate resonators' outputs (used for the
  fricative and nasal branches).
- **Coarticulation**: how a phoneme's realized sound is influenced by
  its neighbours; this engine currently only ramps linearly between
  adjacent targets (see Future/TODO).
