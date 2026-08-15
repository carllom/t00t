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

## Speech Engine — KEY_PER_WORD MIDI Addressing + Full LPC Corpus Playback (#65)

Wires #64's corpus converter into #63's lattice tract as real,
MIDI-playable vocabulary: `KEY_PER_WORD` addressing (note selects a word,
Program Change selects a 128-word page), GATED/ONESHOT/LOOP applied to
lattice voices, and a live pitch-shift override (CC103).

**Corpus fetched locally for verification, not committed** — same
"fetched for this session only" precedent #64 already set. All six
`going-digital/Talkie` example vocab files converted cleanly to
`lattice_words.h`: 1173 words, 0 skipped, 26667 frames, matching #64's
own earlier run exactly. Neither `talkie/` nor `lattice_words.h` touched
the repo.

**`VoiceParams` gained `lattice_word` (a `const LatticeWord *`, not an
index) and `lattice_pitch_shift`** (Q8.8). The pointer mirrors the FM
engine's own `patch` field — always valid (defaults to
`&LATTICE_TEST_WORD`), whether or not `lattice_words.h` exists locally.
`midi_controller.cpp`'s `speech_lattice_word_for_key()` resolves note +
the channel's current page to a `LATTICE_WORDS[]` entry behind
`T00T_SPEECH_HAS_LATTICE_WORDS` (a new CMake gate, same shape as `#47`'s
`T00T_FM_HAS_PATCHES`); without it, every key and every page plays
`LATTICE_TEST_WORD`. Program Change becomes tract-dependent: under the
LPC tract it writes `channel_lattice_page` instead of
`channel_utterance`, selected by the same `channel_tract` CC102 already
set — no new CC needed, no cross-talk between the two meanings.

**GATED/ONESHOT/LOOP reached the lattice tract with a 3-line touch to
`lattice.h`**, not a rewrite: `lattice_load_frame()`/`lattice_advance()`
gained a `pitch_mult` parameter (applied to `pitch_hz` before it becomes
a phase increment) and `lattice_advance()` gained a `loop` bool instead
of a `SpeechMode` parameter — `lattice.h` sits below `sequencer.h` in the
include graph (`tract.h` includes `lattice.h`, `sequencer.h` includes
`tract.h`), so `SpeechMode` itself can't be named there without a cycle.
`render.h`'s `speech_render_voice_lattice()` computes
`mode == SPEECH_MODE_LOOP && gate` and passes the bool through, and
handles the GATED note-off jump inline (mirrors
`speech_render_voice_seq()`'s own inline release-jump, not a
`sequencer.h` function either). The release point is always the word's
own final frame — Talkie's format has no release marker, and the
decoder already guarantees every word ends in genuine silence, so no new
field was needed on `LatticeWord`.

**Real corpus playback clipped badly at the old `SPEECH_LATTICE_GAIN_BOOST`
(40.0)** — 1019/1173 words overshot full scale, some by 2x. Root cause:
the constant was tuned in #63 against `LATTICE_TEST_WORD`'s
Levinson-Durbin-derived coefficients (largest magnitude ~0.90), but real
chip-recorded speech has reflection coefficients much closer to the unit
circle, so the same excitation scale drives a far more resonant filter.
Retuned empirically against the actual local corpus render (binary search
over the boost constant, rebuilding and checking the worst-case peak
across all 1173 words each time): 2.5 leaves the worst word at 29960
(comfortable margin under 32767) while every other host-render check
still passes. Consequence, recorded rather than silently accepted:
`LATTICE_TEST_WORD` itself is now much quieter (peak 546, was audible at
a much louder relative level before) — a known tradeoff of one shared
constant covering both a hand-built fixture and real chip data, not a
regression to chase further in this slice.

