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
                    │ Multi ─▶ cook ─▶ Multi slot [2] ─────┼──▶│ mix by Part, shared FX      │
                    │ notes ─▶ ParamExchange (per-note     │──▶│ note-on: copy note-on-class │
                    │          state only)                 │   │   fields into voice state   │
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
  buffer of cooked patches with a commit flag, and the Multi one more for its cooked mix and
  FX. `ParamExchange` keeps only per-note state, because the generic MIDI controller copies
  the whole active block into the shadow on every MIDI batch — patch-sized state there would
  cost kilobytes per batch — and because Multi edits also come from the UI and SysEx,
  outside a MIDI batch.
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
- **Multi** (`src/patch/multi.h`): N Parts (fixed per module at build time; subtractive and FM 16)
  plus its own settings (`kCommonDescriptors`: FX TYPE, FX MIX, FX P1, FX P2, MASTER). Each
  Part (`kPartDescriptors`): CHANNEL (1–16 or OFF), BANK and PROGRAM (the Patch, as Bank
  Select MSB and Program Change), VOLUME, PAN, FX SEND. FX belong only to the Multi. The
  default Multi puts Part n on channel n with the module's default Patch for it.
- **Channel routing:** a channel event reaches every Part on that channel — notes play on
  each (layered), Program Change loads into each and updates its Patch reference,
  CC7/CC10/CC91 set each Part's VOLUME/PAN/FX SEND (their descriptors' default CCs), other
  CCs and NRPN edit each Part's Patch. NRPN MSB 126 addresses a Part parameter (LSB = its ID)
  on the channel's Parts, MSB 127 one of the Multi's own settings from any channel.
- **Mix (Core 1, once per buffer from the cooked Multi):** a voice's stereo gains are its
  Part's pan gains times its volume; the FX gets the sent share of the mix (each Part times
  its FX SEND), and FX MIX crossfades only that share dry/wet, so the unsent share stays dry
  at unity; MASTER scales the output. At unity volume, full send and unity master the mix is
  bit for bit the single-Patch mix; what that costs is a loop-invariant branch per sample per
  voice (send below full?) and per output sample (master below unity?). A sounding Part with
  send below full adds a per-sample accumulation for its voices.
- **Stored Multi:** the Patch header under magic `T0MT`, then the common values, the Part
  count and each Part's values, with byte counts so older and newer schemas load (missing
  values take descriptor defaults, missing Parts the default Multi's). Loading a Multi loads
  each Part's Patch by reference; a Part whose Patch is gone gets factory bank 0, slot 0.

### Editing and storage

- One volatile Edit buffer per Part, and one for the Multi. Loading a Patch replaces the Edit
  buffer without prompting; the UI marks it edited while it differs from its slot.
- Each Edit buffer keeps a copy of its stored version (as loaded, or as last stored) and
  where it is stored (factory bank and slot, user slot, or none for a SysEx dump). Edited
  means the values or the name differ from that copy. Compare publishes the stored version
  in place of the Edit buffer, as an edit of the same Patch (sounding notes follow it); any
  edit or rename ends Compare and applies to the Edit buffer. A Store makes the Edit buffer
  the stored version of its new user slot.
- The Quick-edit Page (`src/patch/quick_edit.h`, rendered by `quick_edit_page.h`) is
  module-agnostic: its rows are the module's quick-edit-flagged parameters (4–8) in ID
  order, then COMPARE and STORE. It is the UI commands' value-editing interaction context:
  it claims them before Page navigation while its Page shows, and while it isn't editing it
  leaves `+`/`-` and `exit` to Page navigation. A value step is one encoder detent:
  1 for an ENUM or a range up to 127, about 6% for an EXPONENTIAL parameter, else 1/128 of
  the range (`param_step()`).
- Flash is written only on an explicit Store, under multicore lockout with audio muted for
  the erase/program time.
- Factory banks are compiled in and read-only, generated by host tools from today's const
  tables and external formats. User banks live in a reserved region at the end of flash,
  outside the UF2 image, partitioned by module ID, in fixed-size slots; a Store rewrites the
  4 KB sector containing the slot.

#### User-bank layout

