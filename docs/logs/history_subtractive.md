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

## Live editing over NRPN (#149, 2026-11-17)

- **NRPN and default CCs.** A catch-all mapping entry after the explicit
  ones hands every other CC to `PatchPlatformT::midi_cc()`: NRPN
  (CC99/98 select = parameter ID, CC6/38 data) or a descriptor default CC
  sets one value in the channel's Part Edit buffer, then Core 0 re-cooks
  and publishes. Data is scaled onto the parameter's range; a Data Entry
  MSB alone is bit-replicated so 7-bit controllers reach full scale.
  Default CCs, microKORG-numbered where possible: CUTOFF CC74, RESO CC71,
  ENV>CUT CC79, LFO RATE CC12. FX type moved from CC74 to CC76 on
  subtractive only (the other modules keep CC74 until they migrate).
  `InputValue` gained `id`, the matched source id, so a range entry knows
  which CC it got.
- **Live vs note-on.** `SubCookedPatch` splits into `note_on` (waveform,
  sample, filter mode) and `live`. Voices copy `note_on` at note start and
  read `live` from their Part (`PartReceiverT`, now generic in
  `src/patch/receiver.h`) every buffer. Live fields no longer depend on
  note-on ones: SVF Q and seed F_half are cooked unconditionally.
- **Program Change on held notes** (decided with the author): held notes
  keep their old Patch. Slot publishes carry a load ID that changes on a
  Patch load but not on an edit; when Core 1 takes a new load it copies the
  old live fields into that Part's sounding voices, which then ignore the
  Part. A note tagged with a previous patch of a different load starts
  detached too.
- **Coalescing.** A burst of edits within one buffer: the slot publishes
  the first, Core 0's main-loop task publishes the latest on its next
  pass, so change-to-audible latency is one to two buffers.
- **Tests.** `test_patch_platform` adds NRPN selection/scaling/LSB
  refinement, default CCs, module Handler precedence, deselection and
  unknown IDs, coalescing, live vs note-on through stub voices, load vs
  edit, and compile-time default-CC rules. `render_subtractive` adds:
  every parameter cooks into its class's group; a live CUTOFF edit changes
  the sounding note from the next buffer; a note-on WAVE edit doesn't; a
  Program Change mid-note leaves it alone, as does a later edit of the new
  Patch. The factory goldens still match.
- **Review fix: note-on fields move into `VoiceParams`.** The #148
  generation tag broke down once edits stream: with the slot busy, a
  note-on edit waits as pending, and a note started right after it was
  tagged with — and so played — the older published patch for its whole
  length (every 14-bit NRPN hit this: MSB published, LSB pending). A narrow
  race could also leave a tag matching neither patch Core 1 keeps. Now
  Core 0 copies the note-on fields (8 bytes) and the Patch load ID from the
  Edit buffer's latest cook into the note's `VoiceParams`, and
  `PatchPlatformT::cooked()` is that latest cook rather than the last
  published one. Tags and slot generations are gone; Core 1 uses the load
  ID only to decide which sounding notes follow the Part's live fields.
  `NrpnDecoder` also resets the Data Entry MSB when a parameter is
  selected. `render_subtractive` adds "a note right after a queued note-on
  edit plays the edited Patch".
- **Memory**, measured with the Cortex-M33 toolchain (the #148 figures
  above came from the 64-bit host and overstate pointer-holding structs):
  cooked patch 36 B, slot 84 B (× 16 = 1.3 KB), Core 0 platform state
  1.8 KB, Core 1 Part receivers and per-voice state 1.7 KB, `VoiceParams`
  24 B (+8 B for the note-on fields and load ID).

## SysEx core protocol (#150, 2026-11-17)

- **Parser.** `MidiParser` assembles SysEx (F0…F7) into a 256-byte buffer
  and emits `MIDI_SYSEX`; over-length messages are dropped whole, a status
  byte inside one abandons it and is then parsed normally, and real-time
  bytes pass through. Each transport now has its own parser
  (`MidiParsers`, behind the old `midi_parser` name so no engine changed):
  USB and DIN used to share one, so a message split across reads could be
  corrupted by the other transport's bytes.