**Host-render harness extended** (`render_speech.cpp`): a
`render_lattice_native()` helper (mirrors `render_utterance_native()`'s
shape, but calls the output-rate `speech_render_voice_lattice()`
directly); `run_lattice_mode_checks()` (a synthetic 8-frame word proves
GATED's release-jump completes roughly one frame period after note-off
where ONESHOT ignores it and LOOP restarts, then degrades to one-shot
once gate drops); `run_lattice_pitch_shift_check()` (0.5x/1.0x/2.0x
measured against target F0 by Goertzel, within 5%); and, gated behind
`T00T_SPEECH_HAS_LATTICE_WORDS`, `run_lattice_corpus_render()` (every
corpus word rendered through the exact device render path, written to
`lpc_words/*.wav`, checked finite/unclipped/completed) and
`test_lattice_corpus_stability()` (every corpus frame's `|k[i]| < 1` and
`gain` in range, checked directly against the compiled `LATTICE_WORDS[]`
struct data — independent of `talkie2lattice.py`'s own Python-side
validation, which already checks the same property before emission).
Full corpus run: 1173/1173 words finite, unclipped, and completed; 26667
frames all within range. All pre-existing checks in the file still pass.

Builds clean in all four configurations that matter here: the device
firmware with and without a generated `lattice_words.h` present
(`T00T_SPEECH_HAS_LATTICE_WORDS` gate verified both ways, including a
from-scratch CMake reconfigure), plus the default subtractive-engine
build and the `SPEECH_PROFILE=1` variant, to confirm the `VoiceParams`
field additions don't disturb anything outside the speech engine.

**Not yet done at this point:** hardware verification — everything above
is host-render-verified only; hearing real words play from a keyboard,
confirming page-select and the pitch-shift CC by ear, and a per-voice LPC
render-cost measurement are all still open. The "musical" and two-CC
pseudo-program-select addressing modes module_speech.md's Decision Record
leaves room for remain unbuilt.

**Hardware-verified on real `breadboard_rp2350`:** Carl confirmed
`KEY_PER_WORD` addressing working end-to-end across multiple corpus
pages, hearing both male and female voices from the converted vocabulary.
Program Change page-select and CC102 tract-select both confirmed working
from a BeatStep Pro (whose virtual keyboard spans MIDI notes 24-108, not
the full 0-127 range `KEY_PER_WORD` addresses). Two things noted for
later, not acted on in this slice: playback is quieter than the old
hardcoded test word by design (`SPEECH_LATTICE_GAIN_BOOST`'s corpus
retuning above), and Carl described the voice quality as "almost too
smooth and intelligible" for his taste, wanting more of the real
TMS5220's buzzy character — likely reachable via a lattice-tract-only
excitation shape (`excitation.h`'s `glottal_pulse()` is currently shared,
unchanged, with the formant tract), deferred rather than built.

## Speech Engine — LPC Preset + CC16 Tract Selection (#66)

Wires #65's `SpeechTract`/`KEY_PER_WORD` page/pitch-shift fields into the
preset table, so a single CC16 program change can put a channel into LPC
corpus playback instead of requiring CC102 + a separate page-select PC.

**`SpeechPreset` gained three fields** (`tract`, `lattice_page`,
`lattice_pitch_shift`) and every one of the nine existing rows was
updated to set `tract = SPEECH_TRACT_FORMANT` explicitly, rather than
leaving a newly-added struct member to whatever its default-initialized
value would be. `voice_apply_preset()` writes `tract`/`lattice_pitch_shift`
into `VoiceParams` the same unconditional way it already writes every
other preset field; `lattice_page` bypasses it (it's channel-level
addressing state, not a `VoiceParams` field) and gets read straight from
the preset row by `midi_controller.cpp`'s `speech_load_preset()`, the
same way that function already reads `chorus`.

**New preset, `PRESET_LPC_WORDS`** (index 9): `SPEECH_TRACT_LATTICE`,
page 0, pitch-shift neutral (256). Its formant-only fields (utterance,
phoneme, formant_shift, bandwidth_scale, jitter, shimmer, lfo) are left at
the same neutral values `PRESET_PHONEME_KEYBOARD` uses — unread by the
lattice render path, but a channel that switches back to a formant preset
afterward shouldn't inherit anything unusual from them either.

