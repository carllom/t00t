# T00T — Speech Module: Development History

Dated build-phase results and measurement logs for the speech synthesis
module. See `module_speech.md` for the current spec/design.

---

## Speech Engine (build skeleton, #27)

Fourth build-time engine (`make ENGINE=speech`, `breadboard_rp2350` default),
proving the build seam and `module_speech.md`'s divergences from the shared layer
model before any formant DSP exists:

- `MAX_VOICES = 4`, defined in `src/engines/speech/engine.h` ahead of its
  `#include "engine_base.h"`, per #10 — a placeholder pending the P2
  profiling measurement in `module_speech.md`. Only this engine — the other three
  are untouched.
- Unlike the tracker (#13), the standard `ParamExchange`/`voice_alloc` path
  is kept: `module_speech.md` settles that polyphonic speech has N independent
  per-voice segment clocks, not one global tick clock, so the tracker's
  ordered TickBlock ring is not adopted here. `src/voice_alloc.cpp` links
  normally (no `ENGINE_VOICE_ALLOC` override needed in `CMakeLists.txt`).
- `fx/delay.h`/`fx/reverb.h` **are** linked (unlike the tracker) — `module_speech.md`:
  "speech has no sample-RAM pressure" to protect, so there's no reason to
  exclude them, and the `.bss` table below confirms they're present (a
  128 KB delay ring buffer plus reverb, not stripped by the linker).
- Native render rate is `SPEECH_RATE = SAMPLE_RATE / 2` (22.05 kHz), zero-
  order-held ×2 to the shared 44.1 kHz output stage — `src/engines/speech/
  render.h`'s `speech_render_test_tone()` does the resample as a bare integer
  doubling (`dry_{l,r}[2*i] == dry_{l,r}[2*i+1]`), no fractional accumulator.
  That function has no pico-sdk dependency (only `osc/sine.h` + `pan.h`), so
  it is the literal shared source between the device path
  (`audio_engine.cpp`, called from the Core 1 render loop) and the new host
  target below — proving the ZOH seam identically on both before any real
  DSP depends on it.
- `src/engines/speech/display.cpp` and `midi_controller.cpp` are stubs (no
  UI, no phoneme keyboard yet) so the shared `gfx.cpp` path and MIDI
  transports still link.
- Sound source: voice 0 is a hardcoded, always-on 220 Hz test tone (centre
  pan) rendered at the native rate and ZOH'd to the stereo output tail — a
  build/boot smoke test, not a synth. Voices 1-3 are unused placeholders.
  `PROFILE_PIN` (GPIO 22) is bracketed around the render call, ready for the
  P2 measurement slice.
