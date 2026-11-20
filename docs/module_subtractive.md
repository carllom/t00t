# T00T — Subtractive Engine

The original synthesis engine, predating the module system. See `engine.md`
for the shared dual-core architecture, pin allocation, and IPC every module
(including this one) is built on; `history_subtractive.md` for development
history and full performance measurements.

## Overview

General-purpose subtractive synthesizer — one ADSR + LFO + filter voice per
note, with optional PCM sample playback in place of the synthesized
oscillator.

### Specifications

- **Voices**: 16, dynamically allocated (steal policy: silent > released >
  oldest active — see `engine.md`)
- **Oscillators**: 8 waveform types — sine, square, triangle, saw, noise,
  band-limited (PolyBLEP) square, band-limited saw, and PCM sample playback
- **Envelope**: 1 ADSR per voice (linear attack, exponential decay/release)
- **LFO**: 1 general-purpose LFO per voice (4 destinations: amplitude, pitch,
  duty cycle, filter cutoff) plus 1 dedicated mod-wheel vibrato LFO
- **Filter**: 1 state-variable filter per voice (lowpass/bandpass/highpass/
  notch/off)
- **Effects**: 1 shared post-mix insert (delay, reverb, phaser, flanger,
  chorus, bitcrusher, or overdrive, mono send / stereo return) — global,
  not per-voice
- **Patches**: 11 factory Patches (3 synthesized, 8 sample-based) in a
  compiled-in, read-only factory bank in the platform's stored format (see
  `docs/platform.md`); 12 parameters per Patch (oscillator, LFO, filter)
- **Parts**: 16, one per MIDI channel — each channel plays its own Patch
- No arpeggiator or step sequencer in the engine itself; the VGA board's
  buttons each play one fixed note (see MIDI mapping below)

### MIDI Mapping (Input Capabilities)

| Message | Name | Range | Function |
|---|---|---|---|
| Note On/Off | — | notes 0–127, all channels | plays on every Part of the Multi on the channel (layered), one voice each; 16 voices, dynamically allocated |
| Velocity | — | 0–127 | linear to amplitude |
| Pitch Bend | — | 14-bit | ±2 semitones |
| CC1 | Mod Wheel | 0–127 | dedicated vibrato LFO depth |
| CC7 | Volume | 0–127 | the channel's Parts' VOLUME (Multi); live |
| CC10 | Pan | 0–127 | the channel's Parts' PAN (Multi); live |
| CC91 | FX Send | 0–127 | the channel's Parts' FX SEND (Multi): how much of each Part feeds the FX; live |
| CC72 | FX Param 1 | 0–127 | the Multi's FX P1: delay feedback / reverb room size (any channel) |
| CC73 | FX Mix | 0–127 | the Multi's FX MIX: crossfades the sent share dry/wet (any channel) |
| CC75 | FX Param 2 | 0–127 | the Multi's FX P2: delay time / reverb damping (any channel) |
| CC76 | FX Type | 0–127 | the Multi's FX TYPE, the range split into the FX bands (CC74 in the other modules) |
| CC74 | Cutoff | 0–127 | Patch CUTOFF, scaled onto 20–18000 Hz; live |
| CC71 | Resonance | 0–127 | Patch RESO, scaled onto its range; live |
| CC79 | Env → Cutoff | 0–127 | Patch ENV>CUT, scaled onto ±18000 Hz; live |
| CC12 | LFO Rate | 0–127 | Patch LFO RATE, scaled onto 0–20 Hz; live |
| SysEx | t00t protocol | F0 7D 54 … F7 | identify, exact parameter change, edit-buffer request/dump per Part, store a Part's Edit buffer to a user slot, Multi parameter change, Multi edit-buffer request/dump, Multi store and load (select), descriptor dump, user-bank dump and slot-by-slot bank load (`docs/platform.md`); replies over USB |
| NRPN (CC99/98 + CC6/38) | any Patch parameter | 14-bit | NRPN number = parameter ID (`patch.h`); Data Entry 0–16383 scaled onto the parameter's range (MSB alone reaches full scale, LSB refines); live parameters reach sounding notes, note-on ones the next note |
| CC0 | Bank Select MSB | 0–127 | picks the bank for Program Change: 0 = the factory bank, 64 = the user bank (128 slots); other values select no bank |
| CC32 | Bank Select LSB | 0–127 | captured, currently unused |
| Program Change | — | 0–127 | loads that slot of the selected bank into every Part on the channel (factory: 0–10); an empty slot or missing bank is ignored; sounding notes keep the Patch they started with; never selects a Multi |

