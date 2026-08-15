# T00T — Tracker Module: Development History

Dated build-phase results and measurement logs for the tracker synthesis
module. See `module_tracker.md` for the current spec/design.

---

## Tracker Engine (build skeleton, #13)

Third build-time engine (`make ENGINE=tracker`, `breadboard_rp2350` default),
proving the build seam and `module_tracker.md`'s deviations from the shared layer
model before any XM/mixer logic exists:

- `MAX_VOICES = 32`, defined in `src/engines/tracker/engine.h` ahead of its
  `#include "engine_base.h"`, per #10. Only this engine — subtractive and
  groovebox stay at 16.
- `voice_alloc` is not used: in XM, channel N is voice N (fixed assignment,
  no allocation/stealing/age tracking). `src/voice_alloc.cpp` is excluded
  from the link entirely (`CMakeLists.txt`'s `ENGINE_VOICE_ALLOC`, empty for
  `T00T_ENGINE STREQUAL "tracker"`); `main.cpp`'s calls into it compile out
  behind `HAS_VOICE_ALLOC` (defined `0` only for this engine).
- `fx/delay.h` / `fx/reverb.h` are not `#include`d by
  `src/engines/tracker/audio_engine.cpp`, so neither is linked — their
  combined ~128 KB of `.bss` must never compete with the tracker's future
  350-400 KB sample budget.
- `src/engines/tracker/display.cpp` and `midi_controller.cpp` are stubs (no
  UI, no note routing yet) so the shared `gfx.cpp` path and MIDI transports
  still link.
- Sound source: voice 0 is a hardcoded, always-on 440 Hz test tone (centre
  pan, through the shared stereo output tail) — a build/boot smoke test, not
  a mixer. Voices 1-31 are unused placeholders. `PROFILE_PIN` (GPIO 22) is
  bracketed around the render call, ready for the 32-voice measurement slice.

Measured with `arm-none-eabi-size` on a clean `rm -rf build && make
ENGINE=tracker`, and confirmed via `nm` that no `voice_alloc`/`FxDelay`/
`FxReverb` symbols appear in the binary:

| Engine | text | bss | dec |
|---|---|---|---|
| tracker (skeleton) | 25,328 | 10,284 | 35,612 |
| subtractive (default) | 206,040 | 198,344 | 404,384 |
| groovebox | 55,516 | 199,764 | 255,280 |

The subtractive/groovebox `.bss` figures are dominated by the sample corpus
and wavetables baked into those engines, not by `voice_alloc`/delay/reverb —
this skeleton has none of that yet. `make`, `make ENGINE=groovebox`, and
`make ENGINE=tracker` all build clean from a fresh `build/`; the first two
are unchanged in size from before #13.

## Tracker Engine — Performance Target (pre-measurement)

Baseline from `history_subtractive.md`: Voice A (Fairlight 8-bit sample,
linear interpolated) = 5.9%, i.e. ~200 cycles/frame. 32 of those would be
190% of Core 1 — the trap a tracker voice must avoid.

The 200 cycles are not sample interpolation. They are the surrounding
per-sample machinery: a float ADSR, a float-phase LFO with sine-table
lookup, the four-destination modulation chain, and `osc_sample_play()`
dispatch. A tracker voice needs none of it per sample — XM envelopes,
vibrato, tremolo, autovibrato and volume slides all update per tick.

Target for a stripped tracker voice: 25–40 cycles/frame = 0.7–1.2% each, so
32 channels ≈ 25–40% of Core 1, leaving room for a limiter or a global
stereo effect send. To verify with the existing profiling pin before writing
any XM logic.