`src/patch/user_bank.h`. The region is the last 384 KB of the 4 MB flash
(`0x3A0000`–`0x3FFFFF`): eight 48 KB partitions, one per module ID 1–8. Module 1's
partition is the topmost and each further ID sits one partition lower, so a module ID past 8
extends the region downward without moving any existing partition. Every build derives the
layout from the same constants, whichever module it contains, and writes only its own
partition; the UF2 image covers only the program, so flashing any module build leaves the
region alone.

| Within a partition | Offset | Slots | Slot size | Per 4 KB sector |
|---|---|---|---|---|
| Patch slots | 0 | 128 | 256 B | 16 |
| Multi slots | 32 KB | 32 | 512 B | 8 |

A slot holds one stored Patch followed by `0xFF` to the slot's end; a slot without the Patch
magic is empty. Slots never straddle a sector. A module whose stored Patch would exceed 256 B
fails to compile. Loading a slot goes through `patch_decode()`, so a Patch for another
module ID is rejected and an older schema loads with defaults for the parameters it lacks.

A Store (`UserBank::store_patch()`) copies the slot's sector to a 4 KB SRAM buffer, replaces
the slot, and rewrites the sector; storing what the slot already holds writes nothing. The
device writer (`src/patch/user_flash.cpp`):

1. parks Core 1 in a RAM-resident wait at its next buffer (`src/core1_park.h`; the SDK's
   multicore lockout would take over the inter-core FIFO the buffer protocol uses),
2. zeroes both audio buffers,
3. masks every Core 0 interrupt except the audio DMA's, whose handler is RAM-resident and
   register-level, so I2S keeps clocking out silence,
4. erases and programs the sector, unmasks, releases Core 1, and verifies the sector by
   reading it back.

Core 1 drops the buffer requests queued while it was parked and resumes with the next one.
DIN MIDI bytes beyond the UART's 32-byte FIFO that arrive during the write are lost; USB
MIDI waits in the host. If the firmware image ever reaches into the region, the device has
no user bank (every slot empty, every Store refused); every module build also fails at
link time if its image would reach `0x3A0000` (`tools/check_image_size.cmake`), so no UF2
can overwrite the region.

### MIDI control and SysEx

- Program Change on a Part's channel loads a Patch into that Part; Bank Select MSB picks the
  bank: MSB 0–63 is factory bank MSB, 64–127 user bank MSB − 64, and Program Change n is
  slot n of that bank (`PatchPlatformT::load_program()`). Each module has one user bank
  today (MSB 64, 128 slots). An empty slot, a missing bank or a program past a bank's end is
  ignored: the Part keeps its Patch. Bank Select LSB is unused. Both are Configuration inputs
  in the Input pipeline. Multis are selected only from the device UI or SysEx.
- Every parameter is addressable by NRPN = parameter ID; a Descriptor may also carry a
  default CC. Descriptor-driven mapping replaces per-module mapping tables where it fits:
  a module's explicit mapping entries come first, and one catch-all entry hands every other
  CC to the platform. NRPN data (14-bit) and default-CC values (7-bit) are scaled linearly
  onto the parameter's range; a Data Entry MSB alone is bit-replicated so 7-bit controllers
  reach full scale. Exact values travel by SysEx.
- An edit reaches sounding notes only while their Part still holds the Patch they started
  on: a Program Change leaves held notes on their old Patch (publishes carry a load ID so
  Core 1 can tell a new Patch from an edit).
- Note-on fields reach a voice through its per-note parameters: Core 0 copies them from the
  Part's latest cook (published or not) into the note at note-on, together with the Patch's
  load ID, so a note always gets every edit made before it and its pitch maths always
  matches its note-on fields.
  A module whose note-on fields are too large for the per-note parameters (FM: most of a
  ~250-byte cooked Patch) has Core 1 copy them instead, when the note starts, from its
  Part's current or previous cook: each publish carries a cook sequence number, the note
  carries its Part's latest one, and a note whose cook hasn't reached Core 1 yet waits a
  buffer — the same guarantee, but for one case: an edit still pending on Core 0 when a
  Program Change replaces it, within one buffer, never reaches the note played between
  them.