- Host target: `render_speech` (`tools/host_render/render_speech.cpp`, built
  via `make host`) calls the identical `speech_render_test_tone()`, renders
  2 s to `speech_test_tone.wav`, and asserts the ZOH invariant sample-by-
  sample rather than by ear. All checks pass as of 2026-08-07.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=speech`, and confirmed via a `.bss` size comparable to the groovebox's
(both link delay+reverb) that neither effect was stripped:

| Engine | text | bss | dec |
|---|---|---|---|
| speech (skeleton) | 27,544 | 194,260 | 221,804 |
| subtractive (default) | 206,096 | 198,344 | 404,440 |
| groovebox | 55,580 | 199,764 | 255,344 |
| tracker | 58,124 | 409,080 | 467,204 |

`make`, `make ENGINE=groovebox`, and `make ENGINE=tracker` all build clean
from a fresh `build/`, unchanged in behaviour by #27 (the subtractive/
groovebox/tracker figures above drift slightly from their #13-era table
entries due to unrelated work landed since — not this change).

## Speech Engine — Common Component Extraction (P0)

Plan was to land `res2p.h` and `noise.h` in the common layer, wiring
`res2p.h` into the groovebox first (808 toms/congas) so the resonator
would be proven against a known sound before any speech code depended on
it — exit criterion: 808 toms/congas rebuilt on `res2p.h`, output diffed
against the previous build, no audible change.

**Half executed.** `res2p.h` landed in the common layer and is what
speech's formant cascade and fricative/nasal branches are built on. The
groovebox side never happened: the groovebox's toms/congas still use
their own pitch-envelope generator, not `res2p.h`, and `res2p.h`'s own
header comment still reads "backport pending". Left open — see
`module_speech.md`'s Future/TODO.

## Speech Engine — Phoneme Keyboard (#28)

P1: static formant targets, no sequencer, no transitions. One MIDI note =
one sustained phoneme, pitch from note number. Cascade + excitation only;
fricative and nasal branches deferred to #29.

Hardware-verified on real `breadboard_rp2350`: notes, pan, vowel
selection, and both delay and reverb confirmed working end-to-end.

## Speech Engine — Fricative Branch, Nasal Pole, Mixed Excitation (#29)

Added the parallel fricative resonator and nasal pole alongside the
existing voiced cascade, plus mixed excitation (voiced + noise together,
for voiced fricatives like /z/, /v/, /ʒ/) and live `formant_shift`/
`bandwidth_scale` parameters (CC21/22, ramped per sub-block so a CC sweep
can't zipper). This is also what extended `PhonemeDef` with
`fric_F`/`fric_B`/`nasal_F`/`nasal_B` — the original 16-byte sketch
predated this work and had no field for a per-phoneme fricative/nasal
pole.

Confirmed audible on real `breadboard_rp2350` hardware.

## Speech Engine P2 Profiling (#31)

Speech's measurement gate — the direct analogue of the tracker's #16 — deciding
`MAX_VOICES` from a real profiling-pin reading of the full tract (#28 cascade
+ #29 fricative/nasal branches) instead of `module_speech.md`'s "do not trust these
numbers" budget-table estimate.

### Measurement rig

`make ENGINE=speech SPEECH_PROFILE=1` builds an alternate `audio_engine_run()`
(guarded by `T00T_SPEECH_PROFILE`, `src/engines/speech/audio_engine.cpp`) that
replaces the normal MIDI-driven loop with a self-cycling, pin-only rig — same
"hands-off" shape as #16's tracker rig: no display, no stdio, `PROFILE_PIN` (GPIO
22) is the only readout. `MAX_VOICES` is raised to 8 for this build only
(`engine.h`, gated on the same define) so the 8-voice phase has a real 8th slot;
the normal build is untouched and still `MAX_VOICES = 4`.

It self-cycles through 8 phases, holding each ~4s forever, driving `voices[]`
directly with synthetic content (bypassing `ParamExchange`/MIDI entirely, so
content is identical and repeatable on every cycle):

| Phase | Voices | Phoneme | What it isolates |
|---|---|---|---|
| idle | 0 | — | Fixed per-buffer overhead (buffer clear, output loop) |
| 1v | 1 | /a/ (vowel) | Baseline single-voice cost |
| 2v | 2 | /a/ | Linearity check |
| 4v | 4 | /a/ | Working-assumption voice count |
| 8v | 8 | /a/ | Double the working assumption |
| 4v fricative | 4 | /z/ (voiced fricative) | Parallel branch cost vs. the 4v /a/ phase — same voice count, only the phoneme differs |
| 4v recompute-only | 4 | /a/ | Calls `tract_advance_subblock()` only, skipping the per-sample `tract_process_mixed()` loop entirely — isolates coefficient-recompute cost vs. the 4v /a/ phase |
| 4v swept | 4 | /a/ | `formant_shift`/`bandwidth_scale` triangle-swept across their full CC range every buffer, vs. the 4v /a/ phase held at 1.0x |

Reading procedure: flash, let it free-run, read the profiling-pin duty cycle during
each phase (a scope/logic analyzer trigger on the phase transitions, or just enough
patience to catch each ~4s window), record against the table below.

Held-vowel-vs-fricative and static-vs-swept are both expected, from code
inspection, to measure the *same* as their comparison phase: `tract_process_mixed()`
always ticks the cascade, fricative and nasal resonators unconditionally regardless
of a phoneme's `av`/`af`/`an` mix weights (no branch skips a silent resonator), and
`tract_advance_subblock()` always recomputes every resonator's coefficients every
sub-block regardless of whether `formant_shift_tgt`/`bandwidth_scale_tgt` actually
moved. If the real numbers disagree with that prediction, that is more interesting
than confirmation — it would mean some other effect (icache, compiler scheduling)
is in play, and needs explaining, not just recording.

### Measured, `breadboard_rp2350`, 2026-08-07

| Phase | idle | 1v | 2v | 4v | 8v | 4v /z/ | 4v recompute-only | 4v swept |
|---|---|---|---|---|---|---|---|---|
| Duty cycle | 0% | 2.82% | 5.5% | 11% | 22% | 11% | 1.3% | 11% |

Converted to cycles/output-frame at 3401 cycles/frame = 100% (idle is 0%, so no
baseline to subtract, unlike the tracker's #16 rig):

| Voices | Total | Per voice (c/f) | Per voice (%) |
|---|---|---|---|
| 1 | 95.9 c/f | 95.9 | 2.82% |
| 2 | 187.1 c/f | 93.5 | 2.75% |
| 4 | 374.1 c/f | 93.5 | 2.75% |
| 8 | 748.2 c/f | 93.5 | 2.75% |

Flat at **~93.5 cycles/frame/voice (2.75%)** from 2 to 8 voices; 1v's 2.82% is
within scope-reading resolution of the same flat line, not a real per-voice
discontinuity.

**Held vowel vs. voiced fricative: identical (11% both).** Confirms the
code-inspection prediction — the fricative/nasal branches are unconditional, not an
incremental cost paid only by phonemes that use them. There is no separate
"fricative branch cost" to add on top of the flat per-voice number above.

**Coefficient recompute, isolated: 1.3% at 4 voices** = 44.2 c/f total = **11.0
c/f/voice (0.32%)**. Subtracting that from the 4v total gives the per-sample-only
cost: 93.5 − 11.0 = **82.5 c/f/voice (2.43%)** for excitation + the 7-resonator tick
+ ZOH accumulate.

**Static vs. swept `formant_shift`/`bandwidth_scale`: identical (11% both).**
Confirms the second prediction — recompute happens every sub-block regardless of
whether the CC target actually moved, so a live sweep is free; the cost is already
fully included in the flat per-voice number, not an addition to it.

### Discrepancy vs. the predicted budget

Measured ~93.5 c/f/voice total is noticeably above module_speech.md's ~60–75 c/f
prediction — about 25–55% over the top of that range, the kind of gap "do not trust
these numbers" was written to expect. But it isn't evenly spread across the
budget's four line items:

- **Coefficient recompute measured at 11.0 c/f, the predicted line item was ~10
  c/f.** This part of the estimate was essentially right.
- **The remaining 82.5 c/f (excitation + resonators + ZOH) is well above the
  35–50 + 10 + 5 = 50–65 c/f those three line items predicted** — roughly 17–32
  c/f higher, i.e. the entire discrepancy lives here, not in coefficient recompute.

Explanation: the budget table priced the DSP math itself (resonator taps, pulse/
noise generation, doubling samples) but not the per-sample "glue" `render.h`'s loop
actually runs for every voice, every native sample — the `cur_amp` exponential
declick filter, the `voiced_src`/`noise_src` mix-and-scale multiplies, and writing
each ZOH-doubled sample into *two* accumulator buffers (4 stores per native sample,
not 2). None of that is its own line item in the prediction, but the code pays it
unconditionally regardless of phoneme — which is exactly why the fricative and
swept-parameter phases above cost nothing extra: that per-sample tax and the
resonator branch count are both fixed, not phoneme-dependent. On an M33 doing
single-precision float work with no vectorization, a handful of extra multiply/
store instructions per sample is a plausible source for the remaining gap, sized
about right for what it costs.

### Decision: `MAX_VOICES = 8`

Landed in `src/engines/speech/engine.h` (was 4 since #27).

- **93.5 c/f/voice is flat across the whole measured range (1–8 voices)** — no
  falloff or superlinear growth to worry about at higher counts.
- **8 voices costs 22% of Core 1 on their own.** Delay/reverb are mutually
  exclusive (mono send/return, one active effect at a time — `audio_engine.cpp`),
  so the worst realistic total is 8 voices + reverb = 22% + 8% = **30%**,
  comfortably under the ≤50% ceiling the tracker's own #16 decision used as its
  target.
- This is double the #27-era working assumption of 4, so **"robot chorus" (a
  preset with per-voice detune and stereo spread, module_speech.md's Open Questions) is
  now explicitly on the table for P5** — the payoff module_speech.md said to name if the
  number landed better than expected.
- Not pushed further than 8: that's what was measured, and the ≥30% total with
  reverb is a reasonable place to stop banking headroom against P3's still-unbuilt
  segment sequencer, which module_speech.md itself budgets as "a table read plus
  coefficient computation" but hasn't been measured yet.

`module_speech.md`'s former "voice count" open question is struck and moved into
Settled Decisions (as the `MAX_VOICES = 8` bullet above) — its numbered
position in the Open Questions list has since shifted as other items were added
and resolved, so it's no longer "Open Question 2" there.

## Speech Engine — SpeechMode and SUSTAINABLE Decisions (#30)

Settled before writing the segment sequencer, since it was P3's blocker:
it determines whether `seg_remaining` and the active-voice bitmap can
diverge.

**Default `SpeechMode` for new presets is `SPEECH_GATED`.** Every other
engine in this project treats note-off as something a player can hear
happen — the subtractive engine's ADSR release, the groovebox's one-shot
decay envelopes. A default that ignores note-off (`ONESHOT`) would make
the gate on a freshly-created speech preset appear inert, the wrong first
impression for an instrument meant to be playable, not just a talking
clock. `LOOP` and `HOLD` are both real modes but neither is a sane
default: LOOP repeating indefinitely while held is a specialised
drone/vocoder behaviour a preset should opt into, and HOLD sustains a
single phoneme rather than sequencing an utterance at all, so it doesn't
apply to utterance presets.

How GATED bounds `seg_remaining` against note-off: note-off does not zero
it directly (that would cut the tract mid-coefficient-ramp, the click the
resonator stability rule exists to avoid). Instead it jumps `seg_index` to
the utterance's designated release segment, and the normal per-segment
clock runs from there — bounding maximum overhang to the release
segment's own duration. `ONESHOT` and `LOOP` were bounded the same way for
consistency (see `module_speech.md`'s Architecture section for the current
behavior of all three).

A new note-on for a voice still finishing an utterance always wins
immediately — `tract_retrigger()`'s existing phoneme-level rule (snap to
target, no glide, reset state), applied at the utterance level. The
alternative (queuing the note-on until the old utterance's release segment
finishes) would make a fast retrigger on a polyphonic instrument feel like
the busiest voice is stuck — a worse failure mode than clipping a release
tail, the same tradeoff the tracker's underrun policy already makes.

**`SUSTAINABLE` reserved as bit 3 of `PhonemeDef.flags`.** Nothing reads
this bit until singing mode lands (still unbuilt — see
`module_speech.md`'s Future/TODO). The reasoning is entirely about *when*
the flag is cheap versus expensive, not about whether singing mode itself
is P3 or P4 work: at the time of this decision `PhonemeDef` existed only
as a struct definition, no CSV, no generated header, no consumer, so
reserving a bit costs nothing. Once a phoneme CSV was hand-authored and in
use, adding a column would mean back-filling every row and regenerating
every downstream header.

## Speech Engine — `phonemes.h` Generator (#32)

`speechgen.py gen` parses `tools/speech_phonemes.csv` (48 phonemes —
vowels, fricatives, nasals, plosive closure/burst pairs, affricates,
approximants) into `phonemes.h`, failing loudly on any row that would
wrap a `uint8_t`/`uint16_t` rather than truncating silently.

Verification reuses `tools/host_render/render_speech.cpp`: it renders
every row to a WAV in one command, and for the vowels, cross-checks the
measured F1/F2 against `tools/host_render/vowel_reference.h` (generated
from `speech_vowel_reference.csv`, an independently-committed
published-values table — comparing a measurement against the same number
that authored it would prove nothing).

## Speech Engine — Segment Sequencer (#34)

P3 exit criterion: a hardcoded phoneme string is intelligible as a word.

`sequencer.h` implements the per-voice segment clock exactly as designed
(`k = min3(frames left, seg_remaining, SPEECH_SUBBLOCK)`, moved inside
`speech_render_voice_seq()`'s per-voice loop, `render.h`). Two hand-picked
utterances ("HELLO", "CAT" — `utterance.h`, not generated; text-to-phoneme
stayed P4 work) exercise variable per-segment duration, F/B ramping across
a phoneme boundary, and the plosive closure/burst pair.

Landed scoped to exactly what this exit criterion needed, deferring the
rest of the originally-sketched `VoiceParams`/`SpeechMode` design to P4:

- `SpeechMode` shipped with three values (`ONESHOT`/`GATED`/`LOOP`), not
  four — `SPEECH_HOLD` is represented structurally instead (see
  `module_speech.md`'s Architecture for how).
- Utterances were `utterance.h`'s hand-picked table (phoneme strings only,
  no phrase text), not a generated `phrases.h` — letter-to-sound and
  Program Change phrase selection stayed P4 work.
- `VoiceParams` gained exactly `utterance`/`mode`/`rate`; `jitter`,
  `shimmer`, `lfo_rate`/`lfo_depth` were still P4. `rate` existed and was
  exercised by the segment clock but wasn't CC-mapped yet — CC23 picked
  an utterance for on-device testing only.
- Note-off (#30) was implemented for all three shipped modes at this
  point, not just the `GATED` default.
- Malformed/empty utterance data renders silence rather than
  dereferencing it — the Underrun Policy extended to sequencer data, not
  just DMA/tick inconsistency; `render_speech.cpp` verifies by
  deliberately constructing one.

## Speech Engine — Letter-to-Sound (#35)

`speechgen.py gen-phrases` parses a plain-text phrase list
(`tools/speech_phrases.txt`, `NAME: word word word` per line) into
`phrases.h` (`enum SpeechPhraseId`, `SPEECH_PHRASES[]`, reusing
`sequencer.h`'s `SpeechUtterance` struct — the same shape as
`utterance.h`'s hand-picked fixtures — plus `SPEECH_PHRASE_TEXT[]` for
debugging/WAV-naming). Each word runs through `tools/nrl_rules.py`, a
curated few dozen ordered, context-sensitive rules (not the original NRL
report's ~400) plus a short whole-word exception list for irregular
high-frequency function words (THE, OF, ONE, ...) — a word the rules get
wrong is fixed with an attached override rather than by growing the rule
table to chase one exception. Every emitted phoneme symbol (rule-derived
or overridden) is checked against `speech_phonemes.csv`'s own symbol
column before `phrases.h` is written, so a typo fails the build instead of
emitting an out-of-range phoneme index. No rules engine, rule table, or
text parsing links into the device build — only the generated
phoneme-byte arrays do, same host/device split as `phonemes.h`.

Verification extends `render_speech.cpp` the same way #32 did:
`run_phrase_renders()` renders every entry in `SPEECH_PHRASES[]` to a WAV
in one command, checking the audio is finite, non-clipping, and that the
sequencer actually reaches completion — not that the pronunciation is
correct, which no host check can verify. The demo bank
(`tools/speech_phrases.txt`) is 6 phrases / 121 bytes in flash (73
phoneme bytes + 48 bytes of table).

**Blind intelligibility spot-check: was still pending at this point** —
needs a human listener; a host check can only confirm a phrase renders,
not that it sounds like the intended word. WAVs land in
`tools/host_render/build/speech_phrase_*.wav` for whoever does the listen.

## Speech Engine — Utterances and MIDI Integration (#36)

P4 exit criterion: a MIDI sequence plays a sung phrase at correct pitch.

`audio_engine.cpp` sequences `phrases.h`'s generated `SPEECH_PHRASES[]`
(#35) instead of `utterance.h`'s two P3 fixtures. Program Change and CC23
both select a phrase; CC28's phrase-bank mode maps note number to phrase
directly ("playing the words themselves" vs. Program Change's "playing a
line"). `SpeechMode`'s three values were already implemented by #34; this
added a live CC27 mode select so all three (plus the structurally-
represented `SPEECH_HOLD`) are reachable without a rebuild. `rate`/
`jitter`/`shimmer` (CC24-26) and the vibrato LFO (CC1/CC76) are new here —
see `module_speech.md`'s Excitation section for the DSP (vibrato resampled
once per sub-block; jitter/shimmer drawn once per glottal cycle, detected
by phase-accumulator wraparound). `utterance.h`'s HELLO/CAT fixtures
stayed in place for `render_speech.cpp`'s #34-era regression checks, which
need their exact known phoneme strings rather than whatever
`speech_phrases.txt` currently contains.

This also repointed Program Change from phoneme selection (its #28/#29
job) to utterance selection, since a single PC message can't mean both —
CC20 already covers phoneme selection for controllers (the project's
BeatStep Pro) that can't send real Program Change anyway.

Host-verified (`render_speech.cpp`): jitter/shimmer at zero measure as
exactly periodic (0.0 coefficient of variation on both cycle period and
peak amplitude) and visibly perturbed at max; vibrato produces a
measurable, sub-block-rate F0 swing; a generated phrase's measured pitch
tracks three notes spanning two octaves.

**MIDI-layer correctness had no host-side harness at this point** —
`midi_controller.cpp` has a pico-sdk dependency (`voice_alloc`/
`midi_parser`), unlike `render.h`, so it was only compile-verified (both
`ENGINE=speech` and `ENGINE=speech SPEECH_PROFILE=1` build clean), not yet
confirmed by ear on real hardware, including the four-`SpeechMode`
by-ear check the original acceptance criteria called for.
`PHONEME_FLAG_SUSTAINABLE` stayed reserved but unread — true singing mode
is a real gap against the original design sketch, but wasn't in this
step's own acceptance criteria, so deferred rather than silently dropped.

## Speech Engine — Display + Per-Voice Telemetry (#37)

Closes `module_speech.md`'s former "what does the LCD show for a speech module"
open question with the answer the doc itself predicted: current phoneme plus a
formant-space plot.

`display.cpp` replaces the #27/#28-era single PHON row (last note-on's channel
program only) with a per-voice phoneme grid (one cell per voice, `MAX_VOICES`
up to 8) and an F1/F2 formant-space plot, oriented like a conventional IPA
vowel chart (F1 increasing downward, F2 increasing leftward). Both read
`SpeechVoiceUiState` (`engine.h`), published once per render buffer by
`audio_engine.cpp` from each voice's live, ramped `SpeechVoice::F[0]`/`F[1]`
— the same values the tract renders from, not the phoneme's static target, so
a plotted dot moves continuously as a segment glides. Redraw is ~10 Hz
(Core 0, `display_task()`), well below segment rate and with no effect on
Core 1's render deadline.

**Hardware-verified on real `breadboard_rp2350`:** 8 simultaneously-held
phrase voices plus reverb hold steady at 31% Core 1 load — consistent with
#31's flat ~93.5 c/f/voice (22% for 8 voices) plus reverb's own measured
overhead, with no additional cost from the display telemetry itself (it reads
already-published per-voice state on Core 0, off the audio path entirely).

## Speech Engine — Presets, Effects Wiring, CMake Fix (#38)

P5 exit criterion: module is playable and ships.

Delay/reverb linking and per-voice pan turned out to already be in place —
both shipped with the #27 skeleton and the tracker's stereo-output work
respectively, before any P3/P4 sequencer or MIDI-mapping code existed, and
both were already hardware-verified (#28's closing note: "notes, pan,
vowel select, both delay and reverb" confirmed on `breadboard_rp2350`).

**Overdrive was never actually part of "the existing effects chain"
anywhere in the codebase.** The plan for this step assumed it was — its
own wording was "route through the existing delay/reverb/overdrive chain
... nothing about it needs rework, just wiring" — but checking found
`EffectType` (`engine_base.h`) only ever had `FX_DELAY`/`FX_REVERB`;
"delay/reverb/overdrive" traced back to an *Open Question* in
`module_tracker.md` ("the existing delay/reverb/overdrive could run as a
stereo send" — itself hypothetical), and separately to the groovebox's
unrelated per-voice 303 ladder-filter `drive` parameter, a different
mechanism with the same name. Adding a real `FX_OVERDRIVE` would mean
extending `EffectType` and CC74's band-select in the shared
`engine_base.h`, changing CC74's behaviour for the subtractive and
groovebox engines too — a cross-engine feature, not speech-specific
wiring. Decided to defer it and document the gap rather than build a
shared three-engine feature inside a speech-scoped step.

**What actually landed:** `presets.h` (`SpeechPreset`,
`voice_apply_preset()`, 9 presets — one per `SpeechMode` (`SPEECH_HOLD`
via the phoneme keyboard, `ONESHOT`, `GATED`, `LOOP`), robotic, breathy,
tract-shift up/down, and a robot-chorus preset spreading up to
`MAX_VOICES` simultaneously-held notes across the stereo field with a
small per-voice detune), CC16 preset select (`midi_controller.cpp`), and
a CMake fix.

**The CMake fix:** `src/controller.cpp` (the VGA-board 3-button demo) was
gated in `CMakeLists.txt` on "does this engine's directory contain a
`presets.h`" — true only for the subtractive engine until this step gave
speech one too, which pulled the demo into the speech build and broke it
(the demo is hardcoded to the subtractive engine's `VoicePreset`/
`WAVE_SAMPLE`/`osc_sample_phase_inc`, not a generic preset-button
interface). Re-gated on `T00T_ENGINE STREQUAL "subtractive"` — what the
demo's actual code dependency always was. No behaviour change for any
existing engine: subtractive still gets it, groovebox and tracker still
don't (they never shipped a `presets.h` either), speech now correctly
doesn't, matching its buttonless `breadboard_rp2350` target same as the
groovebox.

**Robot chorus** (per-voice pan spread + small detune, keyed by the
allocated voice slot, deterministic, no new state) was the payoff named
back when #31 raised `MAX_VOICES` to 8 — it's in the preset table as
`PRESET_ROBOT_CHORUS`.

Preset selection needed a structural decision: applying a preset directly
to a fresh voice at every note-on (the subtractive engine's exact
pattern) would mean per-field CCs (formant_shift/bandwidth_scale/jitter/
shimmer/rate/mode — CC21/22/24-27) stop affecting future notes the moment
a preset was selected once, silently regressing #29/#36's already-
hardware-verified "CC tweak also becomes the new per-channel default"
behaviour — something the subtractive engine's presets never contend
with, since none of its preset-owned fields have their own live CC.
Resolved by having preset selection (CC16, next-note-only) bulk-write the
same per-channel state those CCs individually own, routed through
`voice_apply_preset()` via a scratch `VoiceParams`, so the preset-to-field
mapping is still written in exactly one place.

Builds clean on all four engines (`make`/`make ENGINE=groovebox`/
`make ENGINE=tracker`/`make ENGINE=speech`, plus `SPEECH_PROFILE=1`).

**Not yet done at this point:** hardware listening confirmation of the
preset table (particularly the robot-chorus preset's stereo spread) and
the profiling-pin measurement of effects-on cost for the performance
table — both need the author at the bench, same as #36's MIDI-wiring gap.

## Speech Engine — LPC Lattice Tract Skeleton (#63)

#39's first slice: an order-10 all-pole lattice filter as a sibling to the
existing formant cascade, proven audible end-to-end before any corpus
converter exists — same skeleton-first order the FM engine already used
for its DX7 patch importer.

**`SpeechVoice`'s tract-specific state became a union.** Adding a second
tract without doubling per-voice RAM meant the formant cascade's fields
(`Res2p` array, F/B ramp state, `av`/`af`/`an`, `formant_shift`/
`bandwidth_scale`) had to move out of `SpeechVoice` into their own struct
(`FormantVoiceState`) so a `LatticeVoiceState` sibling could sit in a
union alongside it, selected by a new `SpeechVoice::tract` field. Fields
genuinely shared by both tracts (glottal phase, noise LFSR, the amplitude
declick smoother, vibrato/jitter/shimmer state) stayed on `SpeechVoice`
itself. A tract switch always happens on a note-on retrigger, so
`speech_render_voice()`/`_seq()`/`_lattice()` (render.h) each
placement-construct their own union member fresh at that edge before
writing into it — a voice that alternates tract note-to-note never reads
the other tract's leftover state.

**Finding: the segment sequencer needed a 3-line touch, not zero.**
`sequencer.h`'s `speech_seg_load()` calls `tract_retrigger()`/
`tract_snap_target()`/`tract_set_target()` directly on what used to be
`SpeechVoice&` — those now take `FormantVoiceState&`, so the union
refactor forced `speech_seg_load()`'s three call sites to pass `sv.fmt`
instead of `sv`. The sequencer's own logic (segment advance, mode
handling, duration calc) is unchanged; only the argument type at three
call sites moved. Recorded per #63's own acceptance criterion rather than
treated as a scope violation — "one new file plus a data pipeline" still
holds for `excitation.h` (untouched) and the sequencer's actual behaviour
(untouched), just not for every line that happened to reference the old
flat `SpeechVoice` layout.

**Coefficient interpolation**: lattice `k[i]` ramps linearly from the
current frame's value to the next frame's over
`SPEECH_LATTICE_INTERP_STEPS` (8) sub-blocks, computed once as a per-step
delta at frame load rather than re-approached every sub-block the way the
formant tract's exponential `TRACT_RAMP_COEFF` works — a genuine linear
ramp reaches the target exactly by the frame's last sub-block, matching
how the TMS5220 itself interpolated. Safe without re-deriving filter
coefficients (unlike the formant cascade) because the stable range
`|k[i]| < 1` is convex.

**Resampler**: `speech_render_voice_lattice()` doesn't take a
`native_frames` count like the other two render functions — it takes
`output_frames` directly and pulls 8 kHz native samples through a
linear-interpolation upsampler on demand, via a persistent fractional
accumulator (`LatticeVoiceState::resample_frac`) that survives across
render calls. A 10-second host render holding one frame confirmed no
measurable F0 drift between an early and a late analysis window.

**Test word**: no corpus converter exists yet (that's a separate, later
slice), so `lattice.h`'s `LATTICE_TEST_WORD` is a small hand-built
fixture — reflection coefficients from a real order-10 Levinson-Durbin
analysis (a throwaway Python script, not committed) of this module's own
`/i/`/`/a/`/`/u/` formant-cascade impulse responses, not hand-guessed
numbers. Every resulting `|k|` came out well inside (-1, 1) (largest
~0.90). The Levinson-Durbin gain is calibrated for a white-noise-driven
signal; this tract's glottal-pulse excitation has a much higher crest
factor and came out audibly quiet at the raw gain value (peak ~220 of
32767) — `SPEECH_LATTICE_GAIN_BOOST` (40x, tuned by re-running the host
render until the peak sat in the same ballpark as the formant tract's own
phonemes) compensates.

**CC102** (tract select, next-note, formant vs. lattice) is the one new
MIDI hook added — inside the CC102–119 range #39's spec already reserved
for LPC controls, and the cheapest way to let a real MIDI channel reach
the hardcoded test word at all, short of the full `KEY_PER_WORD`
addressing scheme #39 explicitly defers to a later slice.

Host-render harness (`render_speech.cpp`): renders the test word to WAV
(finite, unclipped, reaches completion), confirms a malformed word
(null/empty, mirroring the formant sequencer's own malformed-utterance
guard) renders exact silence, and the 10 s resampler-stability render
above. All three pass in both Debug (assert active) and Release builds.
Every pre-existing formant-path check in the same harness still passes
unchanged — the union refactor is a no-op for formant-tract behaviour.
Builds clean on all four engines (`make`/`make ENGINE=groovebox`/
`make ENGINE=tracker`/`make ENGINE=fm`) plus both speech variants
(`make ENGINE=speech`, `SPEECH_PROFILE=1`).

**Hardware-verified on real `breadboard_rp2350`:** CC102 >= 64 plus a
held note triggers the test word, confirming the tract-select field, CC
wiring, and the lattice render/resample path all work end-to-end on
device, not just in the host harness.

**Not yet done at this point:** the corpus converter, real word-selection
MIDI addressing, and per-voice LPC render-cost measurement are separate,
later slices of #39, not this skeleton.

## Speech Engine — Talkie TMS5220 Corpus Converter (#64)

The data pipeline #63 deferred: `tools/talkie2lattice.py` decodes the
TI-99/4A "Talkie" TMS5220 wordset's own bitstream format into #63's
`LatticeFrame` format (reflection coefficients, gain, pitch), following
`syx2patch.py`'s established gitignored-output shape.

**Licensing check before writing any table.** The Talkie library
(going-digital/Talkie, Peter Knight, GPLv2) is the reference decoder for
this bitstream format — a stricter, copyleft license than Dexed's
Apache-2.0, which `syx2patch.py`'s `DX7_ALGORITHMS` table was ported
from. Flagged to the author before writing any code: the bitstream
*format* (4-bit energy, 1-bit repeat, 6-bit period, 5/5/4/4-bit K1-K4,
conditionally 4/4/4/3/3/3-bit K5-K10, LSB-reversed-per-byte packing) is
almost certainly not copyrightable — it's the chip's own hardware
behavior, the same standing the DX7 sysex byte layout already has — but
the ~200 specific table values were a closer call. Resolved: re-express
the values as this project's own Python data (verified against Talkie's
`talkie.cpp` programmatically — hex-to-decimal via a script, not manual
transcription, to eliminate transcription risk across that many numbers
— not copied file text), attributed by comment. `_validate_tables()`
re-checks every table's shape and every reflection coefficient's `|k|<1`
range at import time, not just trusted on sight.

**Bit-level correctness proven two ways.** A `BitWriter` test helper
(the literal inverse of the real chip's bit-reversed packing) builds
synthetic bitstreams with known field values for every frame kind (normal
voiced, normal unvoiced — proving the bit cursor lands correctly after a
shorter K1-K4-only frame — rest, repeat, stop). Separately, the finished
converter was run against the real Talkie corpus (all six of the
library's own example vocab files, fetched for this session only, never
committed): **1173 words decoded, 0 skipped, 0 out-of-range coefficients,
26667 frames (~667 s of audio)** — close enough to #39's own "~1,163
words" estimate to confirm "the standard vocab files" meant all of them,
not one. The generated header was also compiled and run against the real
`lattice.h` types (not just Python-side checks), confirming the emitted
C++ is well-formed. None of that corpus data or the generated header
touched the repo — `talkie/` (source) and
`src/engines/speech/lattice_words.h` (generated) are both gitignored, so
this validation had to be redone from scratch, locally, and is not
reproducible from the commit history alone.

**Two deliberate deviations from `syx2patch.py`'s precedent**, both
recorded rather than silently diverging: a per-word decode failure is a
skipped-with-warning, not a whole-run abort (the Talkie corpus is roughly
35x a DX7 bank's voice count, so one bad word blocking all ~1,163 is a
worse failure mode at that scale); and the TMS5220's raw energy table
value is normalized to `LatticeFrame.gain` by straight division (÷255),
a different unit and calibration than #63's own `LATTICE_TEST_WORD` gain
values (Levinson-Durbin-derived, then re-scaled by
`SPEECH_LATTICE_GAIN_BOOST`) — real corpus playback will likely need its
own loudness pass once heard on hardware.

**Generic by construction, not by promise:** `ChipTables` carries every
TMS5220-specific piece (bit widths, all ten K tables, energy/period
tables) as data, and `decode_word()` takes a `ChipTables` instance as a
parameter rather than hardcoding TMS5220 values inline — adding a
TMS5100 (Speak & Spell) path later is "write a second `ChipTables`
instance," not a pipeline rewrite.

Test suite (`tools/test_talkie2lattice.py`, 14 checks): bit
reader/writer round-trip, table shape/stability, all five frame kinds
against hand-built synthetic bitstreams, vocab-source parsing (commented
and uncommented declarations, mixed hex/decimal byte literals), identifier
de-duplication, decode-failure-is-a-skip behavior, and generated-header
field-count regression lock. All pass with no corpus present; the
corpus-gated path (looking in `../talkie/`, gitignored) was confirmed
working both ways -- skips cleanly with a message when empty, and runs
real per-file checks once populated (proven with a throwaway synthetic
fixture during this session, not committed).

**Not yet done at this point:** `lattice_words.h` isn't generated or
wired into the build (no `talkie/` corpus is committed, by design); no
MIDI addressing mode exists yet to select a word from it even once it is;
the gain calibration above is a first pass, not hardware-confirmed.
