# T00T — Subtractive Engine: Development History

Dated performance measurements for the subtractive synthesis engine — the
original engine, predating the module system. See `module_subtractive.md` for
the current spec/design; `engine.md` for the shared dual-core architecture
this engine (and every other module) is built on.

---

## Performance

Measured duty cycles on the profiling pin (`PROFILE_PIN`, now GPIO 22):

- Idle: 0.85%
- Single voice, no LFO: 2-3%
- Single voice w. LFO: 5-6%
- 16 voice max usage (unreliable measurement): ~75%
- <16 voice normal usage (unreliable measurement): 50%

### Baseline before RP2350

This is the baseline measurements of the state before switching to RP2350 and upgrading the core:

- Idle: 0.81%
- Voice A: "Fairlight" sample (8-bit converted on the fly to Q15): 10.3%
- Voice B: Square wave (BLEP) with 3Hz LFO for duty cycle + Filter with Q envelope: 12.3%
- Voice C: Triangle with LFO controlling amplitude + Filter Q: 10.5%
- All 3 voices sustaining: 31.5%
- Intense work will overload the buffer. Moderate use will get it close to 100%

### Performance gain table

| Phase       | Idle  | Voc A | Voc B | Voc C | ABC   | Max   | Comment |
| - | - | - | - | - | - | - | - |
| RP2040      | 0.81% | 10.3% | 12.3% | 10.5% | 31.5% | >100% | |
| RP2350 port | 0.56% |  6.3% |  6.7% |  6.1% | 18.0% | ~80%  | No code changes, just retarget |
| float env.  | 0.52% |  6.5% |  6.7% |  6.2% | 18.3% | ~85%  | Calculate envelope using floats |
| "float" lfo | 0.52% |  6.3% |  7.0% |  6.4% | 18.7% | ~95%  | Float interface for LFO, but Q15 impl + sine lookup |
| SMULL filt. | 0.50% |  5.9% |  6.6% |  6.0% | 17.5% | ~90%  | |
| SSAT env.   | 0.44% |  5.9% |  6.5% |  5.9% | 17.4% | ~90%  | |

The following measurements were measured after a couple of additions: envelopes, effects, modularization (`subtractive` and `groovebox`).
The baseline reflects the state on 2026-08-06 prior to implementing `tracker`, `speech` and `fm` modules and the subblock optimizations.
Max is measured when using all 16 voice channels.