**`midi_controller_init()`'s per-channel tract/page/pitch-shift
initialization became redundant** once `speech_load_preset()` started
bulk-writing those three fields: loading `PRESET_PHONEME_KEYBOARD` at
power-on already sets them to the same values the explicit lines used to.
Removed the now-dead lines rather than leaving two paths that have to
stay in sync by hand.

**No new host-render coverage**: `presets.h` includes `engine.h`, which
pulls in `engine_base.h` and `hardware/sync.h` — a real pico-sdk
dependency the standalone host build has no access to (the same
constraint `render.h`'s own header comment already documents for why it
stays pico-sdk-free). Preset-table logic has never been host-tested for
this reason; verified here by full device-firmware compilation instead
(`make ENGINE=speech` and the default engine, both clean), consistent
with the project's existing MIDI-layer testing boundary.

**Not yet done at this point:** hardware verification that selecting
`PRESET_LPC_WORDS` from CC16 actually switches a channel's tract cleanly,
and that switching back to a formant preset afterward leaves no audible
trace — implemented and reasoned through against the union
placement-new-on-tract-switch mechanism #63 already built and
hardware-verified, but not itself re-confirmed by ear.

**Hardware-verified on real `breadboard_rp2350`:** Carl confirmed CC16
toggling cleanly between `PRESET_LPC_WORDS` and a formant preset, with no
stale tract state carried across either direction.

## Speech Engine — LPC Per-Voice Cost Measurement Rig (#67)

A human-in-the-loop decision gate (real hardware, real profiling pin --
not resolvable by an agent alone): what the lattice tract's per-voice
render cost actually is, and whether `MAX_VOICES` (currently one shared
pool of 8 across both tracts) needs to change. What's buildable ahead of
that measurement is the rig itself; the measurement, comparison, and
final `MAX_VOICES` decision are still open, waiting on Carl at the bench.

**Extended the existing `SPEECH_PROFILE=1` rig rather than building a
second one.** `ProfilePhase` gained a `tract` discriminator
(`PROFILE_FORMANT`/`PROFILE_LATTICE`); five new phases (idle/1/2/4/8
lattice voices) were appended to the same `PROFILE_PHASES` array the
formant phases already use, so the whole state machine (buffer clearing,
pin toggling, EMA load calculation, phase-hold timing) stays one code
path instead of two. `phoneme`/`recompute_only`/`sweep_tract` are simply
unused on lattice rows -- no second phase-descriptor type was worth it
for three dead fields.

**Found and fixed a measurement-validity bug before it ever reached a
real reading:** `LATTICE_TEST_WORD` is 325 ms; the phase hold is ~4 s.
Rendered under `ONESHOT`-style ignore-note-off (the first thing tried),
each lattice voice would finish the word about 8% into the phase and
spend the remaining ~92% in its post-completion ring-down state -- which
skips `lattice_advance()`/`lattice_advance_subblock()` entirely (the
frame-remaining sentinel that marks a word done never satisfies the
sub-block modulo check again), so the pin would mostly be measuring the
cheaper tail state, not steady playback. Fixed by rendering with
`SPEECH_MODE_LOOP` and a permanently held gate, so every phase's voices
cycle through the word's frames for the entire hold -- caught by tracing
through render.h's own completion logic before ever flashing hardware,
not by a wrong number coming back from the bench.

**Verified by compilation only** in three configurations
(`SPEECH_PROFILE=1`, the plain speech engine, and the default
subtractive engine) -- there's nothing for the host-render harness to
check here; a profiling rig's entire output is a GPIO pin's timing, which
only exists on real silicon.

**Not yet done at this point:** everything the acceptance criteria
actually gate on -- the real cycles/frame/voice and %-Core-1 numbers at
1/2/4/8 lattice voices, comparing them against the formant tract's own
(~93.5 c/f/voice, flat 1-8v), and the resulting `MAX_VOICES` decision
(and its rationale) for the lattice tract, including whether the current
shared-pool-of-8 design holds up or needs to change. All require Carl at
the bench with the profiling pin.

