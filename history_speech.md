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