| Phase       | Idle  | Voc A | Voc B | Voc C | ABC   | Max   | Comment |
| - | - | - | - | - | - | - | - |
| no FX       | 0.48% |  6.4% |  6.9% |  6.3% |   -   | ~90%  | |
| Delay FX    | 1.66% |  7.5% |  8.1% |  7.4% |   -   | ~90%  | |
| Reverb FX   |  8.2% | 14.1% | 14.6% | 14.0% |   -   | ~90%  | |
| LFO(vibrato)| 0.48% |  7.2% |  7.7% |  7.1% |   -   | ~90%  | Pitch LFO through modwheel. No FX |
| | | | | | | | |
| no FX       | 0.53% |  6.9% |  7.4% |  6.8% |   -   | ~90%  | After pan fix (issue #11). Slight (~0.5% for active voice, 0.05% idle) raise in CPU |
| Delay FX    |  2.1% |  8.4% |  8.9% |  8.3% |   -   | ~90%  | After pan fix (issue #11). As above |
| Reverb FX   |  8.4% | 14.8% | 15.3% | 14.7% |   -   | ~90%  | After pan fix (issue #11). As above |
| | | | | | | | |
| no FX       |  0.6% |  5.9% |  5.7% |  5.1% |   -   | ~86/80/70%  | After subchunk fix (issue #12). Max is depending on voice used (A/B/C). Major improvement for modulator-heavy voices (Voice C)! |
| Delay FX    |  2.1% |  7.3% |  7.2% |  6.6% |   -   | ~86/80/70%  | After subchunk fix (issue #12) |
| Reverb FX   |  8.5% | 13.7% | 13.6% | 13.0% |   -   | ~94/90/81%  | After subchunk fix (issue #12) |
| LFO(vibrato)|  0.6% |  5.9% |  5.7% |  5.1% |   -   | ~86/80/70%  | After subchunk fix (issue #12). Pitch LFO through modwheel. No FX. No measurable overhead for vibrato! |

## Performance + DIAG Pages (Widget/Header/Page library)

Fourth module (after OPL, FM, chip) to apply the shared Widget/Header/Page
library, explicitly matched to FM's own Performance/DIAG content exactly
(`history_fm.md`'s "Performance + DIAG Pages" entry): Header's Resource
bar, preset (number + name, from the existing local `PRESET_NAMES[]` table
-- `presets.h`'s `VoicePreset` has no name field of its own, same
decoupled-name convention the old hand-rolled display.cpp already used),
FX type (Label) + FXMIX (Value bar) paired on one row, FX P1 + FX P2 paired
on the next, MOD alone at half width below -- all at scale 1, PERF/DIAG
page names capped to 8 characters and drawn in the header's own accent
color (blue) rather than plain black, both pulled clear of the panel's
rounded corners via `gfx_corner_safe_margin()`.

DIAG: CPU% (PercentageBar), per-voice activity (ActivityGrid, one row --
MAX_VOICES=16 matches FM's own), last note. No multitimbral grid or
algorithm indicator -- subtractive has neither concept (one global preset
across all channels, no per-operator routing) -- so DIAG is just CPU/
voices/note, same as FM's own once its multitimbral-grid and algorithm
content are set aside. Pitch bend, previously its own row, and the old
per-voice dot bar's separate "pressed" (bordered) vs. "sounding" (filled)
distinction are both dropped: ActivityGrid only has one on/off state, and
FM's own DIAG never showed bend either, so matching FM exactly means
neither survives.

Verified via `tools/host_render/preview_subtractive_display.cpp` (new,
mirrors `preview_opl_display.cpp`/`preview_fm_display.cpp`/
`preview_chip_display.cpp`) against synthetic telemetry, and `make
ENGINE=subtractive` builds clean on the actual ARM cross-toolchain. Real
hardware flashing/encoder feel still untested. `display_bringup_test()`
(colour-bars/banner driver diagnostic, unrelated to the Performance/DIAG
Pages) is untouched.

## Patch-platform baseline (#146, 2026-11-17)

Pre-platform reference for the patch platform's "no regression" gate
([platform.md](../platform.md), #145). Later gates (#152) repeat this exact
method on the cooked path and compare.

**Method.** Breadboard rig, firmware at `ccd4ff2` (pre-platform; const
`presets[]` tables). GPIO 22 profiling pin, high while Core 1 renders a
buffer; read as the scope-averaged duty % of the 256-sample buffer period (5.805 ms at
44.1 kHz). Patch: Fairlight (preset 0, `PRESET_FAIRLIGHT`). FX off. N notes
held and sustained at full velocity, N = 0, 1, 2, 4, 8, 16 (`MAX_VOICES` =
16); only the count of simultaneous voices matters, not which notes.

| Voices | Duty | Core 1 time / buffer |
|---:|---:|---:|
| 0 (idle) | 0.57% | 33 µs |
| 1 | 6.0% | 348 µs |
| 2 | 11.3% | 656 µs |
| 4 | 22.1% | 1283 µs |
| 8 | 43.7% | 2537 µs |
| 16 (full) | 81.4% | 4725 µs |

Linear in voice count: ≈5.05% (≈293 µs) per voice. All 16 voices fit, with
18.6% of the buffer period to spare.

FX-on baseline not measured; FX off is the reference for now.

## Patch-platform tracer (#148, 2026-11-17)

Subtractive becomes the patch platform's tracer bullet (spec #145,
[platform.md](../platform.md)): its sounds are now data.

- **Render kernel extracted first.** The per-buffer render moved out of
  `audio_engine.cpp` into host-buildable `render.h` with no behaviour
  change (commit `9db7327`), and `render_subtractive` captured golden
  hashes of every const-table preset through it (C4 vel 100, G4 vel 127
  from buffer 40, release at buffer 120, 300 buffers, default FX). That
  golden is the "sound unchanged" reference for the migration.
- **Descriptors and stored format.** 12 parameters (`patch.h`):
  waveform, sample, duty, LFO rate (centi-Hz), LFO → amp/pitch/PWM
  (per mille), LFO → cutoff (Hz), filter mode, cutoff, resonance, env →
  cutoff. Waveform, sample and filter mode are note-on class; the rest
  live (live editing itself comes with #149). No default CCs yet — CC74,
  the usual cutoff CC, is FX type here. The preset `amplitude` field was
  dropped: velocity always overwrote it.
- **Factory bank.** `gen_subtractive_bank` converts `presets.h` into
  `factory_bank.h` (11 Patches, 45 bytes each). It cross-checks that
  cooking the converted values reproduces exactly the constants the old
  render derived from each preset per buffer, so the bank can't be lossy.
- **Cook and Part slots.** 16 Parts, one per MIDI channel. Program
  Change decodes a factory Patch into the Part's Edit buffer, cooks it
  (sample pointer, LFO phase increment, Q15 depths, SVF Q and seed
  F_half) and publishes it through a `PartSlotT` handshake: Core 0 only
  writes the spare buffer once Core 1 has acknowledged the last publish,
  otherwise it keeps the patch pending and `patch_platform_task()` retries
  each main-loop pass. Core 0's note-on maths uses the last *published*
  patch, so it agrees with what Core 1 plays. Core 1 latches the voice
  block, takes new cooked patches, then renders; a voice copies its Part's
  cooked patch at note-on. Program Change still affects only later notes.
- **Note ↔ patch generation tag** (found in review): a note-on committed
  while patch A was current, followed within the same buffer by a Program
  Change publishing B, made the note start on B with a pitch computed for A
  (wrong pitch whenever the sample changes). Each note now carries the low
  byte of the slot generation Core 0 used, and Core 1 keeps each Part's
  current and previous cooked patch and starts the note on the matching
  one. One previous patch suffices: the slot can't publish again until
  Core 1 has taken the newest. `render_subtractive` checks this case.
- **All parameters fixed per note for now.** The whole cooked patch is
  copied per voice, so live-class parameters don't yet reach sounding
  notes; nothing can change one mid-note until live editing (#149), which
  will have voices read live fields from the Part.
- **Moved off Core 1:** per-voice-per-buffer `svf_compute_q()`, the LFO
  phase-increment divide and four float → Q15 conversions now happen once
  per cook; added: 16 slot checks per buffer and one 40-byte copy per note
  start. Hardware cost is measured by #152.
- **Memory:** Core 0 platform state 2.6 KB, 16 slots 1.4 KB, Core 1
  per-Part (current + previous) and per-voice cooked copies 1.9 KB (SRAM); factory bank ~0.5 KB
  (flash).
- **Tests.** `test_patch_platform` (stub module, MIDI bytes in → cooked
  commits out: Part routing, older-schema defaults, module-ID rejection,
  back-to-back publishes, Core 0 view, encoder bytes) and
  `render_subtractive`: all 11 factory Patches through stored format →
  cook → slot → render match the pre-platform golden bit-exactly.