- **Protocol** (`src/patch/sysex.h`): `F0 7D 54 <cmd> … F7` with identify
  (01/02), parameter change (10, exact int21 value, not scaled like NRPN),
  edit-buffer request (20) and dump (21, packed stored Patch, both
  directions — a received dump is a load, so held notes stay on their old
  Patch). Ranges 30–3F, 40–4F and 60–7F are reserved for store/banks,
  descriptor dump and Data Exchange.
- **MIDI out.** `midi_out_send()` queues replies (512-byte ring) and
  `usb_midi_task()` drains them through `tud_midi_stream_write()`. DIN has
  no TX, so requests over DIN are answered over USB.
- **Tests.** `test_sysex` drives the generic MIDI loop with the real
  subtractive module against `golden/sysex_subtractive.txt`, which
  `tools/sysex_vectors.py` writes with its own framing/packing code:
  assembly with real-time bytes inside, over-length/interrupted/foreign
  SysEx, USB and DIN interleaved, identify, parameter change (positive and
  negative), request/dump round trip, foreign-module and malformed dumps.
  The FM build was checked to compile against the parser changes.

## Descriptor dump over SysEx (#151, 2026-11-17)

- **Dump.** `40` requests it; the device answers table begin (`41`), one
  descriptor message (`42`) per parameter, a labels message (`43`) after
  each labelled ENUM parameter, for the module table then the common
  table, then dump end (`44`). Subtractive's whole dump is 775 bytes.