**Measured on real `breadboard_rp2350`.** Carl read the profiling pin's
duty cycle by ear-and-scope across a full 13-phase cycle (~52 s) and,
usefully, matched each phase to what he heard from the speaker --
sustained-vowel/fricative buzz for the formant phases, `LATTICE_TEST_WORD`
(heard as "biam", its SIL-/i/-/a/-/u/-SIL shape looped) for the lattice
ones, silence for both idle phases and the recompute-only phase (which
never writes to `dry_l`/`dry_r` at all, so silence there is expected, not
a bug) -- which let every one of the 13 readings be identified with
confidence purely from the phase order, without needing a second
instrumented run.

Converting duty cycle to cycles/frame/voice (`duty% * BUF_PERIOD_US *
150 MHz / (256 output frames * voice count)` -- the same formula the
existing ~93.5 c/f/voice number already implies, "frame" meaning an
output-rate frame in both tracts' case, not either one's own native
rate):

| phase | reading | c/f/voice |
|---|---|---|
| formant 1/2/4/8v | 2.75/5.5/11.0/22.0% | 93.54, flat -- exact match to the historical number, confirming no regression |
| formant 4v PH_Z (voiced fricative) | 11.0% | 93.54 -- same as plain 4v, fricative branch free |
| formant 4v recompute-only | 1.3% | 11.05 -- coefficient recompute is ~12% of the per-voice cost |
| formant 4v swept formant_shift/bandwidth_scale | 11.0% | 93.54 -- same as static, live sweep free |
| lattice 1/2/4/8v | 2.3/4.55/9.1/18.1% | ~77.5, flat |

The lattice tract came in **cheaper than the formant tract** (~77.5 vs.
~93.5 c/f/voice), not more expensive -- the order-10 lattice recursion's
own per-sample cost is real, but its 8 kHz native rate (vs. the formant
tract's 22.05 kHz) means fewer native samples get processed per output
frame, and that more than pays back the difference once normalized the
same way the formant number already is.

**Decision: `MAX_VOICES` stays 8, shared between both tracts, unchanged.**
Since the lattice tract measured cheaper, not more expensive, the shared
pool's worst-case Core 1 cost is already bounded by the formant tract's
own already-characterized numbers (22% @ 8v, ~30% with reverb) regardless
of which tract fills the pool -- there's no headroom problem the
measurement reveals, and giving the cheaper tract a separate, larger
budget would need a real architecture change (per-tract voice pools) that
nothing here justifies. Carl confirmed this reading and the decision.

Acceptance criteria all met: cost measured, compared against the formant
tract, `MAX_VOICES` decided and recorded, no regression confirmed.

## Speech Engine — Velocity Toggle (CC15)

Carl's own spec, given directly rather than through an issue: a per-channel
CC (15, next-note, default on) that lets a controller with unreliable or
undesired velocity sensitivity force every note to sound at max velocity
instead. 0-63 disables velocity (every note-on computes `amplitude` from a
fixed 127 instead of the received value); 64-127 restores normal
velocity-sensitive behavior.

`channel_velocity_enabled[NUM_CHANNELS]` bulk-defaults to `true` in
`midi_controller_init()`, same as `channel_pan`/`channel_phrase_bank` --
not routed through the preset table, since it's a performance/controller
concern, not part of what a preset says a voice should sound like.
`ui_state.last_velocity` still always shows the raw received value
(diagnostic display of what the controller actually sent), independent of
whether it ends up affecting `amplitude`.

Verified by device compilation only (`make ENGINE=speech`) -- pure
MIDI-CC-to-per-channel-state wiring, the same testing boundary every
other CC handler in this file already has (no host-render coverage,
hardware-verification pending).

## Speech Engine — Switchable LPC Chirp Exciter (CC104)

Carl's follow-up to the earlier hardware listen (recorded in the
`KEY_PER_WORD`/preset entries above): the LPC corpus sounded "almost too
smooth and intelligible" -- closer to clean synthesized speech than the
real TMS5220's buzzier, more electronic character. Asked whether a
different exciter could get closer to the real chip, switchable against
the existing one.

