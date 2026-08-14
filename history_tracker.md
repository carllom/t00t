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
