# T00T — Speech Synthesis Module

A formant (Klatt-reduced) speech synthesis engine: a phoneme keyboard,
utterance/phrase playback, a reserved-but-unimplemented singing mode, an
LPC lattice sibling tract playing back real TMS5220 chip-speech words, and
a S.A.M.-style sibling tract with a converted allophone table but no
reciter or MIDI addressing of its own yet. See `engine.md` for the shared
dual-core architecture; `architecture.md`
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
- **Presets**: 10 factory presets (`presets.h`) — phoneme keyboard,
  one-shot/gated/looped phrase examples, robotic and breathy timbres,
  tract-length shift up/down, a robot-chorus preset spreading up to 8
  simultaneously-held voices across the stereo field with per-voice detune,
  and one LPC lattice preset selecting the corpus's `KEY_PER_WORD` page 0
- **Effects**: shared post-mix insert (delay or reverb) — no overdrive (see
  Future/TODO)
- **LPC lattice tract**: a second, sibling tract — a 10th-order all-pole
  lattice filter reusing the same glottal-pulse/LFSR-noise excitation,
  selected per voice by `VoiceParams::tract`. Renders natively at 8 kHz
  (the TMS5220's real frame rate) and reaches the shared 44.1 kHz output
  through its own fractional-ratio linear-interpolation resampler, unlike
  the formant tract's exact-integer zero-order-hold doubling.
  `KEY_PER_WORD` addressing (note selects a word, Program Change selects a
  128-word page) reaches the full converted Talkie corpus once
  `tools/talkie2lattice.py` has been run locally; without a generated
  corpus, every lattice voice plays the one hardcoded fixture
  (`lattice.h`'s `LATTICE_TEST_WORD`). `SpeechMode` (GATED/ONESHOT/LOOP)
  and a live pitch-shift override apply to lattice voices the same as
  formant voices — see LPC Lattice Tract and MIDI Mapping.
- **SAM tract**: a third, sibling tract — three formant resonators driven
  and summed independently (parallel, not cascaded), reusing the same
  glottal-pulse/LFSR-noise excitation, selected per voice by
  `VoiceParams::tract`. Renders at the same native rate and zero-order-hold
  doubling as the formant tract. No reciter or MIDI word-addressing scheme
  exists yet: every SAM voice plays one sustained allophone, indexed the
  same way the formant tract's phoneme keyboard is, from the generated
  allophone table (`tools/sam2allophones.py`) once converted locally, or a
  small hardcoded fixture (`sam.h`'s `SAM_TEST_ALLOPHONES`) otherwise — see
  SAM Tract and MIDI Mapping.

### MIDI Mapping (Input Capabilities)

Notes trigger voices through the standard allocator; pitch sets glottal
frequency, velocity sets amplitude (CC15 can disable this per channel, see
below). CC assignments follow the Arturia BeatStep Pro's fixed
absolute-mode 16-encoder block (CC16–31); CC1 and CC76 are the real
GM-standard "mod wheel" and "vibrato rate" assignments, matching the
subtractive engine's own CC1/CC10 precedent. CC15 sits just outside that
encoder block.

| CC | Name | Field | Live? |
|---|---|---|---|
| 1 | Mod wheel (GM) | Vibrato depth | live |
| 10 | Pan (GM) | Pan | next note |
| 15 | Velocity toggle | 0-63 = every note at max velocity, 64-127 = use received velocity (default) | next note |
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
| 102 | Tract select | Formant / LPC lattice / SAM, 3 bands — see LPC lattice tract and SAM Tract | next note |
| 103 | LPC pitch-shift multiplier | Q8.8 override on a word's own recorded pitch contour, 0.5x–2x | live |
| 104 | LPC excitation select | Shared `glottal_pulse()`/noise (<64) vs. the TMS5220's own chirp table + LFSR, paired (≥64) | live |

**Program Change** is tract-dependent: under the formant tract it selects
an utterance (same value CC23 writes) — phoneme selection is CC20-only;
under the LPC lattice tract it selects the `KEY_PER_WORD` page instead
(see LPC Lattice Tract). The two meanings never collide, since only one
tract is active per channel at a time; the SAM tract has no Program-Change
meaning of its own yet. "Live" CCs push directly into every currently-held
voice on the channel, not just the per-channel default for future notes.
CC102–119 is a reserved block for tract-specific controls, disjoint from
the formant tract's CC16–28; CC103/104 are LPC-specific, and CC20's
existing phoneme-select band also reaches the SAM tract's own smaller
fixture (wrapped, not a dedicated CC of its own yet).

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
  render.h          per-voice render core (phoneme-keyboard + sequenced +
                     LPC-lattice + SAM paths) — no pico-sdk dependency,
                     shared with host tooling
  tract.h           formant resonator cascade + coefficient computation;
                     SpeechVoice state (shared fields + the FormantVoiceState/
                     LatticeVoiceState/SamVoiceState union)
  lattice.h         LPC lattice tract: reflection-coefficient filter,
                     per-voice state, hardcoded test word
  sam.h             SAM tract: three parallel formant resonators + frication
                     branch, per-voice state, hardcoded bring-up fixture
  excitation.h      glottal pulse train, jitter/shimmer/vibrato
  sequencer.h       per-voice segment advance, SpeechMode
  phoneme_def.h     PhonemeDef struct, phoneme_unpack()
  phonemes.h        GENERATED — phoneme target table
  phrases.h         GENERATED — utterance phoneme strings
  sam_allophones.h  GENERATED, gitignored — SAM allophone target table
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

`tools/talkie2lattice.py` — decodes the TI-99/4A "Talkie" TMS5220 wordset's
own bitstream format into the LPC lattice tract's `LatticeFrame` format
(reflection coefficients, gain, pitch), following the DX7 `.syx` converter's
gitignored-output precedent:

```
talkie2lattice.py convert <in.ino> [in2.ino ...] <out.h>   # -> lattice_words.h (gitignored)
talkie2lattice.py dump <in.ino> [in2.ino ...]               # per-word summary, writes nothing
```

Source vocab files (`talkie/`, gitignored) aren't supplied — populate it
locally from Talkie's own `examples/` vocab sketches
(https://github.com/going-digital/Talkie) to run `convert` or the corpus-
gated half of `tools/test_talkie2lattice.py`.

`tools/sam2allophones.py` — converts locally-supplied S.A.M. reference
headers into the SAM tract's `SamAllophoneTarget` table, following the same
gitignored-output precedent:

```
sam2allophones.py convert <in.h> [in2.h ...] <out.h>   # -> sam_allophones.h (gitignored)
sam2allophones.py dump <in.h> [in2.h ...]               # per-allophone summary, writes nothing
```

Reference headers (`sam/`, gitignored) aren't supplied — populate it
locally with `RenderTabs.h`/`SamTabs.h` from one of the commonly-circulated
open S.A.M. reimplementations (e.g. https://github.com/s-macke/SAM) to run
`convert` or the reference-gated half of `tools/test_sam2allophones.py`. See
`sam2allophones.py`'s own module docstring for exactly what is and isn't
read from that reference data.

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
    SpeechTract tract;               // formant vs. LPC lattice (tract.h)
};
```

There is no `EnvConfig`/`Envelope` field — unlike every other engine's
voices, gate on/off runs through `render.h`'s own exponential `cur_amp`
smoothing, a simpler declick filter rather than the shared ADSR model.

```cpp
// tract.h — per-voice render state, Core 1 only, never crosses ParamExchange.
// Fields above `tract` are shared by both tracts; `fmt`/`lat` are a union
// (mutually exclusive, selected by `tract`) so 8 voices' worth of tract
// state costs the larger of the two variants, not their sum.
struct SpeechVoice {
    uint32_t glottal_phase;
    uint16_t noise_lfsr;
    float    cur_amp;              // smoothed toward gate target, declicks on/off
    uint8_t  last_trigger, last_phoneme;  // force a retrigger/target-load on first render
    float    lfo_phase;            // vibrato LFO phase, 0..1
    uint32_t glot_cycle_inc;       // this glottal cycle's jittered phase increment
    float    glot_cycle_amp;       // this glottal cycle's shimmer amplitude multiplier
    uint16_t jitter_lfsr;          // separate seed from noise_lfsr
    uint16_t seg_index;            // position within the current utterance (formant tract)
    uint32_t seg_remaining;        // native samples left in the current segment (formant tract)
    bool     seq_done;             // utterance complete; voice still allocated but silent
    bool     gate_prev;            // previous call's gate, to find the note-off edge
    bool     active;               // still sounding

    SpeechTract tract;

    union {
        FormantVoiceState fmt;
        LatticeVoiceState lat;
    };
};

// tract.h — formant tract's own state, one of SpeechVoice's two union members
struct FormantVoiceState {
    Res2p    formant[SPEECH_FORMANTS];
    Res2p    fric, nasal;
    float    F[SPEECH_FORMANTS], B[SPEECH_FORMANTS];         // current, ramped
    float    F_tgt[SPEECH_FORMANTS], B_tgt[SPEECH_FORMANTS]; // segment target
    float    fric_F, fric_B, fric_F_tgt, fric_B_tgt;
    float    nasal_F, nasal_B, nasal_F_tgt, nasal_B_tgt;
    float    av, af, an, av_tgt, af_tgt, an_tgt;              // branch amplitudes
    float    formant_shift, formant_shift_tgt;
    float    bandwidth_scale, bandwidth_scale_tgt;
};

// lattice.h — LPC lattice tract's own state, SpeechVoice's other union member
struct LatticeVoiceState {
    float    k[SPEECH_LATTICE_ORDER], k_step[SPEECH_LATTICE_ORDER];  // reflection coeffs, linear ramp
    float    gain, gain_step;
    float    b[SPEECH_LATTICE_ORDER];   // backward-residual delay line (the filter's only memory)
    uint16_t frame_index;
    uint32_t frame_remaining;
    uint32_t phase_inc;                 // this frame's glottal phase increment, at SPEECH_LATTICE_RATE
    bool     voiced;                    // this frame's excitation: glottal pulse vs. LFSR noise
    bool     word_done;
    float    resample_frac, y_prev, y_cur;  // fractional-ratio upsampler state
};
```

`F`/`B` are ramped toward `_tgt` once per sub-block by a fixed coefficient
(`tract_advance_subblock()`), not by a precomputed per-sub-block delta —
see Resonator and the Stability Rule for why coefficients are never
interpolated directly. Vibrato/jitter/shimmer state resets on retrigger,
not on a plain segment/phoneme change — it tracks the *note*, the same
lifetime as `glottal_phase`. `LatticeVoiceState`'s `k`/`gain` are the
lattice-only exception to that rule — see LPC Lattice Tract below.

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
per-voice cost. This applies to the formant tract only — the LPC lattice
tract (below) uses a different native rate and resampler entirely.

### LPC Lattice Tract

A 10th-order all-pole lattice filter, the sibling tract `VoiceParams::tract`
selects per voice. Reuses the formant tract's glottal-pulse/LFSR-noise
excitation and the sequencer's sub-block-driven render loop shape
unchanged — only the tract filter itself (`lattice.h`) and its frame data
are new.

**Native rate**: 8 kHz, the TMS5220's real frame rate — half the formant
tract's own 22.05 kHz, since an all-pole model of a full vocal tract in one
filter doesn't need formant-cascade headroom. `SAMPLE_RATE / 8000` has no
integer shortcut (44100/8000 = 5.5125), so `speech_render_voice_lattice()`
pulls native samples through a linear-interpolation upsampler on demand,
one native sample at a time whenever its fractional accumulator
(`LatticeVoiceState::resample_frac`) crosses 1.0 — carried across render
calls, not reset per buffer, so a buffer boundary can't introduce phase
drift.

**Coefficient frames**: 25 ms each (200 samples at 8 kHz) — the TMS5220's
own frame period — interpolated 8 times per frame (every
`SPEECH_LATTICE_SUBBLOCK` = 25 native samples), matching the real chip's
own interpolation cadence. Unlike the formant cascade's `F`/`B`
(Resonator and the Stability Rule below), lattice reflection coefficients
`k[i]` are ramped and used directly as filter coefficients, with no
recompute step in between: this is safe because a stable, order-10
all-pole lattice only requires every `|k[i]| < 1`, and that interval is
convex, so linearly interpolating between two in-range coefficients can
never leave it. Debug builds assert this after every interpolation step.

**Word data**: `tools/talkie2lattice.py` decodes a real Talkie TMS5220
vocab source into `lattice_words.h` (`LATTICE_WORDS[]`, gitignored, ~1,163
words once generated locally). Without a generated corpus, every lattice
voice plays one hardcoded fixture, `lattice.h`'s `LATTICE_TEST_WORD` —
reflection coefficients from a real order-10 Levinson-Durbin analysis of
this module's own `/i/`/`/a/`/`/u/` formant-cascade impulse responses
(`phonemes.h`), not hand-guessed or corpus-derived. `SPEECH_LATTICE_GAIN_BOOST`
(`lattice.h`) scales every lattice voice's excitation regardless of word
source; it's tuned against the real corpus's worst-case resonance (its
chip-recorded reflection coefficients sit much closer to the unit circle
than the test word's own smoother, synthetic ones), which leaves
`LATTICE_TEST_WORD` quieter than before.

**`KEY_PER_WORD` addressing**: the only word-addressing mode built so far,
of three the design leaves room for (see Decision Record). MIDI note
number (0–127) selects a word within the channel's current 128-word page;
Program Change selects which page is current (tract-dependent — see MIDI
Mapping). `midi_controller.cpp`'s `speech_lattice_word_for_key()` resolves
note+page to a `LATTICE_WORDS[]` entry, wrapping with `%` past the last,
partially-populated page the same way every other table lookup in this
module wraps out-of-range input. `VoiceParams::lattice_word` carries the
resolved word as a pointer, set at note-on and read straight through by
`audio_engine.cpp` — always valid, defaulting to `&LATTICE_TEST_WORD`,
whether or not a corpus has been generated. A word plays at its own
recorded per-frame pitch contour by default; CC103's Q8.8 multiplier
(`VoiceParams::lattice_pitch_shift`) overrides it live, re-applied at
every frame boundary so a CC change reaches a word already in progress
within one 25 ms frame period.

**Note-off modes on the lattice tract**: `SpeechMode` applies the same
GATED/ONESHOT/LOOP contract lattice voices get from
`speech_render_voice_seq()`'s formant path, adapted to a frame array
instead of a phoneme string — GATED jumps straight to the word's own
final frame (always the decoder's trailing silent STOP frame) on
note-off rather than cutting mid-glide; ONESHOT ignores note-off for word
progression; LOOP restarts at frame 0 while still gated and degrades to
one-shot completion once gate drops.

**Excitation source (CC104)**: the shared `excitation.h`
`glottal_pulse()`/`osc/noise.h` pair (a smooth bipolar triangle spanning
the whole pitch period for voiced segments, a full-range LFSR for
unvoiced ones, both also used by the formant tract) is the default;
switching selects `lattice.h`'s own TMS5220-accurate pair instead --
decap-verified chip data, not a hand-tuned approximation, for both:
`lattice_chirp_pulse()` (a short, sharp, non-negative burst concentrated
at the very start of each pitch period, from the chip's real 52-sample
excitation table, followed by silence for the rest of it) for voiced
segments, and `lattice_chip_noise()` (a 13-bit LFSR decimated 20:1, its
lowest bit picking between two fixed levels rather than read out as
multi-bit noise) for unvoiced ones. Both together, not just the voiced
half, are what give the real chip its buzzier, harsher, more digital
character. CC104 currently switches both at once (paired, not
independently selectable -- see Decision Record for why and how that
could split later). `LatticeVoiceState::chirp_idx`/`tms_rng` are tracked
unconditionally regardless of which exciter is selected, so switching
mid-note has correct state from its very first sample. Live, like the
pitch-shift CC.

The chirp exciter's crest factor is measurably higher than
`glottal_pulse()`'s own (over 2x) -- real corpus words that fit safely
under `SPEECH_LATTICE_GAIN_BOOST`'s existing calibration clipped under
the chirp exciter until `SPEECH_LATTICE_CHIRP_GAIN_SCALE` (0.86, tuned
the same empirical way against the real corpus) brought its own
worst-case peak back down to match.

### SAM Tract

A third sibling tract, `VoiceParams::tract` selects the same way as the
LPC lattice tract above. Three formant resonators (`sam.h`'s
`SamVoiceState::formant[SAM_FORMANTS]`, `SAM_FORMANTS == 3`) are driven
and summed independently rather than chained -- each is excited directly
by the shared glottal pulse, scaled by its own per-formant amplitude
weight, instead of one resonator's output feeding the next the way the
formant cascade's five stages do. A dedicated frication resonator, driven
by the shared LFSR noise excitation, approximates unvoiced consonants
through the same parallel structure rather than sampled audio data.
Coefficients follow the same rule as the formant cascade: F/B/amplitude
targets ramp one sub-block at a time and coefficients are recomputed from
the ramped values, never interpolated directly.

**Native rate and resampling**: shares the formant tract's own 22.05 kHz
native rate and zero-order-hold ×2 resample path -- there's no single
native sample rate specific to this tract's own source material to
target, unlike the LPC lattice tract's TMS5220-matched 8 kHz.

**No word-addressing scheme or reciter yet**: every SAM voice plays one
sustained allophone, held under gate like the formant tract's own phoneme
keyboard. `VoiceParams::phoneme` -- the same field the formant tract's
phoneme keyboard reads -- indexes `render.h`'s `sam_allophone_target()`,
which resolves to the full generated allophone table once one has been
converted locally, or `sam.h`'s small hardcoded `SAM_TEST_ALLOPHONES`
fixture (silence, three vowels, one fricative) otherwise -- a voice is
always valid whichever data source is active.

**Allophone table**: `tools/sam2allophones.py` converts locally-supplied
S.A.M. reference headers (e.g. `RenderTabs.h`/`SamTabs.h` from one of the
commonly-circulated open reimplementations) into `sam_allophones.h`
(gitignored, `SAM_ALLOPHONES[]`/`SAM_ALLOPHONE_DATA_COUNT`) -- 80
allophones, S.A.M.'s own published segmentation. Formant frequency (F1-F3)
targets are not sourced from the reference data: S.A.M.'s own frequency
tables are phase-increment values for a three-oscillator wavetable
technique, not Hz values for a resonant filter, so the tool extends the
same Peterson & Barney (1952) published acoustic data
`tools/speech_phonemes.csv` already uses instead. What *is* read from the
reference data is genuinely S.A.M.-specific: which allophones are
noise-driven (`sampledConsonantFlags`, mapped to `af`/`amp`) and each
formant's relative loudness (`ampl1data`/`ampl2data`/`ampl3data`, through
the reference's own rescale curve, mapped to `amp`). Without a converted
table present locally, every SAM voice plays the fixture; the reference
source directory (`sam/`) and the generated header are both gitignored,
only the converter is committed -- see `tools/sam2allophones.py`'s own
module docstring for the full sourcing breakdown.

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

Formant tract: ~93.5 cycles/frame/voice (2.75%), flat from 1 to 8 voices,
on real `breadboard_rp2350` hardware — the fricative/nasal branches and a
live formant-shift/bandwidth-scale sweep cost nothing extra over that
flat number (both already run unconditionally). 8 voices alone: 22% of
Core 1; with reverb (mutually exclusive with delay): ~30%.

LPC lattice tract: ~77.5 cycles/frame/voice, also flat from 1 to 8
voices, cheaper than the formant tract despite the extra resampler step —
the lower 8 kHz native rate more than pays back the order-10 lattice
recursion's own per-sample cost once normalized per output frame. 8
voices alone: 18.1% of Core 1. `MAX_VOICES = 8` is shared between both
tracts (one voice pool, tract selected per voice — see Decision Record);
since the lattice tract is cheaper, not more expensive, than the formant
tract, the shared pool's worst case stays bounded by the formant
numbers above regardless of which tract's voices fill it.

SAM tract: not yet measured on hardware — its own per-voice cost, and
whether the shared `MAX_VOICES = 8` pool still holds once it's included,
are a later slice.

Full measurement breakdown: `history_speech.md`.

### Future / TODO

- **SAM reciter** — a from-scratch English letter-to-sound reciter and a
  build-time phrase bank (the allophone/pitch-table import tool is done,
  see `tools/sam2allophones.py`) are a separate, later slice.
- **SAM pitch contour** — the stress-driven pitch overshoot/undershoot
  that gives S.A.M. its characteristic cadence is unbuilt; planned to be
  baked in by the reciter and rendered as a per-segment ramped target,
  the same shape F/B targets already ramp.
- **SAM MIDI/CC integration** — beyond the CC102 tract-select band, the
  SAM tract has no live controls, preset-table entry, or display support
  of its own yet.
- **Further LPC addressing modes** — `KEY_PER_WORD` is the only
  word-addressing mode built; a "musical" mode spreading one word across
  the keyboard via standard MIDI Bank Select + Program Change, and a
  two-CC pseudo-program-select for controllers that can't reliably send
  Program Change, are both left room for but unbuilt.
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
  spread and glitch-free switching between two ordinary formant presets —
  `KEY_PER_WORD`'s own preset switch is confirmed, see Decision Record),
  MIDI-layer correctness (Program Change/CC parsing, phrase-bank note
  mapping — only compile-verified so far), and the profiling-pin
  measurement of effects-on cost.

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
13. **`SpeechVoice`'s tract-specific state is a union
    (`FormantVoiceState`/`LatticeVoiceState`), not two flat members** —
    only one tract renders per voice at a time, so a union bounds 8
    voices' cost by the larger variant instead of both combined. The
    union switch always lands on a note-on retrigger, which
    placement-constructs the newly-selected member before writing into
    it, so a voice that changes tract between notes never reads the
    other tract's stale state.
14. **Lattice reflection coefficients are ramped and used directly as
    filter coefficients**, unlike the formant cascade's `F`/`B` (which are
    ramped and then recomputed into biquad coefficients every sub-block)
    — a documented, lattice-only exception to the Resonator and the
    Stability Rule, safe specifically because the all-pole lattice stays
    stable for any `|k[i]| < 1`, and that range is convex.
15. **The LPC test word's coefficients come from a real Levinson-Durbin
    analysis of this module's own formant-cascade impulse responses**,
    not hand-guessed numbers or Talkie-corpus data — the corpus converter
    doesn't exist yet (see Future/TODO), and the module's existing
    `/i/`/`/a/`/`/u/` targets already had published-value cross-checks
    (`vowel_reference.h`) backing their own correctness.
16. **CC102 (tract select) is the only LPC-specific CC wired so far**,
    ahead of any real word-selection mode — cheap, forward-compatible
    (inside the CC102–119 range already reserved for LPC controls), and
    it's what lets the hardcoded test word actually be reached from a
    MIDI channel instead of only from host-render tooling.
17. **`talkie2lattice.py`'s TMS5220 decode tables are re-expressed as this
    project's own data, not copied from the Talkie library's source
    file** — the bitstream *format* (which bits mean what) is the chip's
    own hardware behavior, the same reasoning that already applies to the
    DX7 sysex byte layout ported from Dexed, but Talkie itself is GPLv2 (a
    stricter, copyleft license than Dexed's Apache-2.0), so the specific
    table values are transcribed independently and attributed by comment
    rather than copied wholesale.
18. **A word whose bitstream fails to decode is skipped with a warning,
    not an abort of the whole run** — a deliberate deviation from
    `syx2patch.py`'s stricter "any bad voice aborts the whole bank"
    policy, since the Talkie corpus (~1,163 words across several
    independently-sourced files) is roughly an order of magnitude larger
    than a 32-voice DX7 bank, and one corrupted or unusually-shaped word
    blocking every other word's conversion is a worse failure mode at
    that scale. Every skip is still reported, not silent.
19. **The TMS5220's raw energy value is normalized to `LatticeFrame.gain`
    by dividing by 255**, a different unit and calibration than #63's
    `LATTICE_TEST_WORD` (whose gain values came from an unrelated
    Levinson-Durbin analysis, then re-scaled again by render.h's
    `SPEECH_LATTICE_GAIN_BOOST`) — real corpus playback may need its own
    loudness retuning once heard on hardware; this converter's job is a
    faithful decode of the chip's own value, not a loudness match to
    LATTICE_TEST_WORD's bring-up-only calibration.
20. **`KEY_PER_WORD` is the first of three addressing modes the design
    reserves room for, and the only one built** — key+Program-Change-page
    is directly what a real MIDI keyboard already offers, where the
    "musical" (Bank Select + Program Change spreading one word across the
    keyboard) and two-CC pseudo-program-select modes both need either
    standard Bank Select support or a controller workaround this slice
    doesn't need yet.
21. **Program Change's meaning is tract-dependent** (utterance under the
    formant tract, `KEY_PER_WORD` page under the LPC tract) rather than a
    new dedicated CC for page-select — reuses the meaning Program Change
    already has ("what a note-on plays"), and the two tracts are mutually
    exclusive per channel, so there's no message that could mean both at
    once.
22. **`VoiceParams::lattice_word` carries a pointer, not an index** —
    mirrors the FM engine's own `const FmPatch *patch` field. A voice is
    always valid (defaults to `&LATTICE_TEST_WORD`) whether or not
    `lattice_words.h` has been generated locally, the same
    gitignored-generated-data contract `T00T_FM_HAS_PATCHES` already
    established, without needing `LATTICE_WORD_COUNT` visible to every
    consumer of `VoiceParams`.
23. **A lattice word's `SPEECH_MODE_GATED` release point is always its own
    final frame**, not a separately authored marker (unlike the formant
    tract's `SpeechUtterance::release_index`) — the Talkie bitstream
    format has no release marker of its own, and `talkie2lattice.py`'s
    decoder already guarantees every word ends in a genuine silent STOP
    frame, so this needs no new field on `LatticeWord`/`LatticeFrame`.
24. **`SPEECH_LATTICE_GAIN_BOOST` is retuned against the real corpus's
    worst-case resonance, not `LATTICE_TEST_WORD`** — real chip-recorded
    reflection coefficients sit much closer to the unit circle than the
    test word's own smoother, synthetic ones, so the single constant that
    keeps the whole corpus unclipped leaves the test word quieter than it
    was before. A per-word calibration was rejected: this tract already
    has one intentional global excitation-scale constant
    (`SPEECH_EXCITATION_HEADROOM`'s own precedent on the formant side),
    and per-word gain data would mean extending `LatticeFrame`'s format
    and `talkie2lattice.py`'s emission for a problem one shared constant
    already solves.
25. **The pitch-shift multiplier gets a new CC (103) rather than reusing
    an existing live slot** — it has no formant-tract equivalent to share
    with, and the CC102–119 range was already reserved for exactly this
    kind of LPC-specific addition.
26. **`SpeechPreset` gained `tract`/`lattice_page`/`lattice_pitch_shift`,
    and every existing preset row sets `tract` explicitly** (all
    `SPEECH_TRACT_FORMANT`) rather than leaving it to an implicit
    zero-value default — a preset table is exactly the kind of place a
    silently-defaulted field goes unnoticed for a long time, and the cost
    of writing it out on nine already-existing rows is one column.
    `lattice_page` isn't a `VoiceParams` field (it's per-channel
    addressing state, not a per-voice render parameter), so
    `speech_load_preset()` reads it straight from the preset row rather
    than through `voice_apply_preset()`, the same way it already reads
    `chorus`.
27. **`MAX_VOICES = 8` stays shared between both tracts, not split into a
    separate per-tract budget** — real hardware measurement put the LPC
    lattice tract at ~77.5 cycles/frame/voice, flat from 1 to 8 voices,
    cheaper than the formant tract's own ~93.5, so the shared pool's
    worst-case Core 1 cost is already bounded by the formant numbers
    (Performance above) regardless of which tract fills it. A separate,
    larger budget for the cheaper tract would be a real architecture
    change (per-tract voice pools instead of one shared pool with a
    per-voice tract selector) that nothing in the measurement justifies.
28. **The velocity toggle (CC15) is next-note, not live, and defaults
    on** — velocity only ever feeds `amplitude` at the moment a voice is
    struck (there's no separate live-amplitude CC it would need to keep
    in sync with), so "live" has no meaning for it the way it does for
    formant_shift or rate; defaulting on keeps a fresh channel's dynamics
    behaving exactly as they did before this CC existed.
29. **The TMS5220 chirp table's exact values are decap-verified data,
    cross-checked against two independent MAME source trees** (the
    current `tms5110r.hxx`'s `TI_LATER_CHIRP`, used by the
    TMS5110A/TMS5200/TMS5220 family this tract targets, and the older
    historic-mame single `chirptable[]` as an independent check on the
    fetch itself, not as a source — it's a different, earlier chip's
    table and correctly differs) rather than trusted from memory or
    written by ear, the same rigor `tools/talkie2lattice.py`'s own K
    tables already established for exactly this kind of chip-behavior
    data.
30. **`lattice_chirp_pulse()` is a lattice-tract-only alternative, not a
    replacement for `excitation.h`'s `glottal_pulse()`** — the formant
    tract's own excitation is already tuned and hardware-verified against
    published vowel formant data, so it stays untouched; CC104 exists
    specifically so both can be A/B'd live rather than picking a winner
    outright.
31. **CC104 is live, not next-note** — unlike a lattice word's frame
    coefficients, switching excitation source has no filter-state
    discontinuity to worry about (`LatticeVoiceState::chirp_idx` is
    tracked unconditionally either way), so there was no reason to make a
    live A/B comparison wait for the next note.
32. **The real chip's own coarse two-level LFSR noise (unvoiced source) is
    paired with the chirp table under the same CC104, not independently
    switchable** — a deliberate, temporary choice: hear both chip-accurate
    sources together first, decide by ear whether unvoiced segments need
    their own separate toggle, rather than pre-emptively adding a second
    CC before knowing it's wanted. Splitting later is a small change
    (`chirp_exciter` already reads as "voiced source" by name; a second
    bool for the noise half doesn't disturb it), not a rework.
33. **`SPEECH_LATTICE_CHIRP_GAIN_SCALE` (0.86) is a second, chirp-only
    scale on top of `SPEECH_LATTICE_GAIN_BOOST`, not a lower shared
    constant** — the chirp exciter's own crest factor is measurably
    higher than `glottal_pulse()`'s (over 2x), so the same excitation
    scale that keeps the corpus safe under the default exciter clipped
    two of its loudest words (O/OH) under the chirp one. Lowering
    `GAIN_BOOST` itself would have fixed that at the cost of quieting the
    default exciter for a problem specific to the other one; a second,
    chirp-only multiplier fixes it without touching the already-correct
    default calibration.
34. **The SAM tract's three formant resonators are driven and summed in
    parallel, not chained into a cascade** — the defining structural
    difference from the formant tract's own five-stage cascade, chosen to
    give this tract its own distinct, buzzier character rather than a
    third variation on the same cascade topology.
35. **Unvoiced consonants are approximated through a dedicated frication
    resonator driven by the shared LFSR noise excitation, not sampled PCM
    bursts** — the original source renders some unvoiced consonants from
    short embedded audio samples; importing that data would mean a second
    class of imported proprietary audio alongside any allophone/pitch
    table data, on top of the formant-synthesis approximation already
    accepted for this tract's overall authenticity target. An optional
    sample-accurate mode is left as a possible later addition, not built
    here.
36. **`SpeechVoice`'s tract-state union grew a third member (`sam`)
    instead of a separate flat struct** — the same reasoning that already
    governs the formant/lattice union: only one tract renders a given
    voice at a time, so 8 voices' worth of tract state costs the largest
    single variant, not the sum of all three.
37. **The SAM tract shares the formant tract's 22.05 kHz native rate and
    zero-order-hold ×2 resample path**, rather than adopting a rate of
    its own the way the LPC lattice tract's 8 kHz matches the TMS5220's
    real frame rate — there's no single sample rate specific to this
    tract's own source material, since it ran across several home
    computers at whatever rate each one's own DAC used.
38. **`SAM_EXCITATION_HEADROOM` reuses the formant cascade's own
    `SPEECH_EXCITATION_HEADROOM` value rather than a separate constant**
    — an initial, smaller headroom left the fricative allophone
    uncomfortably close to clipping at full velocity; the formant
    tract's own value, already proven safe at the same frication-branch
    target values, brought every fixture entry comfortably under the
    ceiling.
39. **CC102 (tract select) changed from a two-way to a three-way band**
    (formant / LPC lattice / SAM), the same banding shape CC27's mode
    select already uses, rather than reserving a separate CC for the new
    tract — CC102 already meant "which tract," so a third band extends
    that meaning instead of introducing a second, overlapping control.
40. **The SAM tract's bring-up fixture reuses CC20's existing
    phoneme-select band** (wrapped onto the smaller `SAM_ALLOPHONE_COUNT`)
    rather than a dedicated CC of its own — cheap reachability from a
    real MIDI channel ahead of any SAM-specific addressing scheme, the
    same reasoning CC102 alone gave the LPC lattice tract's own hardcoded
    test word before `KEY_PER_WORD` existed.
41. **The SAM allophone table's formant frequencies are not decoded from
    S.A.M.'s own reference data**, unlike every other converter this
    project has built — S.A.M.'s `freq1`/`freq2`/`freq3` tables are
    phase-increment values driving three independent oscillators (its own
    synthesis technique doesn't use resonant filters at all), not Hz
    values a filter-based tract could use, and there's no way to verify a
    translated guess without a reference render to compare against.
    `tools/sam2allophones.py` extends the same published Peterson & Barney
    acoustic data the formant tract's own phoneme table already uses
    instead.
42. **What *is* read from the reference data is the per-allophone
    noise/formant classification and amplitude balance**
    (`sampledConsonantFlags`, `ampl1data`/`ampl2data`/`ampl3data`), not
    formant frequency — these are genuine structural/behavioral S.A.M.
    data (which allophones are noise-driven, how loud each formant is
    relative to the others), not audio waveform data, keeping the
    reference-data dependency real without needing the frequency-encoding
    translation #41 rejected.
43. **The reference data's own per-allophone arrays are 80 entries, not
    the 81 some published documentation implies** — every reference
    implementation `tools/sam2allophones.py` has checked ships
    `sampledConsonantFlags`/`ampl1-3data`/`phonemeLengthTable` at 80
    entries; `SAM_ALLOPHONE_COUNT` and `SAM_ALLOPHONE_NAMES` match that
    verified data shape (dropping a documentation-only "UN" 81st slot,
    folded into the adjacent syllabic-nasal "UM") rather than a
    transcribed comment believed without the data to back it.
44. **`sam2allophones.py` searches every given input file for each named
    array**, rather than requiring a fixed file-to-array mapping — the
    reference implementations split these arrays across files
    differently (`RenderTabs.h` vs `SamTabs.h`) depending on which fork
    supplied them, so the converter shouldn't need to know or care which
    file a given array lives in.

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
- **Lattice filter**: an all-pole IIR filter structured as a chain of
  stages, each parameterised by one reflection coefficient, rather than
  the direct-form biquad coefficients the formant cascade uses.
- **Reflection coefficient**: a lattice filter's per-stage parameter
  (`k[i]`, `|k[i]| < 1` for stability); the LPC lattice tract's analogue
  of the formant cascade's per-formant F/B pair.