Non-MIDI input: VGA board buttons (A/B/C, `vgaboard_rp2350` only) — each
fixed to one note/channel, routed through the same Shaping →
`input_dispatch()` → `set_note` path a MIDI note-on/off uses (see
`engine.md`'s MIDI Input section).

### Display (Presentation Capabilities)

Four Pages (`src/wslcd/page.h`/`header.h`), the shared Widget/Header/Page
library (CONTEXT.md's Widget catalog) applied to subtractive — same
Performance-page layout as OPL's/FM's/chip's own (`docs/module_opl.md` /
`docs/module_fm.md` / `docs/module_chip.md`'s Display sections):

- **Performance** (required, default): the Header's Resource bar (combined
  active-voice/CPU indicator) plus the last-played channel's Patch
  (program number + name, `*` while edited) and
  the three FX CCs (CC73/72/75) as compact Value bars ("FXMIX"/"FX P1"/
  "FX P2") with FX type (CC76) as a Label ("OFF"/"DELAY"/"REVERB"/"PHASER"/
  "FLANGER"/"CHORUS"/"CRUSH"/"DRIVE") and the mod wheel (CC1) as a fourth
  Value bar ("MOD").
- **MULTI** — the Multi (`src/patch/multi_edit.h`, `multi_page.h`): its slot
  (`M03`) and name, then one bar per Part (number, channel or OFF, Patch
  name, filled to its volume), then LOAD and STORE. LOAD picks a Multi slot
  (the Multi's own, else the first stored one) and loads it; STORE picks a
  slot and a name as on EDIT. The only way, besides SysEx, to change the
  whole Multi.
- **EDIT** — the Quick-edit Page (`src/patch/quick_edit.h`,
  `quick_edit_page.h`): the last-played channel's Part (held while
  editing), where its Patch is stored (`F0.02` factory, `U012` user), its
  name with `*` while edited; then the five quick-edit parameters (WAVE,
  LFO RATE, CUTOFF, RESO, ENV>CUT) as Value bars showing what is heard
  (the stored values while comparing), then COMPARE and STORE. Short press starts editing, turning picks a
  row, short press acts on it (edit a value, toggle Compare, Store), long
  press backs out one level. Store picks a user slot (the Patch's own if it
  came from one, else the first empty), then the name: turn to move the
  cursor, press to change the letter under it, press OK to store; a long
  press cancels, as does a Program Change into the Part. Edits go
  through the Part's Edit buffer like NRPN and SysEx; any edit ends Compare.
- **DIAG**: the exact-value detail Resource bar deliberately sacrifices —
  CPU% (PercentageBar), per-voice sounding activity (ActivityGrid), and
  last note (name + octave + velocity). No multitimbral grid or algorithm
  indicator the way FM's/OPL's own DIAG Pages have. Pitch bend isn't shown on either
  Page (dropped along with the old hand-rolled display, matching FM's own
  Performance/DIAG content exactly).

Reachable via the breadboard's rotary encoder (`src/encoder_nav.h`,
docs/engine.md's "Encoder navigation" entry) where one's wired: turn to
change Page, hold to jump to Performance. Performance-only otherwise
(`HAS_ENCODER=0`).

## Technical Overview

### Source Layout

- `engine.h` — `VoiceParams`/`ParamExchange` (via `engine_base.h`'s
  templates): per-note inputs only
- `patch.h` — the module on the patch platform: descriptor table, sample
  list, `SubCookedPatch`, the cook step, the Part slots
- `factory_bank.h` — GENERATED by `gen_subtractive_bank` from `presets.h`;
  the factory bank as stored Patches — do not hand-edit
- `presets.h` — authoring source for the factory bank (`VoicePreset` table
  and names); host tools only, the device never includes it
- `render.h` — the per-buffer render (voices, post-mix FX, interleave),
  host-buildable and shared by Core 1 and the host tools
- `audio_engine.cpp` — Core 1 loop: takes new cooked patches from the Part
  slots, calls `render.h`; parks in RAM while Core 0 stores to flash
  (`src/core1_park.h`)
- `display.cpp` — Performance, EDIT and DIAG Pages (see Display above)
- `sub_platform.h` — access to the module's patch platform instance for
  the display
- `input_subsystem.cpp` — the Input pipeline's module-specific tail:
  mapping table, Handlers, and Voice Allocation Interface calls, built on
  the shared dispatch layer below; owns the patch platform instance
  (`src/patch/platform.h`) and the module's user bank
  (`src/patch/user_bank.h`)

Also draws on shared, non-engine-specific code: `src/controller.cpp`
(VGA-board buttons — subtractive is the only engine that links it, see
`CMakeLists.txt`), `src/button_shaping.h`/`src/sensor_event.h` (button
SensorEvent → Input event Shaping), and `src/midi/midi_dispatch.h`/`.cpp`/
`midi_controller_generic.h` (shared generic per-MIDI-message-type dispatch
helpers, bank-select state, and the full parse-and-dispatch loop — also
used by `fm`).

### Build

Default engine — `make` alone builds it (equivalent to
`make ENGINE=subtractive`). No subtractive-specific build flags. See
`building.md` for board selection, MIDI transport overrides, and flashing.

### Tools

- `tools/host_render/gen_subtractive_bank` — converts `presets.h` into
  `factory_bank.h`, checking the conversion is lossless; `--check` reports a
  stale header.
- `tools/host_render/render_subtractive` — renders every factory Patch
  through the device path and compares against
  `tools/host_render/golden/subtractive_render.txt`; `--write-golden`
  regenerates it after an intended sound change, `--wav <dir>` dumps audio.

Both build with `make host`.

- `tools/sysex_vectors.py` — writes the golden SysEx byte vectors
  (`tools/host_render/golden/sysex_subtractive.txt`) from `factory_bank.h`,
  with its own implementation of the framing and packing; the host test
  `test_sysex` checks the firmware against them.

## Architecture

### Patches and cooking

A Patch is 12 Descriptor values (`patch.h`'s table). Program Change
decodes a factory or user-bank Patch into the channel's Part Edit buffer; NRPN and the
default CCs change single values in it. After either, Core 0 cooks the Edit
buffer into a `SubCookedPatch` — sample pointer, LFO phase increment, Q15
LFO depths, SVF Q and seed coefficient — and publishes it to the Part's
slot. Core 1 never sees Descriptors or stored data.

The cooked patch has two groups, matching the Parameter classes:

- **note-on** (waveform, sample, filter mode): Core 0 copies them from the
  Part's latest cook into the note's `VoiceParams` at note-on, so an edit
  reaches only later notes — every later note, even one started while
  Core 1 is still a buffer behind.
- **live** (everything else): a voice reads them from its Part's newest
  cooked patch every buffer, so an edit reaches sounding notes from the
  next buffer — while the Part still holds the Patch load the note belongs
  to (`VoiceParams::load_id`). Once a different Patch is loaded, the old
  Patch's sounding voices keep a frozen copy of its live values and finish
  on them.

At the start of each buffer Core 1 takes any new cooked patch per Part. A
burst of edits within one buffer coalesces: the Part slot publishes the
first, and Core 0 publishes the latest Edit buffer state on its next pass.

The Patch has no envelope parameters: the ADSR is one fixed shape for every
voice. The note's velocity alone sets amplitude.

### ADSR Envelope

Per-voice state machine on Core 1. Envelope `level` is a float in 0.0–1.0,
converted to Q15 (`level * 32767`) inside the render loop. Attack is **linear**
(additive per sample); decay and release are **exponential** (multiplicative
coefficient per sample) for natural-sounding amplitude curves.

```
States: IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE

IDLE:     level = 0, voice silent
ATTACK:   level += attack_rate, until level >= 1.0 → DECAY
DECAY:    level = sustain + (level - sustain) * decay_coeff,
          until within epsilon of sustain_level → SUSTAIN
SUSTAIN:  level = sustain_level, held while gate is true
RELEASE:  level *= release_coeff, until level < epsilon → IDLE
```

`EnvConfig` holds `attack_rate`, `decay_coeff`, `sustain_level`,
`release_coeff`, and `gated_attack_decay` (default `true`), built from
milliseconds via `env_config(attack_ms, decay_ms, sustain_pct, release_ms)`.
`Envelope` exposes `init()`, `trigger()`, `release(cfg)`, `active()`, and
`advance(cfg)` (returns the current float level). With `gated_attack_decay`
true — the only value any preset here uses today — release from any active
state transitions to RELEASE immediately, using the current level as the
starting point. Set false, a release requested during ATTACK or DECAY is
deferred until they finish naturally instead of interrupting them.

Amplitude chain per sample (Core 1 render loop):
```
env_f = envelope.advance(cfg)          // float 0.0–1.0
level = env_f * 32767                   // Q15
if waveform == WAVE_SAMPLE: raw = osc_sample_play(sample, phase)
else:                       raw = osc_sample(waveform, phase, duty, lfsr, phase_inc)
scaled = (raw * amplitude) >> 15
scaled = (scaled * level) >> 15
if lfo_depth > 0:                       // tremolo, depth pre-converted to Q15
    mod = 32767 - lfo_depth + (lfo_val * lfo_depth) >> 15
    scaled = (scaled * mod) >> 15
if filter_mode != OFF:
    cutoff = base + (level * env_amount) >> 15 + (lfo_val * lfo_filter_depth) >> 15
    F_half = svf_compute_f_half(cutoff)
    scaled = filter.tick(scaled, F_half, Q_q15, mode)
```

The actual Core 1 loop computes `env_f`, the LFO value, and `F_half` once per
64-sample sub-block (`SUBBLOCK` in `audio_engine.cpp`) and linearly ramps each
toward its next target across the block, rather than recomputing them fresh
every sample as shown above — the oscillator phase advance, PolyBLEP
correction, and the filter's own two-pass state update stay genuinely
per-sample.

Current ADSR values:
- Attack:  10ms
- Decay:   100ms
- Sustain: 70%
- Release: 800ms

### LFO

Per-voice LFO on Core 1, driven by a float phase accumulator in [0.0, 1.0)
advanced by `lfo_rate / SAMPLE_RATE` each sample. The phase is scaled to the
fixed-point range and read from `sine_table` (`osc_sine`) for a smooth Q15 value.
Single LFO per voice with independent depth controls for four destinations:

- **Amplitude (tremolo)**: `lfo_depth` — multiplies post-envelope amplitude
- **Pitch (vibrato)**: `lfo_pitch_depth` — offsets `phase_inc` by ±fraction (0.05 ≈ ±1 semitone)
- **Duty cycle (PWM)**: `lfo_pwm_depth` — sweeps duty_cycle ± around center, clamped 1–1022
- **Filter cutoff**: `lfo_filter_depth` — offsets cutoff in Hz (signed)

LFO params in VoiceParams: `lfo_rate` (Hz, shared) plus four depth fields.
`lfo_rate`, `lfo_depth`, `lfo_pitch_depth`, and `lfo_pwm_depth` are floats; the
inner loop converts the depths to Q15 once per buffer. LFO phase state lives on
Core 1 only and is reset to 0 on trigger.

#### Mod-wheel vibrato (dedicated LFO)

Separate from the preset LFO above, each voice has a second, dedicated vibrato
LFO for the MIDI mod wheel: a fixed 5 Hz (`MOD_VIBRATO_HZ`) sine that modulates
pitch by up to ~±50 cents (`MOD_VIBRATO_MAX_Q15`) at full wheel. Its depth comes
from `VoiceParams::mod_depth` (Q15, 0 = off), a live control set by CC1 — it is
not part of a Patch. It stacks on top of
any preset pitch LFO and runs from its own `mod_lfo_phase[v]` accumulator, also
reset to 0 on trigger.

### Waveform Types

```c
enum Waveform : uint8_t {
    WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAW, WAVE_NOISE,
    WAVE_SQUARE_BLEP, WAVE_SAW_BLEP, WAVE_SAMPLE
};
```

The synthesized waveforms are derived from the phase accumulator (no extra
tables needed except sine); `WAVE_SAMPLE` plays back PCM data instead:
- **Sine**: wavetable lookup with linear interpolation (1024-entry table)
- **Square**: sign of phase, with variable duty cycle (0–1023)
- **Triangle**: piecewise linear, 4-quarter ramp
- **Saw**: phase directly scaled to [-32767..32767]
- **Noise**: 16-bit Galois LFSR (polynomial 0xB400), per-voice state, reseeded on trigger
- **Square BLEP**: band-limited square via PolyBLEP correction at both edges
- **Saw BLEP**: band-limited saw via PolyBLEP correction at wrap point
- **Sample**: PCM playback from a `SampleDef` (signed int8 data shifted to Q15),
  linearly interpolated, with optional looping; the phase advances at a
  resampling rate derived from the target vs. base frequency. Dispatched
  separately via `osc_sample_play()` / `osc_sample_advance_phase()`.

PolyBLEP smooths discontinuities over one sample on each side using a quadratic
polynomial residual. Fixed-point Q10 arithmetic, uses RP2350 hardware divider.
The naive (non-BLEP) variants are kept for intentionally aliased/"crusty" sound.

### State-Variable Filter (SVF)

Per-voice SID-style 2-pole (12dB/octave) multimode filter. Produces lowpass,
bandpass, highpass, and notch outputs from shared state variables.

```c
enum FilterMode : uint8_t { FILTER_OFF, FILTER_LP, FILTER_BP, FILTER_HP, FILTER_NOTCH };
```

Implementation: fixed-point SVF with 2-pass integration for stability.
The filter itself is all integer arithmetic. On the RP2350 (Cortex-M33),
`SMULL` provides 64-bit intermediates, so the multiplies use `int64_t` and
no intermediate clamping is needed. State (`lp`, `bp`) lives in `SVFilter`
with `init()` and `tick(input, F_half, Q_q15, mode)`.

#### Per-sample update (2-pass)
```
for pass in 0..1:
    hp = input - lp - (Q_q15 * bp) >> 15
    bp += (F_half * hp) >> 15
    lp += (F_half * bp) >> 15
output = lp | bp | hp | lp+hp depending on mode (input if OFF)
```

#### Coefficient computation
- **F_half** (Q15): `cutoff_hz * 76539 >> 15`, clamped [33, 15564]
  - Approximation of `π * cutoff / sample_rate`, <0.1% error
- **Q** (Q15): `65534 - (resonance << 1)`, clamped to a minimum of 2
  - resonance=0 → Q=65534 (2.0, no resonance)
  - resonance=32767 → Q≈0 (near self-oscillation)

#### Modulation
Per-sample cutoff = base_cutoff + (envelope × env_amount >> 15) + (LFO × lfo_filter_depth >> 15),
clamped 20–18000 Hz. Q is constant per buffer.

Filter state (lp, bp) reset to 0 on voice trigger for clean attacks.

## Status and Plan

### Performance

Idle ~0.6%. One voice ~4.5–5.5% (more with LFO and filter both active). Delay
insert adds ~1.5pp; reverb adds ~8pp. Measured on breadboard_rp2350 at
44.1 kHz / 150 MHz. Full measurement history: `history_subtractive.md`.

## Decision Record

1. **Attack is linear, decay/release are exponential** — matches how
   amplitude envelopes are perceived; a linear decay/release sounds
   unnaturally abrupt.
2. **Envelope, LFO, and filter coefficient are computed once per 64-sample
   sub-block and linearly ramped**, rather than recomputed every sample —
   cuts control-rate cost without an audible behavior change; oscillator
   phase, PolyBLEP correction, and the filter's own per-sample state update
   are unaffected.
3. **Naive (non-BLEP) waveforms are kept alongside their PolyBLEP-corrected
   counterparts** — the aliased versions are an intentional "crusty" sound
   option, not legacy code left behind.
4. **Mod-wheel vibrato is a separate, dedicated LFO**, not a shared instance
   with the preset LFO — a live control (CC1) has to layer independently on
   top of whatever pitch-LFO a preset already programs, and stay at zero
   depth (off) by default regardless of preset.
5. **The SVF uses two-pass integration in fixed point**, with `int64_t`
   intermediates (`SMULL` on the M33) — needed for stability at high cutoff /
   low Q; a single pass is not.
6. **CC10 pan is a live control, not part of a Patch** — it is per-channel
   state applied at note-on, matching mod-wheel depth's treatment.
7. **Program Change is linear, within the bank Bank Select MSB picks** —
   PC n loads slot n, the same numbering every module on the platform uses,
   so every one of the 128 user slots is reachable, rather than
   microKORG-style row/column numbering.
8. **Each VGA board button plays one fixed note**, not the old per-button
   4-note cycle — a bare on/off switch has no state to key a cycle position
   off other than the button itself, and a fixed note keeps the button's
   Shaping config (note, channel, velocity) a plain, self-contained value
   rather than mutable per-button state threaded through the input path.
9. **Program Change now routes through the Router** (`CONFIGURATION`,
   `src/midi/midi_dispatch.h`'s shared synthetic id) instead of bypassing
   the dispatch table directly — the patch-select Handler reads the bank
   value via `midi_channel_bank_msb()` (shared, module-agnostic plumbing)
   rather than a locally duplicated array.
10. **Voice allocation lives inside `set_note()`, not the dispatch loop**
    — `voice_alloc_allocate()`/`release()` are the Voice Allocation
    Interface (CONTEXT.md), reached from a Handler, never interleaved in
    MIDI parsing/dispatch. `set_note()` resolves its own voice via its own
    `note_voice[]` lookup (steal-on-retrigger included); the GPIO
    button path (`controller.cpp`) reaches the same Handler and no longer
    pre-allocates a voice of its own.
11. **One Part per MIDI channel** — every channel keeps its own Patch
    selection with no routing table; channel → Part routing belongs to the
    Multi.
12. **Note-on fields travel in the note's `VoiceParams`, live fields come
    from the Part** — Core 0 computes a note's pitch from the same cooked
    note-on fields it writes into the note, so the two can never disagree,
    whatever Core 1 has taken by then. That adds 8 bytes per voice to the
    `ParamExchange` block, which is copied once per MIDI batch.
13. **`presets.h` is the authoring source for the factory bank** — the
    generator proves the conversion lossless against the render constants
    each preset implies, so factory sounds can't drift silently.
14. **The preset `amplitude` field is not a Patch parameter** — velocity
    always overwrote it, so it never affected sound.
15. **A Program Change leaves held notes on their old Patch** — loading a
    sound is a different act from tweaking one: edits follow sounding
    notes, a new Patch only reaches later notes. The cost is a one-off copy
    of the old live fields into that Part's sounding voices when the new
    Patch arrives, nothing per buffer.
16. **NRPN data is scaled onto each parameter's range** — any 7- or 14-bit
    controller sweeps the full range, at the price of exact integer values
    for wide ranges; exact values are the SysEx path's job.
17. **FX type is on CC76, not CC74** — CC74 is the de-facto cutoff CC, and
    cutoff is a Patch parameter here. The other modules keep FX type on CC74
    until they move onto the platform.
18. **Explicit mapping entries come first, then one catch-all entry hands
    every other CC to the platform** — the descriptor table, not the mapping
    table, decides which CC drives which parameter; a compile-time check
    rejects a default CC an explicit entry would shadow.
19. **The Quick-edit Page edits the last-played Part, held while editing**
    — it matches the Performance page's Patch row, and a note on another
    channel can't move an edit in progress to a different Part.
20. **Sixteen Parts, Part n on channel n by default** — every channel
    plays its own Patch out of the box; Parts on one channel layer, a Part
    on OFF is silent.
21. **Pan, volume and FX send belong to the Part, read by Core 1 per
    buffer** — they follow live edits on sounding notes, so a voice's
    note-on fields carry none of them.

## Glossary

- **PolyBLEP**: Polynomial Band-Limited Step — a correction added at a
  waveform's discontinuity to suppress aliasing, without full oversampling.
- **SVF**: State-Variable Filter — a filter topology that produces
  lowpass/bandpass/highpass simultaneously from the same per-sample state.
- **Q15**: fixed-point format with 15 fractional bits (range roughly
  -1.0..1.0 as a 16-bit signed integer).
- **Sub-block**: a fixed-size chunk of samples (64 here) within one audio
  buffer, used as the unit at which slowly-changing parameters (envelope,
  LFO, filter coefficient) are recomputed and then ramped across, rather
  than recomputed every sample.