- **Common table** (`src/patch/common.h`, decided with the author): FX
  type, mix, P1, P2 — described, but still set through each module's FX
  CCs until the Multi (#155) holds them.
- **Value labels** (decided with the author): `ParamDescriptor` gained an
  optional label list, checked against the range by
  `descriptor_table_valid()`; WAVE, SAMPLE, FILTER and FX TYPE carry them.
- **Chunked and paced.** One message per descriptor, each checked at
  compile time to fit `MIDI_SYSEX_MAX`. `PatchSysExT` (the SysEx handler,
  now an object holding the dump's progress) sends the next message only
  when `midi_out_send()` — now returning false when the USB TX queue is
  full — accepts it, and `patch_platform_task()` resumes it each pass.
  DIN has no TX; the same backpressure would pace a DIN out.
- **Tests.** `test_sysex`: the dump matches the golden vector (from
  `tools/sysex_vectors.py`, which states the expected tables literally);
  through a 100-byte-per-pass queue it arrives whole and in order over
  several passes and then stops; identify is answered while a dump waits.
- **Review fixes.** A reply (identify, edit-buffer dump) that found the
  queue full mid-dump was silently dropped; `PatchSysExT` now holds it and
  sends it ahead of the dump's next message. `sysex_descriptor_dump_fits()`
  also rejects at compile time what the encoding can't carry: non-ASCII or
  over-127-character strings, more than 127 labels, values outside int21.

## Patch-platform gate: cooked path (#152, 2026-11-17)

Same method as the #146 baseline (breadboard, GPIO 22 scope-averaged duty,
Fairlite, FX off — set over USB with CC76/CC73 — N sustained full-velocity
notes). Firmware: the patch-platform build (it answered the SysEx identify
request, so #150 or later).

| Voices | Baseline (#146) | Cooked path | Change |
|---:|---:|---:|---:|
| 0 (idle) | 0.57% | 0.58% | +0.01 pp |
| 1 | 6.0% | 6.05% | +0.05 pp |
| 2 | 11.3% | 11.4% | +0.1 pp |
| 4 | 22.1% | 22.3% | +0.2 pp |
| 8 | 43.7% | 44.1% | +0.4 pp |
| 16 (full) | 81.4% | 82.5% | +1.1 pp |

- **Gate: not met, by a small margin.** The cost grows with voice count —
  about 5.10% per voice against 5.05% (≈ +3 µs per voice per buffer, +1.4%
  at 16 voices) — so it's per-voice render work, not the fixed per-buffer
  slot checks (idle moved by only 0.01 pp). All 16 voices still fit, with
  17.5% of the buffer spare against 18.6%.
- **Cause: not yet established.** The cooked path removed per-voice work
  (SVF Q, LFO increment, float → Q15 conversions), so the extra cost is in
  how the kernel reaches its data: live fields are read through a per-voice
  reference that picks the Part's patch or the voice's frozen copy, and
  note-on fields come from the voice's `VoiceParams`. Candidates: those
  loads being re-done inside the per-sample loop rather than hoisted, and
  code layout changes in flash (XIP cache). Next step: compare the
  generated inner loop of `sub_render_buffer()` before (9db7327) and now.
- **Latency, by ear:** no delay audible on parameter edits or Patch
  changes. Not timed; by design an edit lands one to two buffers later
  (≈5.8–11.6 ms, #149).
- **Not measured:** Core 0 cook time per Patch.

### Regression found and fixed (2026-11-17)

- **Cause.** Comparing the compiled per-sample loop of `sub_render_buffer()`
  at 9db7327 (pre-platform) with the patch-platform build: the old loop
  kept the voice's `VoiceParams` pointer in a register and loaded fields
  straight off it. The new loop reads from two structs per voice (the
  note's `VoiceParams`/`note_on` and the Part's live fields), the extra
  pointer pushed both onto the stack, and every sample reloaded a pointer
  and then the field: sample pointer, amplitude, duty cycle and the
  sample-end check, about four extra loads per sample per voice.
- **Fix.** The per-voice constants the per-sample loop reads (waveform,
  sample pointer, filter mode, amplitude, duty) are copied to locals before
  it. Output is bit-identical (render goldens unchanged).
- **Re-measured**, same method:

| Voices | Baseline (#146) | Before fix | After fix |
|---:|---:|---:|---:|
| 0 (idle) | 0.57% | 0.58% | 0.57% |
| 1 | 6.0% | 6.05% | 5.4% |
| 2 | 11.3% | 11.4% | 10.2% |
| 4 | 22.1% | 22.3% | 19.8% |
| 8 | 43.7% | 44.1% | 39.1% |
| 16 (full) | 81.4% | 82.5% | 72.7% |

- **Gate: passes.** About 4.5% per voice against 5.05%: 10.7% less Core 1
  time than the pre-platform baseline at 16 voices, 27.3% of the buffer
  spare. The patch-derived work moved to Core 0 (SVF Q, LFO increment,
  float → Q15 conversions) now shows, no longer masked by the reloads.

### Core 0 cook time (2026-11-17)

Measured with `make PATCH_PROFILE=1` (`src/patch/profile.h`): the M33 DWT
cycle counter around `Module::cook()` (cook), `PatchPlatformT::load()`
(decode + cook + publish) and `set_param()` (cook + publish), last and
largest count kept in a global read through the debug probe. Effects off
(CC76/CC73), no notes held, 150 MHz. Program Changes and CC/NRPN edits sent
over USB with `amidi`; the normal build was flashed back afterwards.

| | Warm (back-to-back) | Isolated (last of spaced-out events) | Largest seen |
|---|---:|---:|---:|
| Cook | 151–255 cycles (1.0–1.7 µs) | 1,289 cycles (8.6 µs) | 3,082 cycles (20.5 µs) |
| Program Change: decode + cook + publish | 486 cycles (3.2 µs) | 4,904 cycles (32.7 µs) | 8,890 cycles (59 µs) |
| Parameter edit: cook + publish | 316 cycles (2.1 µs) | — | 4,976 cycles (33 µs) |

- The spread is the XIP flash cache: the cook path runs from flash, and
  between spaced-out events the display and the rest of the main loop evict
  it; "largest seen" includes the 16 loads at boot.
- Even cold, a Patch load costs about 1% of a 5.8 ms buffer period on
  Core 0, so cooking needs no incremental scheme for subtractive.

## User banks in flash (#153, 2026-11-18)

- **Layout** as in `platform.md`'s User-bank layout. Subtractive's 45 B
  Patch fits a 256 B slot with room for schema growth.
- **Banks and Program Change** (decided with the author). Bank Select MSB
  0–63 = factory bank, 64–127 = user bank; Program Change n = slot n. The
  microKORG row/column numbering is dropped so all 128 user slots are
  reachable; an empty slot or missing bank is ignored rather than loading
  an init Patch. Factory programs 11–127 no longer wrap onto the 11 factory
  Patches.
- **Store.** SysEx `30` stores a Part's Edit buffer into a user slot and
  answers with `31` and a result code. The flash writer parks Core 1 in RAM
  at its next buffer with a cooperative flag rather than the SDK's
  multicore lockout, which would take over the inter-core FIFO the audio
  buffer protocol runs on. The audio DMA handler was made RAM-resident and
  register-level (it called the out-of-line, flash-resident
  `multicore_fifo_push_timeout_us()`), so it stays enabled during the
  erase and I2S keeps clocking out the zeroed buffers.
- **Host tests.** `test_user_bank` (layout, partitioning, sector
  read-modify-write, refusals, survival across a "reflash"),
  `test_patch_platform` (Bank Select + Program Change into factory and user
  banks, Store) and `test_sysex` (store request/reply golden vectors), all
  against a RAM-backed NOR flash stub (`tools/host_render/ram_flash.h`).
- **On hardware (2026-11-18):** Part 0's Fairlite with CUTOFF set to 1234 Hz
  over SysEx, stored to user slot 5 (reply OK), loaded into Part 1 with
  Bank Select 64 + Program Change 5. Then the FM build was flashed, then
  subtractive again; after the reboot (Part 1 back on Sq-PWM) slot 5 still
  loaded as Fairlite at 1234 Hz. Store mute length is still to be measured.

## Quick-edit Page with Compare and Store/rename (#154, 2026-11-18)

- **Interaction** (decided with the author): the encoder's push button is
  a short press = ENTER, a 0.5 s hold = EXIT, for every module with an
  encoder (a hold still jumps to Performance outside an edit). The Page
  claims UI commands before Page navigation while showing; rename uses a
  cursor mode and a character mode over 16 characters plus OK.
- **Part:** the last-played channel's Part, frozen once editing starts.
- **Compare:** publishes the stored version as an edit, not a load, so
  sounding notes switch with it; any edit (encoder, NRPN, SysEx) ends it.
- **Platform:** each Edit buffer keeps its stored version and origin;
  `edited()`, `set_compare()`, `rename()`, `revision()` (any change the
  display shows) and `user_slot_name()` were added; a Store makes the slot
  the stored version.
- **Rendering** is shared by the device and `preview_subtractive_display`,
  which now runs the real module through UI commands and writes the EDIT
  Page states to `[out_dir]` (its output path was a hard-coded scratch
  directory).
- **After review:** rows show what is heard (the stored values while
  comparing); a Store ends Compare; a long press cancels a Store from any
  of its steps; a load into the Part closes the Store dialog; the head row
  drops its `CMP` tag (the COMPARE button shows it) so a 16-character name
  fits. The edited marker compares against the copy taken at load or
  Store, so it doesn't notice the slot being overwritten from elsewhere.
- **Host tests:** `test_quick_edit` (rows, Part follow, value steps per
  curve, Compare, Store slot choice, rename, cancel, failed Store, value
  text), `test_patch_platform` (edited marker, Compare, rename, Store,
  load without prompt), `test_ui_nav_consumer` (short/long press, claim).
- **On hardware (2026-11-18):** the EDIT Page works end to end (row
  select, value edits, Compare, Store with rename, long press back). The
  breadboard encoder's push switch turned out to be dead (no SW–GND
  continuity when pressed); tested with a jumper from GP28 to GND instead.