- SysEx (`src/patch/sysex.h`), processed on Core 0 into Edit buffers — the same path NRPN/CC
  takes. Every message is `F0 7D 54 <command> <body> F7`: `0x7D` is the non-commercial
  manufacturer ID, `0x54` ('T') the t00t sub-ID.

  | Command | Message | Body |
  |---|---|---|
  | `01` | identify request | — |
  | `02` | identify reply | module ID, schema version, protocol version (2), Part count, parameter count (2 × 7 bits, LSB first) |
  | `10` | parameter change | Part, parameter ID (2 × 7 bits, LSB first), value (int21) |
  | `20` | edit-buffer request | Part |
  | `11` | Multi parameter change | Part (`7F` = the Multi's own settings), parameter ID (2 × 7 bits), value (int21) |
  | `21` | edit-buffer dump (both directions) | Part, stored Patch (packed) |
  | `22` | Multi edit-buffer request | — |
  | `23` | Multi edit-buffer dump (both directions) | stored Multi (packed); loading one loads its Parts' Patches |
  | `30` | store request | Part, user slot (2 × 7 bits): store the Part's Edit buffer there |
  | `31` | store reply | Part, user slot (2 × 7 bits), result: `0` OK, `1` no such slot, `2` bad Patch, `3` flash write failed |
  | `32` | Multi store request | Multi slot (2 × 7 bits) |
  | `33` | Multi store reply | Multi slot, result (as `31`) |
  | `34` | Multi load request | Multi slot (2 × 7 bits): select that Multi |
  | `35` | Multi load reply | Multi slot, result: `0` OK, `1` bad magic, `2` wrong module, `3` truncated, `4` empty slot |
  | `36` | bank dump request | — |
  | `37` | bank slot (both directions) | slot kind (`0` Patch, `1` Multi), slot (2 × 7 bits), the stored Patch or Multi (packed), or no data for an empty slot |
  | `38` | bank slot reply | slot kind, slot (2 × 7 bits), result (as `31`) |
  | `39` | bank dump end | Patch slot count, Multi slot count (2 × 7 bits each) |
  | `3A`–`3F` | *reserved:* more store and bank messages | |
  | `40` | descriptor dump request | — |
  | `41` | descriptor table begin | table, descriptor count (2 × 7 bits) |
  | `42` | descriptor | table, ID, min, max, default (int21 each), unit, curve, class, flags (bit 0 quick edit), default CC (`7F` = none), name, group |
  | `43` | descriptor labels | table, ID, label count, labels (ENUM parameters with labels, right after their descriptor) |
  | `44` | descriptor dump end | — |
  | `45`–`4F` | *reserved:* more descriptor messages | |
  | `60`–`7F` | *reserved:* Data Exchange transfers | |

  A descriptor dump sends table 0 (the module's), table 1 (the Multi's own settings) and
  table 2 (one Part's parameters), the last two from `src/patch/common.h`. It is chunked — one message per descriptor, each within
  `MIDI_SYSEX_MAX`, checked at compile time — and paced by backpressure: the next message goes
  out only when the outgoing queue has room, so a slower transport (a future DIN out) would
  pace it the same way. Other SysEx is handled while a dump is in progress; a reply that
  finds the queue full is held and sent before the dump continues. Strings are a
  length byte then 7-bit ASCII; unit, curve and class codes are the `descriptor.h` enum values.

  A **bank dump** answers request `36` with one `37` per user-bank slot — every Patch slot in
  order, then every Multi slot — and `39` with both slot counts, paced by the outgoing queue
  like the descriptor dump (after any descriptor dump in progress). A slot carries its stored
  Patch or Multi byte for byte without the slot's `0xFF` padding; an empty slot, or one holding
  nothing loadable for this module, carries no data. A **bank load** is the same `37`
  messages sent to the device: each is written into its slot as it is (no data empties the
  slot; the header is checked as for a Store, so another module's Patch is refused) and
  answered by `38`. The sender waits for each answer before sending the next slot, so nothing
  arrives over DIN while flash is being written. Restoring a dump into the same module
  therefore leaves the user-bank region byte for byte as it was dumped, older-schema Patches
  included. A slot needs a sector rewrite only if its contents change, each with the brief
  audio mute of a Store.

  int21 is two's complement over three 7-bit bytes, least significant first. IDs and counts are
  2 × 7 bits, least significant first. Packed data
  carries 8-bit bytes in groups of up to 7, each group led by a byte holding their high bits.
  Unknown commands, malformed messages and other manufacturers' SysEx are ignored; a dump of
  another module's Patch is rejected. The parser assembles F0…F7 into a 256-byte buffer per
  transport (a longer message is discarded whole; real-time bytes inside pass through).
  Replies go out over USB-MIDI only. Golden byte vectors:
  `tools/host_render/golden/sysex_subtractive.txt`, written by `tools/sysex_vectors.py`.

### Host editor

`tools/editor/` ([README](../tools/editor/README.md)): a static Web MIDI page for Chromium
browsers, opened straight from the file system (no server). It identifies the device, reads the descriptor
dump and builds its UI from the module table (groups → panels; labelled ENUMs → lists,
other ranges → a slider, exponential where the descriptor's curve says so, plus an exact
value box). It loads a Part's Edit buffer into its own copy, streams each edit as a
parameter change, can push the copy back as an edit-buffer dump, and stores by pushing the
copy and then sending a store request, so the slot gets what the editor shows.

It exports the copy as a Patch file and imports one into the copy (pushing it to the
device; a Patch for another module is refused, an older schema's takes defaults), and
backs up and restores the whole user bank as a bank file, through the bank dump and bank
load above. A restore refuses a bank file for another module and stops at the first slot
the device refuses.

On an FM device it imports DX7 `.syx` files (a 32-voice bulk dump or a single-voice dump),
converting each voice to a stored FM Patch byte for byte as `tools/syx2patch.py bank` does
for the factory banks. A chosen voice is imported into the copy and pushed, so it plays and
can be stored like any Patch; the whole file can be written into consecutive user Patch
slots with bank slot messages, one at a time as a restore sends them. Both converters are
tested against the same fixture file (`tools/host_render/golden/dx7_syx.txt`).

#### Files

- **Patch file** (`.t0p`): one stored Patch, header and payload, as it is.
- **Bank file** (`.t0b`), all integers little-endian:

  | Offset | Size | Field |
  |---|---|---|
  | 0 | 4 | magic `T0BK` |
  | 4 | 1 | format version (1) |
  | 5 | 1 | module ID |
  | 6 | 2 | Patch slot count |
  | 8 | 2 | Multi slot count |
  | 10 | … | each Patch slot, then each Multi slot: length (2 bytes, 0 = empty), then the stored Patch or Multi as the bank dump carries it |

  The format is the editor's (`tools/editor/files.js`); the device only sees the bank
  slot messages made from it.

The editor sends nothing to a device whose protocol version isn't the one it speaks (2),
nor while its copy is for a different module or schema than the device reports. Its SysEx
codec is tested against the firmware's golden byte vectors.

Still to come: Multi editing (the Multi and Part tables are in the dump but not shown).

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

Baseline load (method in each module's history log): one patch (subtractive Fairlight,
FM E.ORGAN 1), FX off, 0 / 1 / 2 / 4 / 8 / 16 notes held. Core 1 figures are GPIO 22 duty
as % of the 5.805 ms buffer period, idle (0) / 4 / 16 voices.

| Quantity | Module | Baseline (#146) | Cooked path | Target |
|---|---|---|---|---|
| Core 1 duty per buffer, idle / 4 / 16 voices | subtractive | 0.57 / 22.1 / 81.4% | 0.57 / 19.8 / 72.7% (#152, after the per-voice locals fix): −10.7% at 16 voices | ≤ baseline |
| Core 1 duty per buffer, idle / 4 / 16 voices | FM | 0.76 / 19.0 / 68.7% | 0.76 / 19.0 / 68.7% (#159): unchanged | ≤ baseline |
| Voice headroom at `MAX_VOICES` = 16 | subtractive | 16 fit, 18.6% spare | 16 fit, 27.3% spare (#152) | no loss |
| Voice headroom at `MAX_VOICES` = 16 | FM | 16 fit, 31.3% spare | 16 fit, 31.3% spare (#159) | no loss |
| Core 0 cook time per Patch | subtractive | n/a | cook 1.0–1.7 µs warm, ≤ 20.5 µs cold; Program Change (decode + cook + publish) 3.2 µs warm, ≤ 59 µs cold (#152) | TBD |
| Core 0 cook time per Patch | FM | n/a | cook 23 µs warm, 74–136 µs cold, ≤ 166 µs; Program Change (decode + cook + publish) 128 µs warm, 274–328 µs cold, ≤ 390 µs (#159) | TBD |
| Parameter change → audible latency | all | n/a | subtractive and FM: no delay audible on parameter edits or Patch changes (by ear, not timed); by design 1–2 buffers | ≤ 1 block (≈5.8 ms) + transport |

### Memory

| Item | Where | Size | Notes |
|---|---|---|---|
| Descriptor tables (module + shared) | flash | TBD | `constexpr`, never touched by Core 1 |
| Factory banks | flash | subtractive: 11 × 45 B; FM: 175 B per Patch (the 8 DX7 ROM banks: 256 × 175 B = 44 KB, locally generated) | compiled in |
| Cooked slot × 2 per Part | SRAM | subtractive: 84 B/slot × 16 = 1.3 KB; FM: 256 B/slot × 16 = 8 KB | plus Core 1's own per-Part (current + previous) and per-voice state: subtractive 1.7 KB; FM 8 KB per-Part and a 250 B Patch copy per voice (4 KB) |
| Edit buffer per Part + Multi | SRAM | subtractive: 1.8 KB platform state (16 Parts); FM: 24 KB (16 Parts × 151 values, twice) | Descriptor-domain values, name, cooked copy, NRPN state; the Multi adds ~0.8 KB (values, cooked mix and its slot) |
| SysEx assembly buffer | SRAM | TBD | bounded; over-length messages discarded |
| User-bank region | flash (end) | 384 KB: 8 modules × (128 Patches × 256 B + 32 Multis × 512 B) | layout above |
| Store sector buffer | SRAM | 4 KB | one per module build, in the user bank |

Cook time is measured with `make PATCH_PROFILE=1` (`src/patch/profile.h`): Core 0 cycle
counts around cook, load and edit, read from RAM through the debug probe. "Warm" is with the
code in the XIP flash cache (back-to-back edits), "cold" the first run after other Core 0 work
has evicted it.

## Reuse inventory

Existing pieces the platform builds on, and how.

| Existing | Where | Reuse |
|---|---|---|
| `ParamExchangeT` lock-free double buffer | [src/engine_base.h](../src/engine_base.h) | Pattern for the per-Part cooked slots; itself narrows to per-note and Multi-level state |
| Generic MIDI controller (active → shadow copy per batch) | [src/midi/midi_controller_generic.h](../src/midi/midi_controller_generic.h) | Unchanged; the reason cooked patches live outside `ParamExchange` |
| MIDI parser | [src/midi/midi_parser.h](../src/midi/midi_parser.h) | Extended to assemble F0…F7; today it drops System Common bytes |
| Input pipeline dispatch (`InputCategory`, mapping tables, `midi_dispatch_program_change`) | [src/input_layer.h](../src/input_layer.h), [src/midi/midi_dispatch.h](../src/midi/midi_dispatch.h) | Program Change / Bank Select as Configuration; descriptor-driven NRPN/CC replaces per-module tables in `src/engines/*/input_subsystem.cpp` |
| USB-MIDI and DIN transports | [src/midi/usb_midi.cpp](../src/midi/usb_midi.cpp), [src/midi/uart_midi.cpp](../src/midi/uart_midi.cpp) | SysEx input on both; both are receive-only today (see open questions) |
| FM channel → Patch table on Core 0 | `fm_channel_patch`, FM's Input subsystem before #158 | Precedent for Part → Patch routing; replaced by the Multi |
| FX chain and `EffectParams` | [src/fx/](../src/fx/), [src/engine_base.h](../src/engine_base.h) | Described by the shared common-components Descriptor table; owned by the Multi |
| Widget library, Page framework, encoder navigation | [src/wslcd/](../src/wslcd/), [src/encoder_nav.h](../src/encoder_nav.h) | Quick-edit Page from Value bar / Value row / Label |
| Today's const preset tables | [src/engines/subtractive/presets.h](../src/engines/subtractive/presets.h), FM `patches.h` | Source for host-generated factory banks |
| `syx2patch.py` DX7 converter and its tests | [tools/syx2patch.py](../tools/syx2patch.py) | Extended to emit stored-format FM factory banks |
| xm2t00t versioned blob format (magic + version, shared Python/C++ header) | [tools/xm2t00t/](../tools/xm2t00t/) | Precedent for a host-generated, versioned binary format |
| Host test harness | [tools/host_render/](../tools/host_render/) (`test_*.cpp`) | Platform seam tests: MIDI bytes in, commits / SysEx replies / storage writes out |
| Host renderers (FM / OPL patch render, XM render diff) | [tools/host_render/](../tools/host_render/) | Render equivalence: stored-format → cook vs today's const-table render |
| GPIO 22 profiling pin | [engine.md](engine.md#profiling-gpio) | P0 gate measurements |

## Open questions

1. **MIDI out over DIN.** Replies go out over USB-MIDI only, since the DIN port is wired
   RX-only; requests arriving over DIN are answered over USB too. DIN replies would need a
   MIDI-out circuit and a free UART TX pin.
2. **Parts per Multi** for the remaining modules (groovebox = one bass + one drum Part?).
   Subtractive and FM have 16.
3. **A project-authored FM factory bank.** The DX7 ROM banks are generated locally and
   can't ship in the repo; without them FM has only the INIT VOICE. A bank of the
   project's own FM sounds would give every build something to play.
4. **Store glitch length.** How long audio is silent around a Store — the park wait (up to
   one buffer, cutting the playing buffer short), the 4 KB erase and 16 page programs, and up
   to two buffers before fresh audio after Core 1 resumes — and whether that is acceptable
   mid-performance. To be measured on the rig.
5. **Bank restore time.** A restore rewrites a flash sector for each slot whose contents
   change, each with a Store's mute. On the rig one rewritten slot is answered in about
   33 ms over USB, so a restore changing all 160 slots takes about 5 s of repeated short
   mutes; whether that is acceptable, and how it sounds, is still to be judged. Batching
   a sector's slots into one rewrite would need the load to span messages.

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
- **Parts on one channel layer.** A channel event reaches every Part on it, the standard
  multitimbral behaviour; a Part on OFF hears nothing.
- **FX MIX crossfades only the sent share.** Full send reproduces the single-Patch sound
  exactly, and a Part with send 0 is untouched by the FX at any mix.
- **A Multi refers to Patches; a missing one loads factory bank 0, slot 0.** Patch edits are
  stored with the Patch, never in the Multi, and a Multi always loads to known sounds.
- **Engine structs are never stored.** The stored format survives engine refactors; only
  cooking produces engine structs.
- **SysEx sub-ID `0x54` ('T') under the non-commercial manufacturer ID `0x7D`**, with
  commands grouped in ranges so store/banks, descriptor dump and Data Exchange can be added
  without renumbering.
- **Bank Select range split, linear Program Change, empty means ignore.** MSB 0–63 factory
  banks leaves room for several per module (e.g. DX7 ROM1–ROM4); PC n = slot n is the same in
  every module; a Program Change into an empty slot or missing bank never disturbs a
  performance. LSB stays free.
- **Fixed, module-independent user-bank layout growing down from the end of flash.** Every
  build agrees on where each module's partition is without knowing other modules' Patch
  sizes, and new module IDs never move existing user data.
- **Store parks Core 1 cooperatively and keeps the I2S clock running.** Audio is silent, not
  stopped, so the DAC never loses its clocks; the inter-core FIFO keeps carrying only the
  buffer protocol.
- **Bank dump and load slot by slot, verbatim.** One message per slot fits the parser's
  buffer for any module whose Patch fits a dump, and copying the stored bytes rather than
  decoding and re-encoding them is what makes a restore byte-for-byte, older schemas and
  all. The dump covers Patch and Multi slots together, since a Multi refers to Patches
  by slot.
- **A bank load is paced by its answers.** DIN input has a 32-byte FIFO and is lost while
  flash is written; waiting for each slot's answer keeps the line quiet during the write on
  either transport, with no flow control in the protocol.
- **Host tools over runtime complexity.** Format conversion and import (DX7 `.syx`, OPL
  `.sbi`/`.bnk`) stay in host tools and the editor; runtime SysEx carries only the stored
  format.
