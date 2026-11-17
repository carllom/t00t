# T00T Patch Platform

Design doc for the module-agnostic layer that makes sounds data: Patches and Multis are
stored, edited and exchanged as values described by Descriptors, then cooked on Core 0 into
the engine-native constants each module's render loop already consumes. Spec: #145.
Vocabulary (Patch, Multi, Part, Bank, Edit buffer, Store, Cook, Descriptor, Parameter class,
Quick-edit Page) is defined in [CONTEXT.md](../CONTEXT.md#patch-platform).

Budgets and open questions are filled in by the roadmap tickets as they land. The
cross-module dual-core architecture it builds on is in [engine.md](engine.md).

## Goal and hard constraint

t00t becomes a hybrid instrument: Patch and Multi data, deep editing in a host editor with
live preview on the device, and select / quick-edit / Compare / Store on the device itself.

**Hard constraint:** parametrisation must not cost render time or voices. Core 1 cycles per
buffer must be equal or lower on the cooked path than today, with no loss of voices or
channels. This is proven by measurement (the P0 gates), not estimate.

## Architecture

### Core 0 cooks, Core 1 modulates only

```
 MIDI / SysEx / UI                Core 0                                   Core 1
 ─────────────────  ┌──────────────────────────────────────┐   ┌─────────────────────────────┐
 Program Change ──▶ │ Bank/slot ─▶ Edit buffer (per Part)  │   │ once per buffer:            │
 NRPN / CC      ──▶ │              │  (Descriptor terms)   │   │   check each Part's commit  │
 SysEx          ──▶ │              ▼                       │   │   flag, take newest slot    │
 Quick-edit Page──▶ │            cook ─▶ cooked slot [2] ──┼──▶│ per voice:                  │
                    │                    + commit flag     │   │   env/LFO × cooked base     │
                    │ notes ─▶ ParamExchange (per-note +   │──▶│ note-on: copy note-on-class │
                    │          Multi-level state only)     │   │   fields into voice state   │
                    └──────────────────────────────────────┘   └─────────────────────────────┘
```

- **The render loop never sees the parameter model.** Descriptors are read only by Core 0,
  the UI and the SysEx layer. All Patch-derived maths (routing tables, base phase increments,
  EG rates/levels, base filter coefficients) is cooked on Core 0 at control rate.
- **Core 1 keeps only per-voice modulation** on cooked constants, as it does today. For FM and
  OPL this moves routing resolution (today on Core 1 at note-on) to Core 0; the per-sample
  kernel reads the cooked patch instead of a pointer into a `const` table. For subtractive,
  base coefficients arrive cooked; per-sub-block env/LFO modulation of cutoff stays on Core 1.
- **Per-Part cooked slots live outside `ParamExchange`.** Each Part owns a small double
  buffer of cooked patches with a commit flag. `ParamExchange` keeps only per-note and
  Multi-level state, because the generic MIDI controller copies the whole active block into
  the shadow on every MIDI batch — patch-sized state there would cost kilobytes per batch.
- **Voices never see a torn patch.** A swap publishes a whole cooked patch; Core 1 checks the
  commit flags once at the start of each buffer. Live-class parameters reach sounding voices
  through the Part's active slot; note-on-class fields are copied per voice at note-on, so a
  later swap cannot change a sounding note's structure.
- **Latency trade:** an edit takes effect up to one block late (256 samples at 44.1 kHz,
  ≈5.8 ms), stepped unless the engine already ramps that value.
- **Data Exchange parameters** (samples, waveforms, ramps, LFO shapes) are exempt from double
  buffering and may tear or mute the engine during transfer. Only the class and a SysEx range
  are reserved; the transfer design is out of scope.

### Data model

- **Descriptor tables:** one `constexpr` table per module plus one shared table for the
  common components (FX, master, MIDI). Entry: parameter ID, name, group/Page, integer range,
  display curve/unit, Parameter class, quick-edit flag, optional default CC. Parameter IDs are
  append-only within a module's schema.
- **Stored format:** header (magic, module ID, schema version, name, payload length) plus one
  integer per parameter in its Descriptor domain (packed uint8/uint16), in parameter-ID order.
  Parameters missing from an older schema load as the Descriptor default; a mismatched module
  ID is rejected. Two stored object types: Patch and Multi.
- **Multi:** N Parts (fixed per module at build time) plus common FX/master settings. Each
  Part: MIDI channel, Patch reference, volume, pan, FX sends. FX belong only to the Multi.

### Editing and storage

- One volatile Edit buffer per Part, and one for the Multi. Loading a Patch replaces the Edit
  buffer without prompting; the UI marks it edited while it differs from its slot.
- Flash is written only on an explicit Store, under multicore lockout with audio muted for
  the erase/program time.
- Factory banks are compiled in and read-only, generated by host tools from today's const
  tables and external formats. User banks live in a reserved region at the end of flash,
  outside the UF2 image, partitioned by module ID, in fixed-size slots; a Store rewrites the
  4 KB sector containing the slot.

### MIDI control and SysEx

- Program Change on a Part's channel loads a Patch into that Part; Bank Select MSB picks the
  bank. Both are Configuration inputs in the Input pipeline. Multis are selected only from the
  device UI or SysEx.
- Every parameter is addressable by NRPN = parameter ID; a Descriptor may also carry a
  default CC. Descriptor-driven mapping replaces per-module mapping tables where it fits.
- SysEx under manufacturer ID `0x7D` plus a t00t sub-ID: identify, descriptor dump, parameter
  change, edit-buffer request/dump, store-to-slot, bank dump, and a reserved range for Data
  Exchange. Processed on Core 0 into Edit buffers — the same path NRPN/CC takes.

### Host editor

A static Web MIDI single-page app in the repo. It builds its UI from the descriptor dump
(groups → panels, ranges → controls), streams parameter changes, syncs and keeps its own copy
of the Edit buffer, triggers Store, and imports/exports stored-format and external formats
(DX7 `.syx` first). Its SysEx codec is tested against the same golden byte vectors as the
firmware.

## Roadmap

P0 is the measurement gate (breadboard rig, GPIO 22 profiling pin) and recurs at each point
where the performance contract must be re-proven. P1 onward are build phases, numbered in
this doc to follow the tickets' dependency order. Subtractive is the tracer-bullet pilot
(P2–P7); FM is the stress test (P8); the remaining modules migrate afterwards.

| Phase | Ticket | Scope | Blocked by |
|---|---|---|---|
| P0 | #146 | Baseline: today's Core 1 render cost for subtractive and FM | — |
| P1 | #147 | Vocabulary: glossary entries and this design doc | — |
| P2 | #148 | Tracer: subtractive Patches as data — descriptors, stored format, cook, Part slot | #147 |
| P3 | #149 | Live editing over NRPN on subtractive (live vs note-on classes) | #148 |
| P4 | #150 | SysEx core: assembly, identify, parameter change, edit-buffer dump | #149 |
| P4 | #151 | Descriptor dump over SysEx | #150 |
| P0 | #152 | Gate: subtractive on the cooked path vs baseline | #146, #149 |
| P5 | #153 | User banks in flash: Store, Bank Select, survives reflash | #150 |
| P6 | #154 | Quick-edit Page with Compare and Store/rename (subtractive) | #152, #153 |
| P6 | #155 | Multi: Parts, channel routing, per-Part mix, FX in the Multi | #152, #153 |
| P7 | #156 | Web MIDI editor: generic live editing | #151, #153 |
| P7 | #157 | Bank dump, backup and restore | #153, #156 |
| P8 | #158 | FM migration onto the platform (stress test) | #152, #155 |
| P0 | #159 | Gate: FM on the cooked path vs baseline | #158 |
| P8 | #160 | DX7 `.syx` import in the editor | #156, #158 |
| Pn | — | Remaining modules (OPL, chip, groovebox, speech, tracker), one ticket each | #158 |

## Budgets

To be filled by the P0 gates. "Target" is the contract; "Measured" is recorded by the gate
ticket, with the measurement narrative in the module's history log.

### CPU

| Quantity | Module | Baseline (#146) | Cooked path | Target |
|---|---|---|---|---|
| Core 1 time per 256-sample buffer, defined load | subtractive | TBD | TBD (#152) | ≤ baseline |
| Core 1 time per 256-sample buffer, defined load | FM | TBD | TBD (#159) | ≤ baseline |
| Voice headroom (max voices before underrun) | subtractive | TBD | TBD (#152) | no loss |
| Voice headroom (max voices before underrun) | FM | TBD | TBD (#159) | no loss |
| Core 0 cook time per Patch | subtractive | n/a | TBD (#152) | TBD |
| Core 0 cook time per Patch | FM | n/a | TBD (#159) | TBD |
| Parameter change → audible latency | all | n/a | TBD (#152) | ≤ 1 block (≈5.8 ms) + transport |

### Memory

| Item | Where | Size | Notes |
|---|---|---|---|
| Descriptor tables (module + shared) | flash | TBD | `constexpr`, never touched by Core 1 |
| Factory banks | flash | TBD | compiled in |
| Cooked slot × 2 per Part | SRAM | TBD | × N Parts per module |
| Edit buffer per Part + Multi | SRAM | TBD | Descriptor-domain values |
| SysEx assembly buffer | SRAM | TBD | bounded; over-length messages discarded |
| User-bank region | flash (end) | TBD | e.g. 128 Patches + 32 Multis per module, 4 KB sectors |

## Reuse inventory

Existing pieces the platform builds on, and how.

| Existing | Where | Reuse |
|---|---|---|
| `ParamExchangeT` lock-free double buffer | [src/engine_base.h](../src/engine_base.h) | Pattern for the per-Part cooked slots; itself narrows to per-note and Multi-level state |
| Generic MIDI controller (active → shadow copy per batch) | [src/midi/midi_controller_generic.h](../src/midi/midi_controller_generic.h) | Unchanged; the reason cooked patches live outside `ParamExchange` |
| MIDI parser | [src/midi/midi_parser.h](../src/midi/midi_parser.h) | Extended to assemble F0…F7; today it drops System Common bytes |
| Input pipeline dispatch (`InputCategory`, mapping tables, `midi_dispatch_program_change`) | [src/input_layer.h](../src/input_layer.h), [src/midi/midi_dispatch.h](../src/midi/midi_dispatch.h) | Program Change / Bank Select as Configuration; descriptor-driven NRPN/CC replaces per-module tables in `src/engines/*/input_subsystem.cpp` |
| USB-MIDI and DIN transports | [src/midi/usb_midi.cpp](../src/midi/usb_midi.cpp), [src/midi/uart_midi.cpp](../src/midi/uart_midi.cpp) | SysEx input on both; both are receive-only today (see open questions) |
| FM channel → Patch table on Core 0 | [src/engines/fm/engine.h](../src/engines/fm/engine.h) (`fm_channel_patch`) | Precedent for Part → Patch routing |
| FX chain and `EffectParams` | [src/fx/](../src/fx/), [src/engine_base.h](../src/engine_base.h) | Described by the shared common-components Descriptor table; owned by the Multi |
| Widget library, Page framework, encoder navigation | [src/wslcd/](../src/wslcd/), [src/encoder_nav.h](../src/encoder_nav.h) | Quick-edit Page from Value bar / Value row / Label |
| Today's const preset tables | [src/engines/subtractive/presets.h](../src/engines/subtractive/presets.h), FM `patches.h` | Source for host-generated factory banks |
| `syx2patch.py` DX7 converter and its tests | [tools/syx2patch.py](../tools/syx2patch.py) | Extended to emit stored-format FM factory banks |
| xm2t00t versioned blob format (magic + version, shared Python/C++ header) | [tools/xm2t00t/](../tools/xm2t00t/) | Precedent for a host-generated, versioned binary format |
| Host test harness | [tools/host_render/](../tools/host_render/) (`test_*.cpp`) | Platform seam tests: MIDI bytes in, commits / SysEx replies / storage writes out |
| Host renderers (FM / OPL patch render, XM render diff) | [tools/host_render/](../tools/host_render/) | Render equivalence: stored-format → cook vs today's const-table render |
| GPIO 22 profiling pin | [engine.md](engine.md#profiling-gpio) | P0 gate measurements |

## Open questions

1. **Bank Select range split.** Proposed: MSB 0–63 factory banks, 64–127 user banks, leaving
   room for several factory banks per module (e.g. DX7 ROM1–ROM4). Undecided: whether LSB is
   used at all, and what an MSB pointing at an empty bank does (ignore vs load init Patch).
2. **MIDI out.** No transmit path exists today: USB-MIDI only reads, and the breadboard DIN
   port is wired RX-only. SysEx replies (identify, dumps) need a USB TX path; DIN replies need
   a MIDI-out circuit and a free UART TX pin. Is USB-only reply acceptable for the editor?
3. **t00t SysEx sub-ID and message numbering**, including the reserved Data Exchange range.
4. **Parts per Multi** for each module (FM 4–8? groovebox = one bass + one drum Part).
5. **Slot sizes and region size.** Patch and Multi slot sizes per module, how many slots fit
   a 4 KB sector, and the total reserved region at the end of the 4 MB flash.
6. **Shipping DX7 factory banks.** FM's `.syx` inputs and generated `patches.h` are
   gitignored today; factory banks generated from them can't ship in the repo. Does FM ship
   with a project-authored factory bank, and leave DX7 ROMs to user import?
7. **Store glitch length.** How long audio is muted during a sector erase/program, and
   whether that is acceptable mid-performance.
8. **Cook budget for FM.** Whether a full FM Patch cook fits Core 0's control-rate budget
   while MIDI and the display are busy, or needs to be incremental per parameter.

## Decision record

- **Module-native parameters, not an abstract cross-module set.** Each engine keeps its
  identity without a translation layer; one Descriptor table drives UI, MIDI mapping, SysEx
  and the editor.
- **Cook on Core 0.** Keeps the render loop free of the parameter model, so parametrisation
  costs Core 1 nothing.
- **"Multi", not "Performance".** Performance already names the Performance page.
- **FX in the Multi, not the Patch.** Loading a Patch into one Part must never change FX that
  other Parts depend on.
- **No Program Change for Multis.** A stray Program Change must never swap the whole setup.
- **Engine structs are never stored.** The stored format survives engine refactors; only
  cooking produces engine structs.
- **Host tools over runtime complexity.** Format conversion and import (DX7 `.syx`, OPL
  `.sbi`/`.bnk`) stay in host tools and the editor; runtime SysEx carries only the stored
  format.