RP2350 accelerators considered (to evaluate only if the target were missed):
M33 DSP extension (`SMULBB`/`SMLABB`, `SMLAD` for the Q15 mix); SIO
interpolator blend mode (computes `a + ((b - a) * alpha)` in hardware,
exactly the lerp — per-core, so Core 1 owns one outright with no
save/restore, though 8-bit alpha = 256 steps and the DSP instructions may
already be fast enough that the setup overhead isn't worth it). Neither was
evaluated in the end — see below, the headline number cleared the target
without them.

## Tracker Engine — 32-Voice Mixer (#15/#16)

Replaces the #13 test tone with the real stripped mixer from `module_tracker.md`
("Rendering Pipeline" / "Voice mixer"): `src/engines/tracker/mixer.h` is a
pure-integer, pico-sdk-free header (`TrackerSample`/`TrackerVoice`,
`mix_voice()`, `samples_to_loop_end()`, `wrap_loop()`,
`tracker_render_buffer()`) shared verbatim between the device engine and
`tools/host_render/render_tracker_mixer.cpp`, which proves ramp linearity,
loop-wrap bounds, one-shot end-of-sample, and nearest-vs-linear divergence
against exact expected values before anything touches hardware.

### #16 measurement rig

No display and no stdio on this build (USB is MIDI-only), so the profiling
pin is the only readout. `audio_engine_run()` self-cycles through 6 phases,
holding each 4 seconds, forever: idle (0 voices, isolates fixed per-buffer
overhead), 8, 16, 24, 32 voices (linear), 32 voices (`mix_voice_nearest()`).
Every voice loops forever — nothing goes inactive mid-phase — so each
phase's reading is stable for its whole hold time, unlike #15's demo (which
let one-shot voices decay away). Voice 0 is always a deliberately tight
4-sample loop (`samples_to_loop_end()`'s worst case: many short runs per
sub-block instead of one); the rest are the #15 chorus. Buffer size is a
build-time choice (`audio_common.h`'s `T00T_SAMPLES_PER_BUFFER`, overridden
via `make ENGINE=tracker DMA_BUFFER_SIZE=512`) so the idle number could be
re-measured at 512 for the IRQ-overhead comparison.

Measured on `breadboard_rp2350`, 2026-08-07:

| Buffer | Idle | 8v | 16v | 24v | 32v linear | 32v nearest |
|---|---|---|---|---|---|---|
| 256 | 0.52% | 8.20% | 15.3% | 22.8% | 30.1% | 20.1% |
| 512 | 0.52% | 8.20% | 15.3% | 22.8% | 30.1% | 20.1% |

Per-voice cost, `(duty - idle) / voice_count`, cycles/frame at 3401
cycles/frame = 100%:

| Voices | Linear | Nearest |
|---|---|---|
| 8 | 32.65 c/f (0.96%) | — |
| 16 | 31.42 c/f (0.92%) | — |
| 24 | 31.57 c/f (0.93%) | — |
| 32 | 31.44 c/f (0.92%) | 20.81 c/f (0.61%) |

Flat at ~31.5 cycles/frame/voice across 8/16/24/32 — including the tight-loop
stress voice at every point, so this is the honest worst-case number, not a
best case that degrades under load.

### Decisions (written per #16's acceptance criteria; full reasoning in `module_tracker.md` Settled Decisions)

- **32 channels**, not 16. 30.1% of Core 1 for the full complement, inside
  the 25-40 cycles/frame prediction and comfortably under the ≤50% goal —
  ~20 points of headroom left for a limiter or global effect send.
- **Linear interpolation, no nearest-neighbour build flag.** Nearest saves a
  real 33% (20.81 vs 31.44 c/f) and does audibly alias, as predicted, but
  linear already clears budget with room to spare — nothing forces the
  trade. `mix_voice_nearest()` stays in `mixer.h`, proven and ready if a
  later voice-count push needs it.
- **DMA buffer size stays 256.** Idle duty — the number that isolates fixed
  per-buffer/IRQ overhead — was identical at 256 and 512 (0.52% both), as
  was every other phase re-checked at 512. No measurable IRQ overhead at
  this sample rate means 512 would only add latency for zero offsetting
  benefit.
- DSP-extension / SIO-interpolator options (module_tracker.md's optional fallback)
  were not evaluated — only called for if the headline number missed
  target, and 30.1% is well inside it.

## xm2t00t Host Converter (#14)

### Validation

`python3 tools/xm2t00t/test_xm2t00t.py` — unit checks (period/effect/struct
sanity, the two rejection paths) always run; corpus-dependent checks run
against whatever `.xm` files are in `xm/` (gitignored — third-party
copyrighted modules aren't committed; populate it yourself, e.g. from Mod
Archive, and re-run) and skip cleanly if that directory is empty:

- Song structure (channels/orders/patterns/instruments/samples counts) vs
  both `openmpt123 --info`'s stdout and libopenmpt's C API directly
  (`openmpt_ref.py`, `ctypes` over `libopenmpt.so.0` — no Python binding
  needed), the latter also diffing the exact order-\>pattern list.
- Delta-decode round-trip: a second, independently-coded implementation of
  XM's delta decode (explicit signed arithmetic + wrap loop, vs. the
  converter's unsigned mod-256/65536 accumulate) compared byte-for-byte
  against the converter's own decode, over every sample in the corpus.
- Guard-byte correctness and full blob self-consistency (re-reading the
  emitted bytes with `blob_format.py` and diffing every header field, the
  order table, pattern row counts, and sample PCM against the source parse).

Verified against 5 real modules (2026-08-07): `118in64.xm` (18ch),
`bzl-hscr.xm` (16ch), `dubmood_-_mario_airlines_keygen_edit.xm` (12ch),
`kenny_beltrey_-_positrons.xm` (32ch — module_tracker.md's max), `records.xm`
(16ch). All match `openmpt123 --info` and libopenmpt exactly; all pass every
check above; all convert well under the default SRAM budget (93-241 KB of a
380 KB budget).

## Tracker Engine — Reference-Diff Harness and Ring Depth (#17)

Player *logic* (order/pattern walk, note triggering, `TickBlock`/`TickRing`
shapes) landed as `src/engines/tracker/player.h`, pulled forward so its
reference-diff harness had a real player to drive instead of a copy.

**Implemented.** `python3 tools/host_render/diff_xm.py` is the one-command
harness: for each module (the checked-in synthetic fixtures in
`tools/xm2t00t/xm_synth.py`, plus whatever real corpus is in `xm/`), it
converts to a blob, renders the device path via
`tools/host_render/render_xm_device` (the real `player.h` + `mixer.h`, not
a copy), renders the reference via `openmpt123 --filter 2` (linear
interpolation, matching `mix_voice()` exactly), and diffs the two WAVs.

- **Tolerance metric**: windowed RMS-in-dB (512-frame windows) relative to
  the reference's peak, computed after correcting for a measured global
  level scalar (`gain_ratio`, least-squares best fit) between our engine's
  raw vol/64-of-full-scale channel-volume convention and libopenmpt's
  default XM render level — a real, ~11-20 dB, and apparently
  song-dependent gap (`gain_ratio` varies a lot across the real corpus;
  worth its own investigation later, e.g. whether libopenmpt's default XM
  mix level applies headroom keyed off channel count) that is a
  level-*convention* question, not a correctness one, so it's reported
  rather than silently baked into the engine. Threshold is -10 dB, set
  empirically off the two synthetic fixtures (see `diff_xm.py`'s
  `THRESHOLD_DB` comment for the measurements): the only windows that come
  anywhere close are the ones straddling a note-on/retrigger boundary — two
  independently-implemented declick ramps settling to the same target along
  different sample paths — never a sustained region. A wrong note or
  dropped trigger replaces the signal with something uncorrelated at
  comparable amplitude (error at or above 0 dB), so -10 dB stays
  unambiguously on the correct side of that gap.
- **Divergence localization**: `render_xm_device` emits a per-tick CSV trace
  (frame → order/pattern/row/tick + any note triggers that tick); the first
  window that exceeds tolerance is mapped back to the nearest preceding
  trace entry, so a failure reads as "order 2 (pattern 5) row 12 tick 0,
  channels 3 and 7", not just a sample offset.
- **Regression corpus**: `tools/xm2t00t/xm_synth.py` builds two small,
  byte-valid `.xm` files from code (not checked-in binaries — `xm/` stays
  gitignored, real Mod Archive corpus, matching `test_xm2t00t.py`'s
  existing convention), deliberately staying inside the notes-only player's
  scope (center panning throughout — `pan.h`'s equal-power law vs whatever
  law FT2 actually used is a real, permanent, out-of-scope divergence for
  this harness, not a bug). Both pass at -10 dB. The real corpus is run and
  reported too, but only informationally: any module using effects or
  envelopes (i.e. nearly all of them) diverges the moment it needs one,
  which is exactly the point — later effect/envelope work gets a concrete,
  located baseline to work against instead of starting from nothing.

This slice also caught a real bug in `mixer.h` (#15/#16, already shipped):
`wrap_loop()` read/wrote `v->pos` while the loop that had just advanced
position was still holding the new value in a local — a wrap partway
through a multi-sample `mix_voice()` call (anything larger than one
sub-block, i.e. every real call from `tracker_render_buffer()`) discarded
the advance and wrapped from a stale position instead. Invisible to
#15/#16's own tests (`n == 1` per call in the loop-wrap test never reaches
the buggy branch; `test_full_mix()` only asserts aggregate stats), caught
immediately by a waveform-accurate reference diff. Fixed in `mixer.h`.

**Ring depth: 2 tick slots.** `player.h`'s `TickRing` picked the 2-slot
side of the tradeoff: 20 ms of slack per tick is sufficient, and the host
reference-diff harness that drives the ring today is single-threaded
(produce, then immediately consume — no lookahead benefit from more
slots). Nothing observed so far argues for 4.

## Tracker Engine — Cross-Core Wiring (#18)

Added the real cross-core wiring on top of #17's player logic: `TickRing`
head/tail became genuinely atomic, Core 0 gained `player_task.cpp` (song
load, resident SRAM sample table, ring priming/refill, play/stop/seek
transport driven by MIDI Start/Continue/Stop/Program Change), and Core 1's
`audio_engine.cpp` replaced the #15/#16 synthetic profiling rig with a real
ring-consuming render loop that renders silence on ring-empty. A small
synthetic demo song (`tracker_song_blob.h`, from `xm_synth.notes_basic()`)
auto-plays on boot; swapping in a real module from `xm/` for a listening
test is a one-command manual regenerate. It already sounds like music here.

**Ring atomics: `std::atomic<uint32_t>` head/tail**, not hand-rolled ARM
barriers. `push()`/`pop()` release-store their own index; `full()`/`empty()`
acquire-load the other core's — the standard SPSC pattern — chosen over
matching `ParamExchangeT`'s `__compiler_memory_barrier()` style specifically
because `player.h` must stay host-buildable (`tools/host_render` links it
with the host compiler, no pico-sdk headers allowed). The reverse multicore
FIFO (unused by the tracker's voice allocator — there isn't one) carries
Core 1's non-blocking "tick consumed" doorbell back to Core 0; Core 0's wake
cadence for draining it comes for free from `output.cpp`'s existing DMA IRQ
(~every 5.8ms at the default buffer size), so no new timer was needed.

## Tracker Engine — Effects (#19)

The effects covering ~90% of real usage: `0` arpeggio, `1`/`2` porta, `3`
tone porta, `4` vibrato, `A` volume slide, `C` set volume, `B` jump, `D`
pattern break, `F` speed/tempo. All ten land in `player.h`'s
`player_produce_tick()` as per-channel state machines, tick 0 vs later-tick
semantics enforced by only ever calling
`tracker_tick_period()`/`tracker_apply_tick_volume_effects()` when
`!row_boundary` (arpeggio's own tick-0 contribution is 0 either way, so
it's included in that same gate rather than special-cased). Pitch effects
needed real period math on Core 0 for the first time —
`tracker_note_to_period()`/`tracker_period_to_inc()` are a runtime C++ port
of `tools/xm2t00t/periods.py`'s linear/Amiga formulas (double-precision, not
fixed-point: 32 channels of `pow()` at ~50 Hz is nowhere near Core 0's
budget) — but a channel with no active pitch effect still latches straight
from the precomputed `note_increments` table, byte-identical to pre-#19
output. `B`/`D` share one row-level pending-jump/pending-break pair in
`PlayerState`, resolved at the row's last tick so "B and D on the same row"
(different channels) composes naturally: jump's order, break's row.

- **Effect memory is per-command, not global**: porta up/down and tone
  porta each keep a separate memory slot (`PlayerChannelState`), matching
  FT2 rather than sharing one. A continuous effect (porta, tone porta,
  vibrato, volume slide) must be restated every row it runs on — an empty
  effect column on a later row correctly stops it, it does not coast on
  the previous row's memory.
- **Verified against `openmpt123`** via one new synthetic fixture per
  effect in `tools/xm2t00t/xm_synth.py` (`tools/host_render/diff_xm.py`'s
  asserted set), plus targeted C++ unit tests in `render_xm_device.cpp`
  for state the audio diff can't cheaply pin down (exact `B`+`D` landing
  row, trigger-generation counter untouched by tone portamento, volume
  clamp/memory). The diff harness caught two real bugs no unit test
  would have: porta/tone-porta sliding pitch 4x too fast (a published
  FT2 pseudocode `*4` was double-applying a period-scale correction
  `periods.py`'s convention doesn't need), and arpeggio applying its two
  offset nibbles in the opposite order from `openmpt123`. It also turned
  up a latent mixer crash (division by zero) once pitch became runtime-
  computed instead of table-only: an unbounded portamento could round
  `inc` down to the format's own "channel silent" sentinel (0) for a
  voice that was still very much active — fixed by flooring
  `tracker_period_to_inc()`'s output above `mixer.h`'s Q8.24→Q18.14
  latch-shift's own floor.
- **Vibrato is the one deliberately-approximate case.** The sine table
  values and the overall mechanism (per-tick position advance, waveform
  lookup, sign flip at half-cycle, phase reset on retrigger) are real
  FT2 conventions, but the exact position-to-table-index rate constant
  was calibrated empirically (pitch-tracking a long held run against
  `openmpt123`) rather than sourced with certainty, because a continuous
  oscillation has no settling point to converge on the way porta/tone
  porta do — any small rate mismatch is permanent phase drift, not noise
  that damps out. `vibrato_basic`'s fixture is deliberately one gentle,
  short row for exactly this reason (see the fixture's own docstring);
  getting this bit-exact against libopenmpt was explicitly left to the
  long tail of FT2 quirks (#25), not this step's bar.

## Tracker Engine — Instruments, Envelopes, Key-Off (#20)

Multi-sample note→sample mapping and per-note relative-note/finetune were
already in place from #17-#19 (needed for pitch even before envelopes
existed); this step's actual new surface is `player.h`'s
`tracker_resolve_envelope_volpan()` (volume/panning envelopes with sustain
and loop, run every tick independent of any pattern effect),
`tracker_fadeout_tick()`, `tracker_autovibrato_delta()`, and the volume
column's remaining bands (fine/coarse volslide, panslide, vol-column
vibrato sharing the effect column's oscillator, vol-column tone porta with
its own coarse rate table). The host-side blob format (`InstrumentHeader`,
`EnvelopePoint`, envelope/autovibrato/fadeout fields) and the XM
parser/writer were already complete from #14 — this step was entirely
device-side (`player.h`) plus test fixtures. Ping-pong loops were split out
to #21 (see below).

- **Key-off does not cut a voice directly.** It only sets a per-channel
  `key_off` flag that the envelope/fadeout machinery consumes every tick
  from then on. Verified against `openmpt123` (not just FT2's own replayer
  source, which turned out to disagree with it on one point — see below):
  an instrument with an *enabled* volume envelope releases through it
  (continuing past its sustain point, plus fadeout once the envelope's own
  last point is reached); an instrument with **no** volume envelope at all
  cuts almost instantly on key-off, regardless of its fadeout field. FT2's
  own replayer source (ft2-clone, a byte-accurate port) applies fadeout
  unconditionally, envelope or not — an earlier version of this
  implementation followed that literally, and diverged badly against
  `openmpt123` (`fadeout_basic`'s fixture); `openmpt123` is this harness's
  oracle, so the device player follows it, not the DOS original, on this
  one point.
- **Envelope evaluation is a fresh interpolation each tick**, not FT2's
  own incremental Q8.8 delta-accumulation (which exists on 1990s hardware
  to avoid a per-tick division Core 0 has three million spare cycles for)
  — behaviourally equivalent for well-formed envelopes, which is the
  overwhelming majority of real content.
- **Volume-column commands are a second, independent active-effect
  slot** (`active_vol_effect`/`active_vol_param`, own memory), since XM
  allows an effect-column and volume-column continuous effect on the
  same row simultaneously. Vol-column vibrato/tone-porta share state
  (oscillator position, glide target) with their effect-column
  equivalents; when both columns target the same mechanism on one row
  (a pathological, essentially never-authored case) the effect column
  wins rather than double-stepping it.
- **Verified against `openmpt123`**: new fixtures in
  `tools/xm2t00t/xm_synth.py` for volume/panning envelopes (sustain,
  loop, the panning envelope's pan-dependent asymmetric-swing formula),
  fadeout, and the volume column's level/pan bands all diff clean.
  Vol-column vibrato and tone portamento are continuous pitch
  oscillators/glides, same category as the effect-column vibrato (#19:
  "not chased to bit-exactness... any small rate mismatch is permanent
  phase drift") — their *mechanism* (no retrigger, oscillates/glides,
  shared state) is instead pinned down with C++ unit tests in
  `render_xm_device.cpp`, matching that precedent rather than fighting
  it. Autovibrato is the same story and is covered the same way (a
  dedicated sweep/freeze/no-op unit test; not asserted in the audio
  diff harness).

## Tracker Engine — Ping-Pong Loops and Sample Offset (#21)

`mixer.h` gains a `TrackerVoice::backward` direction flag, a ceil-based
`samples_to_loop_start()` mirroring the existing `samples_to_loop_end()`,
and `wrap_ping_pong()` (signed 64-bit, resolved only at an actual boundary
crossing). `player.h`'s `tracker_trigger_note()` gets a new
`Effect::SAMPLE_OFFSET` branch: memory (`sample_offset_memory`) is only
written on a row that both carries `9xx` and actually triggers a note, and
an offset at or past the target sample's length suppresses the whole
trigger — both verified against `openmpt123`, not assumed from FT2's own
documentation, which turned out to already be the harness's working
convention (same kind of FT2-vs-`openmpt123` gap as #20's key-off finding).

**Ping-pong implementation: direction flag with a mirrored read**, not
host-side loop unrolling. Decided from two constraints already on record
rather than a fresh measurement of the losing option: #16 measured ~20
points of spare Core 1 headroom at 32 voices, while the module's *other*
hard limit — 350-400 KB of SRAM for sample data — has no such slack;
unrolling every ping-pong loop region at conversion time spends from the
constrained resource to save from the one with headroom, backwards from
where the trade should go. `TrackerVoice::backward` costs one `bool` per
voice (32 bytes total) and one per-*batch* branch, not a per-sample one.

- **Guard-sample correctness at every loop type turned out to need no code
  change.** The existing guard byte (`blob_writer.py`'s `_guard_byte()`:
  loop-start value for any looped sample, last value for one-shot) is only
  ever read at `s[idx+1]` when `idx+1 == num_samples` (the loop reaches the
  sample's physical end) — true regardless of loop *type*, since
  ping-pong's reflection happens in position space (`wrap_ping_pong()`),
  not by reading the buffer differently near an edge. Verified by tracing
  through, not just assumed; see `mixer.h`'s `TrackerSample` comment.
- **The direction-flag reflection is not the textbook `2*boundary - pos`.**
  A zero-overshoot landing exactly on `loop_end_pos` (the increment divides
  the loop length exactly from a whole-sample start — rare, but the mixer
  must not hang on rare input) reflects to itself under the textbook
  formula, since `loop_end_pos` is an exclusive bound no read may land on.
  `wrap_ping_pong()` reflects around `loop_end_pos - 1` / `loop_start_pos +
  1` instead, trading a 2-part-in-16384-of-a-sample inaudible bias on every
  bounce for guaranteed termination. Caught by
  `tools/host_render/render_tracker_mixer.cpp`'s
  `test_pingpong_exact_boundary()` — the harness hung indefinitely before
  this fix, which is why that test exists as a permanent regression check.
- **The final, boundary-crossing step of a backward run is recomputed with
  signed 64-bit arithmetic from the batch's entry position**, not trusted
  from the `uint32_t` the per-sample loop just advanced. A ping-pong
  reflection routinely needs to represent a position before the loop start
  (or, symmetrically, past the loop end by more than one boundary width for
  a loop shorter than one increment), which a Q18.14 `uint32_t` position
  cannot hold — unlike a plain forward loop's overshoot, which is always
  unsigned-safe because addition never wraps low. Resolved once per
  boundary crossing, not a per-sample cost either way.
- **`9xx` past the sample's end suppresses the entire trigger**, not just
  the offset (clamped to 0, or to the end) — matching `openmpt123`'s
  documented FT2-compatible behaviour ("notes with offset commands beyond
  the sample length are never triggered"). The instrument column still
  latches (matching every other "nothing to play" branch in
  `tracker_trigger_note()`); nothing else about the channel's state
  changes.
- **Verified against `openmpt123`**: two new fixtures in
  `tools/xm2t00t/xm_synth.py` (`ping_pong_basic`, a tight ping-pong loop
  held long enough to bounce many times within one row; `sample_offset_basic`,
  a `9xx` trigger deep into a long two-toned sample plus a memory-reuse
  row plus a past-the-end suppressed trigger on a second, short
  instrument) both diff clean. `render_xm_device.cpp` adds a C++ unit
  test for `9xx`'s exact mechanics (start_pos scaling, the next-to-a-note
  memory rule, the suppression bounds check) that the audio diff can't
  cheaply pin down.

**Re-measured on real `breadboard_rp2350` hardware, profiling pin (2026-08-07)**,
matching #16's own methodology — one voice on a deliberately tight loop
plus a chorus of the rest, idle/8/16/24/32 voices — except the tight voice
is now ping-pong instead of forward: **0.7% idle, 8.19% (8v), 15.6% (16v),
23.0% (24v), 30.4% (32v)**. Indistinguishable from #16's forward-loop
baseline (8.20%/15.3%/22.8%/30.1%) to within measurement noise — confirms
the analytical prediction above: ping-pong's per-sample cost is identical
to a plain forward loop's (same interpolate/scale/accumulate body, `pos -=
inc` instead of `pos += inc`), and the direction-flag choice over
host-side unrolling cost nothing measurable. Measured via
`tools/xm2t00t/xm_synth.py`'s `voice_count_profile()` — a real song (not a
rebuilt synthetic rig; the #16-era phase-cycling code in `audio_engine.cpp`
no longer exists post-#18) temporarily swapped in for
`tracker_song_blob.h`, see that function's own docstring.

**Found along the way, not a #21 regression:** the test song's first draft
used key-off to silence all channels between laps, and the duty cycle never
dropped after the first lap even though the channels visibly went quiet —
key-off does not deactivate a `TrackerVoice` in `mixer.h`, only its target
volume; the mixer keeps fully interpolating and accumulating every
ever-triggered channel at zero output forever unless it's a one-shot that
plays through to its natural end. Pre-existing, unrelated to ping-pong/9xx,
and out of scope for #21 — left as an open, unscoped issue (see
`module_tracker.md`'s Future/TODO).

## Tracker Engine — Remaining `Exy` Sub-Commands (#22)

Done and closed (split from the original "FT2 quirk tail" issue on
2026-08-08: the bounded/mechanical sub-commands below landed and closed
here; the four named quirks and open-ended corpus-chasing were split out to
#25). `E1x`/`E2x` (fine porta up/down) and `EAx`/`EBx` (fine volume slide
up/down) apply once, at tick 0 only, with their own memory slots separate
from the continuous `1xx`/`2xx`/`Axy` commands. `E9x`/`Rxy` (retrigger)
landed as the *general* Rxy form — full volume-change table, not just
E9x's plain fixed-interval case — since both effect-column letters decode
to the same `Effect::RETRIG_NOTE` enum value and E9x's decode already
reaches that shared code with the volume-change nibble zeroed (`effects.py`
strips it), so handling Rxy properly was no extra work. `ECx`/`EDx`/`EEx`
(note cut, note delay, pattern delay) are invisible to Core 1 and only
change what Core 0 reads or how long it holds a row/tick, same category as
`Bxx`/`Dxx`'s existing row-level pending-effect handling: `EDx` defers a
row's entire tick-0 processing to the tick within the row its param names;
`EEx` holds the current row for `param` additional full-speed passes, with
the held repeats' own tick 0s falling through to normal *continuation*
handling (not a re-trigger) via one adjusted `row_boundary` computation
(`tick_in_row == 0 && !pattern_delay_holding`) that every other
per-channel dispatch already keyed off.

- **`E3x` (glissando control) was tried and reverted.** The mechanism (a
  persistent per-channel flag) was trivial, but snapping tone portamento's
  audible pitch to the nearest semitone each tick is not spec-clear enough
  to be bounded: two reasonable implementations (snapping a local copy for
  output only, vs. snapping the persisting glide state itself) both
  diverged from `openmpt123` within 1-2 ticks of the glide starting.
  Genuinely diff-driven quirk work, not the mechanical case the rest of
  this step turned out to be — moved to #25's deferred list.
- **Verified against `openmpt123`**: one new fixture per command
  (`fine_slides_basic`, `retrig_basic`, `note_cut_basic`,
  `note_delay_basic`, `pattern_delay_basic`) in `xm_synth.py`, asserted in
  `diff_xm.py` alongside every earlier fixture — all pass. Exact
  tick-scheduling behaviour the audio diff can't cheaply pin down (the
  precise delay/cut/retrigger tick, the exact row-hold length) gets a
  targeted C++ unit test in `render_xm_device.cpp` per command — all pass.

## Tracker Engine — FT2 Quirk Tail Deferred (#25)

Split out of #22 on 2026-08-08 so #22 could close on its own done scope.
Deferred, not scheduled: `E60` pattern loop (including nested/interacting
cases), `E3x` glissando control, envelope handling on note-off, portamento
with a changed instrument, arpeggio wraparound at high speeds, and the
open-ended corpus-driven work beyond those (run the regression set, find
the first divergence, fix it, add the module to the set, repeat until a
stated corpus of ≥10 real modules passes). No natural completion point, so
picking this back up is a deliberate decision, not automatic follow-on from
anything else. Blocks the eventual dynamic-sample-loading work (#23), which
needs settled player semantics first.

## Tracker Engine — Display (#24)

`src/engines/tracker/display.cpp`: order/pattern/row position, per-channel
activity, and song title/tracker name/channel count. A full 240×284 16bpp
framebuffer would be 133 KB — a quarter of SRAM, competing directly with
sample data — so this uses tile rendering into `gfx.cpp`'s existing shared
scratch buffer instead, redrawn on change at up to 20 Hz, not a persistent
framebuffer.

## Tracker Engine — Dynamic Sample Loading (Phase 2, planned, not built)

Design only — not implemented. Recorded here so the reasoning survives
until #23 (blocked on #25 settling player semantics first) picks it up.

Bandwidth check first: QSPI quad at ~75 MHz ≈ 35 MB/s, so a 64 KB sample
loads in ~2 ms by DMA while a row at 125 BPM / speed 6 is 120 ms. This
ratio collapses the design — one or two rows of notice is enough, not
long-horizon streaming.

Because the song is deterministic, the plan is to compute the schedule on
the host, not have Core 0 speed-play at runtime. Same answer, and offline
it unlocks:

- **Belady's MIN eviction** — with the whole future known, evict the
  sample whose next use is furthest away. Provably optimal; no online
  heuristic can match it.
- **Static placement** — variable-size samples in a fixed pool is a
  fragmentation problem at runtime, but an interval-packing problem
  offline. Greedy first-fit over liveness intervals. No runtime allocator,
  no compaction.
- **Build-time verification** — the converter computes peak working set
  and rejects modules it cannot schedule, with a reason.

The MCU would just execute a load script. Correctness requirements
identified: **liveness, not triggers** (a sample is evictable only when no
channel is still reading it — looped sustains with no note-off and long
envelope releases hold samples past their last trigger, the easiest place
to introduce rare, unreproducible clicks); **load before evict** (peak
residency is working set plus the largest in-flight load); **seeking**
(precompute a required-resident-set manifest per order position; jump →
load set → resume; a full 400 KB reload is ~12 ms).

What this would not fix: peak simultaneous working set. A module with
700 KB of instruments all live in one pattern is unplayable regardless.
The scheme converts a hard limit into a soft one. Favourable case: tracker
modules have poor count-weighted locality (the kick is everywhere) but
decent size-weighted locality — big samples tend to be the localised ones.

**Hardware escape hatch, considered but not adopted:** RP2350 QMI CS1
supports PSRAM; boards like the Pimoroni Pico Plus 2 wire up 8 MB. Would
make single-module capacity a non-issue and load faster, but does not
remove the SRAM working-set architecture (same random-access latency
problem), and is a board change from bare `breadboard_rp2350` — a fork, not
a drop-in. If flash capacity turns out to limit how many modules ship in
one firmware, ADPCM in flash with decode-on-load would give ~4× without
touching the runtime path.

## Tracker Engine — Host-Side SRAM Size Reduction (planned, not built)

Design only — not implemented (verified: no dedup/decimation/truncation
code exists in `tools/xm2t00t/` beyond the 8-bit conversion already
shipped). In order of effort:

1. **8-bit conversion** — done. 2× immediately, and period-correct for the
   material.
2. **Trim past `loop_end`** — not done. Unreachable; XM samples are
   frequently padded there.
3. **Truncate to actual reach** — not done. The idea is a simulator that
   knows the furthest position any one-shot playback ever reaches. Mind
   `9xx` sample-offset commands (#21 landed `9xx` before this optimisation
   exists): a naive "furthest position a plain trigger reaches" simulator
   would truncate a sample shorter than some `9xx` offset the song actually
   uses still legitimately reaches (`tracker_trigger_note()` sets
   `start_pos` directly from the offset — mid-sample, past wherever an
   un-offset trigger's own playback would have gotten to by the time it's
   cut). When this is built, the reach calculation must include every
   `9xx` param actually used against each sample, not just note-to-note
   pitch/duration.
4. **Deduplicate** — not done. Modules built from sample packs often carry
   byte-identical instruments.
5. **Per-sample decimation from known increments** — not done. The
   simulator would know the exact set of increments the song uses for each
   sample; if a sample is only ever played at increment ≥ 1.0 (pitched up,
   common for high keyboard mappings), content above the resulting Nyquist
   is never audible and the sample could be resampled offline to the
   minimum rate the song actually needs — perceptually lossless, and only
   possible because of the dry run.

Estimate: these five together were expected to cut typical modules by
50–70%, which may put most of the corpus under the SRAM ceiling and reduce
dynamic loading to an outlier feature.

**The deterministic simulator** these items depend on does not exist yet
either. The plan is a full XM player inside the converter that renders no
audio but tracks state — the intended source of truth for items 3 and 5
above, for the load schedule, and for the seek manifests. The plan calls
for building it before dynamic loading (phase 2) is otherwise needed, on
the reasoning that it's the same code either way, and having it from day
one would make the phase-2 feature mostly data-format work rather than a
new subsystem.

## Tracker Engine — Feedback to the Subtractive Engine (proposal, not yet ported)

From `history_subtractive.md`: 2–3% per voice without LFO, 5–6% with. The
LFO roughly doubles voice cost — a 0.1–20 Hz signal evaluated 44,100 times
a second with a float phase accumulator and a sine-table lookup, three
orders of magnitude of oversampling.

Proposal: retrofit the tracker's sub-block skeleton to the subtractive
engine — LFO value computed once per sub-block (~690 Hz, still 34×
oversampled for a 20 Hz LFO) and linearly ramped; ADSR level likewise; SVF
`F_half` coefficient recomputed per sub-block and ramped, rather than per
sample. Must stay per-sample regardless: the SVF two-pass state update and
PolyBLEP correction, both per-sample by nature.

Plausibly 30–40% off the subtractive voice cost, i.e. the difference
between 16 voices at 75% and 16 voices with headroom. The reasoning for
proving the pattern in the tracker engine first — where the voice loop is
simple enough to get right quickly — rather than refactoring the
subtractive engine directly, is lower risk.