**Researched, not guessed.** `excitation.h`'s `glottal_pulse()` (a smooth
bipolar triangle) is shared unchanged between the formant and lattice
tracts, tuned for the former. Real TMS5220 hardware instead drives its
lattice filter from a stored "chirp" ROM table -- fetched and
cross-referenced against two independent MAME source trees rather than
relying on memory: the current `mame/src/devices/sound/tms5110r.hxx`
(`TI_LATER_CHIRP`, the table used by the TMS5110A/TMS5200/TMS5220 family,
labeled "decap-verified") and the older historic-mame single
`chirptable[]`. The two didn't match -- and shouldn't: the historic-mame
value turned out to be `TI_0280_PATENT_CHIRP`, the *earlier* TMS5100/
TMC0281's own table, used as an (inaccurate, by the modern source's own
account) stand-in before MAME's TMS5220 emulation split chirp tables per
chip variant. Confirming which table belongs to which chip, rather than
taking the first match, is what made this usable: `TI_LATER_CHIRP` --
`{0x00,0x03,0x0f,0x28,0x4c,0x6c,0x71,0x50,0x25,0x26,0x4c,0x44,0x1a,0x32,
0x3b,0x13,0x37,0x1a,0x25,0x1f,0x1d,0,0,...}` (52 entries, 31 trailing
zeros) -- is the one for this tract's actual target chip.

Also fetched: exactly how the real chip uses the table, since a wrong
mechanism would misrepresent the data even with the right values. Per
the fetched source, `m_pitch_count` (reset on every pitch-period
boundary) indexes the table directly each sample, held at its last
(zero) entry once the period outlasts 51 samples -- a short burst at
period start, silence for the rest. That's architecturally different
from `glottal_pulse()`'s continuous phase-fraction lookup, so
`lattice_chirp_pulse(sample_in_period)` takes a discrete per-sample
index instead of a phase; `LatticeVoiceState::chirp_idx` tracks it,
reset on the same glottal-phase-wraparound detection the excitation loop
already used for jitter/shimmer draws. Normalized by the table's own
peak (0x71 = 113), not an assumed full-scale 127, so the returned shape
matches `glottal_pulse()`'s existing [-1,1] contract without rescaling
downstream gain.

**Switchable, not a replacement**: CC104 (live, unlike most LPC-specific
CCs' next-note default -- see Decision Record for why live is safe here),
`VoiceParams::lattice_chirp_exciter`, threaded through
`speech_render_voice_lattice()`, `SpeechPreset`, and
`midi_controller.cpp`'s per-channel state the same way CC103's pitch
shift was. The formant tract's own `glottal_pulse()` usage is completely
untouched.

**Host-render regression lock, and three iterations to get the
measurement right, not the DSP.** `run_lattice_chirp_exciter_check()`
checks two things: pitch tracking (the chirp exciter should reproduce
the same F0 as `glottal_pulse()` -- only the in-period shape should
differ) and crest factor (the chirp exciter should be measurably
peakier, the actual audible claim). Crest factor worked on the first
try (2.84 vs. 6.22 -- confirmed, over 2x). Pitch tracking took three
wrong measurements before landing on a correct one, each ruled out by
reasoning rather than accepted at face value:
- `local_peak_freq()`'s +-150 Hz spectral search read 260 Hz (2x F0) --
  the chirp burst's harmonic-rich, DC-heavy spectrum genuinely carries
  strong energy at 2xF0, which a narrow search around the target
  legitimately finds first.
- A zero-crossing count read 195 Hz (1.5x F0) -- the same harmonic
  richness adds spurious crossings within one true period that survived
  the debounce gap.
- A first-pass autocorrelation read 65 Hz (0.5x F0, for *both* exciters,
  including the already-known-correct `glottal_pulse()` case -- the
  tell that this one was the test's bug, not the DSP's) -- an unbounded
  [0.5x, 2x] lag search hit the classic autocorrelation octave error,
  where correlation at 2x the true period can outscore the true period
  for a clean, low-noise periodic signal; a second bug (comparing raw
  sums with different term counts per lag) compounded it.

Fixed by narrowing the autocorrelation search to +-15% around the
already-known target period (this checks reproduction of a known pitch,
not blind detection of an unknown one) and fixing the term-count bias.
Final reading: `glottal_pulse` 129.7 Hz, chirp 124.9 Hz, both within 5%
of the target 130 Hz.

Full suite re-run after: all pre-existing checks still pass, including
the full local corpus (1173/1173 words finite/unclipped/completed) --
the new exciter doesn't push any real corpus word over the gain ceiling
`SPEECH_LATTICE_GAIN_BOOST` was retuned against. Device firmware verified
in three configurations (plain speech engine, `SPEECH_PROFILE=1`, and
the default subtractive engine).

**Not yet done at this point:** hardware verification that the chirp
exciter actually sounds like the intended "closer to the real chip"
character, and the unvoiced/fricative source's own real-chip LFSR
(coarse two-level noise, specific tap polynomial) -- deliberately left
out of this slice, see Decision Record.

**Hardware-verified on real `breadboard_rp2350`:** Carl confirmed the
chirp exciter (CC104 >= 64) sounds "age accurate" -- more robotic than
the default triangle, with clearer pronunciation as a side effect. The
decap-sourced table and the crest-factor-based character claim both hold
up by ear, not just by measurement.

## Speech Engine — TMS5220 Unvoiced Noise, Paired With the Chirp Exciter

Carl's follow-up after confirming the chirp exciter by ear: also try the
real chip's own unvoiced/fricative noise generator, paired with the
chirp table under the same CC104 for now rather than a second CC --
listen to both together first, decide afterward whether unvoiced needs
its own separate toggle.

**Same research discipline as the chirp table.** Fetched the exact
generator from the same source already cross-checked for the chirp data:
a 13-bit shift register (seeded `0x1FFF`, the chip's own reset value,
never reseeded again except at power-on -- mapped here to this voice's
own lattice-state construction), updated 20 times per native sample
(oversampling-by-decimation, whitening what a slow-clocked 13-bit
register alone would render as an audibly patterned buzz), taps at bits
12/3/2/0 -- the same positions confirmed for the chirp table's own chip
family. Only the register's low bit after those 20 updates is read, and
only to pick between two *fixed* levels (+64/-64), not to produce a
multi-bit noise value -- coarse by design, not merely reused wiring
around a different scale. `lattice_chip_noise(uint16_t &rng)` reproduces
this; `LatticeVoiceState::tms_rng` carries the register, reset alongside
every other lattice-only field in `lattice_reset()`.

**Normalization matched to the chip's own relative scale, not
independently maxed.** The real chip runs chirp-table values and this
fixed ±64 through the identical final left-shift before the lattice
filter (the same `m_excitation_data` variable serves both paths in the
fetched source), so dividing both by `LATTICE_CHIRP_PEAK` (113, the
chirp table's own peak) rather than giving noise its own independent
`±1.0` keeps their real relative loudness intact: the noise floor
genuinely sits below the chirp burst's own peak by design, not by
accident of two separately-normalized constants.

**A new, targeted host check caught a real clipping regression before
it shipped.** `run_lattice_chirp_exciter_check()`'s existing crest-factor
number (glottal_pulse 2.84, chirp 6.22, over 2x) was itself the warning
sign: `SPEECH_LATTICE_GAIN_BOOST` was calibrated against the *default*
exciter's crest factor, and nothing yet confirmed the real corpus stayed
safe under the *other* one. Re-running `run_lattice_corpus_render()`
against all 1173 words with `chirp_exciter=true` (added as a
WAV-free, peak-only variant of the same sweep rather than doubling the
suite's runtime/disk cost permanently) found exactly that: two words
("O", "OH") clipped at 33666, just over the 32767 ceiling. Fixed with a
second, chirp-only multiplier (`SPEECH_LATTICE_CHIRP_GAIN_SCALE`, 0.86,
tuned the same empirical rebuild-and-measure way as the original
`GAIN_BOOST`) rather than lowering the shared constant and quieting the
already-correctly-calibrated default exciter for a problem specific to
the other one. Re-run confirms both exciters now land at comparable
worst-case peaks (29960 default, 28952 chirp) with matching margin.

**Regression lock**: `test_lattice_chip_noise_two_level()` calls
`lattice_chip_noise()` directly 100,000 times and confirms every value
is exactly one of the two expected levels (not a third, which would mean
a tap or masking bug) and that both appear roughly evenly (not stuck on
one, which would mean a frozen or non-toggling register) -- unit-level,
independent of the lattice filter or resampler, the same shape as the
chirp exciter's own pitch-tracking/crest-factor check but for a claim
("exactly two values") that's exact rather than statistical.

Full suite re-run clean after the gain fix (0 failures, both corpus
sweeps passing). Device firmware verified in three configurations (plain
speech engine, `SPEECH_PROFILE=1`, default subtractive engine).

**Not yet done at this point:** Carl's own by-ear listen to decide
whether the unvoiced noise should get its own independent CC rather than
staying paired with the chirp table's own toggle.

## Speech Engine — S.A.M. Tract Skeleton (#70)

#69's first slice: three independently-driven formant resonators, summed
in parallel rather than chained, as a third sibling to the formant
cascade and LPC lattice tract -- proven audible end-to-end before any
reciter or allophone-table import tool exists, the same skeleton-first
order #63 already used for the LPC lattice tract.

**Union grew a third member, not a fourth flat field.** `SpeechVoice`'s
tract-state union already held `fmt`/`lat`; `sam` (`SamVoiceState`,
`sam.h`) joins them as a third mutually-exclusive variant, sized like the
others so 8 voices' worth of tract state still costs the largest single
variant, not the sum of three. `speech_render_voice_sam()` follows the
same placement-construct-on-tract-switch rule the other two render
functions already established.

**Parallel, not cascaded.** Each of the three resonators is driven
directly by the glottal/noise source and summed independently
(`sam_process_mixed()`), rather than one resonator's output feeding the
next the way the formant cascade's five stages do. A dedicated frication
resonator (mirroring the formant tract's own fricative branch) shapes the
shared LFSR noise excitation for unvoiced consonants, rather than
importing the original's short sampled PCM bursts -- #69's own scope
decision, avoiding a second class of imported proprietary audio data.

**No reciter or generated allophone table yet.** `sam.h`'s
`SAM_TEST_ALLOPHONES` is a small, hand-authored fixture -- silence,
three vowels, one fricative -- reusing the formant tract's own Peterson &
Barney (1952) F1-F3 reference values rather than guessed numbers. The
voice's existing `phoneme` field indexes it directly (wrapped
`% SAM_ALLOPHONE_COUNT`), the same field the formant tract's phoneme
keyboard already reads -- reused rather than duplicated, since only one
tract renders a given voice at a time.

**Headroom**: an initial `1/6` guess left `/s/` (`af=1.0`, the same
frication-branch target values as the formant tract's own `/s/` row)
peaking at 30767 of 32767 -- uncomfortably close to clipping at full
velocity. Reusing the formant cascade's own `1/12` headroom instead
(`SAM_EXCITATION_HEADROOM = SPEECH_EXCITATION_HEADROOM`) brought every
fixture entry comfortably under the ceiling (vowels ~4000-4500,
`/s/` 15383) without a separate constant to maintain.

**CC102** (tract select) changed from a two-way to a three-way band,
matching CC27's mode-select shape -- band 0 formant, band 1 LPC lattice,
band 2 SAM. The cheapest way to reach the new tract from a real MIDI
channel, short of any SAM-specific CC work #69 defers to a later slice.

Host-render harness (`render_speech.cpp`): renders every fixture entry to
WAV (finite, unclipped), and confirms an out-of-range allophone index
wraps rather than reading past the fixture table. All pre-existing checks
in the same harness still pass unchanged. Builds clean on all engines
plus both speech variants (`make ENGINE=speech`, `SPEECH_PROFILE=1`).

**Not yet done at this point:** hardware verification, the reciter and
allophone-table import tool, MIDI/CC integration beyond the CC102
bring-up hook, pitch contour, and per-voice cost measurement are separate,
later slices of #69, not this skeleton.
