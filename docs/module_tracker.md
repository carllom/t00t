# T00T — XM Tracker Module

Plays tracker modules (XM / FastTracker 2 format) with far more channels than
the historical Amiga 4-channel limit — a build-time module alongside the
subtractive and groovebox engines. See `engine.md` for the shared dual-core
architecture this engine is built on; `history_tracker.md` for build-phase
results and full performance measurements.

Target: RP2350 @ 150 MHz, PCM5122 I2S DAC, 44.1 kHz stereo output.

## Overview

XM module player: fixed channel-to-voice assignment (no dynamic voice
allocation), a pure sample-based mixer, and host-side conversion from real
`.xm` files into a linked binary blob. There is no live-note input — the
engine plays a loaded song; MIDI provides transport control only.

### Specifications

- **Format**: XM (FastTracker 2) — see Decision Record for why, and for the
  rejected alternatives (MOD, S3M, IT)
- **Channels**: 2–32 (fixed by the module header), stereo, channel N = voice N
- **Interpolation**: linear (a nearest-neighbour mode exists in code, unused
  by default)
- **Instruments**: multi-sample keymaps, volume/panning envelopes (sustain +
  loop), fadeout, autovibrato
- **Effects**: the full XM effect and volume-column set — arpeggio, all
  portamento variants, vibrato, volume/panning slides, retrigger, note
  cut/delay, pattern delay/break/jump, sample offset, ping-pong loops (see
  Glossary and Architecture)
- No SD/streaming: songs are baked into flash as `const` data and copied to
  SRAM at load

### MIDI Mapping (Input Capabilities)

This engine plays a pre-loaded song rather than live notes — MIDI is
transport control only, not note input.

| Message | Function |
|---|---|
| MIDI Start | Transport: seek to order 0, play |
| MIDI Continue | Transport: resume from wherever it stopped |
| MIDI Stop | Transport: stop |
| Program Change | Configuration: seek to the order index given by the program number |

### Display (Presentation Capabilities)

`display.cpp`, tile-rendered (no persistent framebuffer), ~20 Hz refresh,
change-detected redraws only:

- Song title and tracker name (painted once at init — no song-swap UI)
- Channel count
- Current order (position / total), pattern (index / total), and row
- Per-channel activity dots (up to 32, two rows of 16)

The display reads Core-0-local state the player task already holds, so no
reverse channel from Core 1 is needed; the snapshot is one tick ahead of
what's audible, invisible at the ~20 ms tick rate.

## Technical Overview

### Source Layout

- `engine.h` — vestigial `VoiceParams` (unused; real per-tick state flows
  through the tick ring below), `MAX_VOICES = 32`
- `player.h` — Core 0 tick production: pattern walk, effects, envelopes,
  `TickBlock`/`TickRing` (also host-buildable, no pico-sdk headers)
- `player_task.h` / `player_task.cpp` — Core 0 task: song load (flash →
  SRAM), ring priming/refill, play/stop/seek transport
- `mixer.h` — pure-integer, pico-sdk-free voice mixer (`TrackerVoice`,
  `mix_voice()`, loop wrap / ping-pong); shared verbatim with the host build
- `audio_engine.cpp` — Core 1 entry point: consumes the tick ring, renders
  via `mixer.h`
- `input_subsystem.cpp` — MIDI transport control (see MIDI Mapping),
  routed through `input_dispatch()`/the Router (`TRANSPORT`/`CONFIGURATION`
  categories) same as every other migrated engine, via
  `src/midi/midi_dispatch.h`/`midi_controller_generic.h`'s shared dispatch
  layer -- `kMappingTable` has no `NOTE`/`MODIFIER` entries, since this
  engine has no live-note traffic at all
- `display.cpp` — Core 0 status display
- `blob_format.h` — generated C++ mirror of the binary song-blob layout;
  source of truth is `tools/xm2t00t/blob_format.py`
- `tracker_song_blob.h` — generated built-in demo song, linked into every
  build; regenerate from a real `.xm` file with `tools/xm2t00t/xm2t00t.py`

### Build

Build with `make ENGINE=tracker`. No tracker-specific build flags; the
shared `DMA_BUFFER_SIZE` flag was evaluated for this engine specifically
(see Performance below) and left at its default (256).

### Tools

`tools/xm2t00t/` — a pure-Python, stdlib-only host converter that turns a
`.xm` module into this engine's binary blob format, linked as `const` data
(the device never parses XM at runtime). No dedicated `README.md`; usage is
documented by `xm2t00t.py`'s own `--help` and by `blob_format.py`, the
format's source of truth:

```
xm2t00t.py convert <in.xm> <out_prefix>   # writes <out_prefix>_blob.h
xm2t00t.py dump <in.xm>                   # prints the song-structure dump only
xm2t00t.py gen-header <out.h>             # regenerates blob_format.h from blob_format.py
```

`tools/host_render/diff_xm.py` — reference-diff harness: converts a module,
renders both the real device mixer (`render_xm_device`) and `openmpt123`,
and diffs the two WAVs. Runs under the shared host-render harness described
in `engine.md`'s Host DSP Tooling section.

## Architecture

### Deviation From the Shared Layer Model

`architecture.md` proposes a sequencer as another Core 0 input source
calling `voice_alloc_allocate()` and writing `VoiceParams`. This module does
not fit that shape:

- **`voice_alloc` is unused.** In XM, channel N *is* voice N. Fixed
  assignment, no allocation, no stealing, no age tracking.
- **`MAX_VOICES` is 32** for this engine. The reverse-FIFO active-voice
  bitmap is already a `uint32_t`, so no IPC change was needed — but it is
  exactly full.
- **`ParamExchange` semantics change** from latest-wins to an ordered
  handoff (below), since the shared latest-wins exchange would silently
  drop rows of notes.
- **Stereo output is a prerequisite.** Panning is not optional in a
  tracker.

Everything else — build-flag engine selection, per-engine `VoiceParams`, the
I2S DMA path, the profiling pin — carries over unchanged from the shared
layer.

### Core Split

```
Core 0 (control)                        Core 1 (audio)
──────────────────────────────────      ──────────────────────────────────
player_tick()  ~50 Hz                   render_buffer()  on DMA IRQ
  read pattern row (tick 0 of row)        while frames remain:
  effects: porta, vibrato, slides,          if tick_remaining == 0:
           arpeggio, retrig                    consume next tick block
  volume + panning envelopes                   ack via reverse FIFO
  autovibrato                               n = min(frames, tick_rem, SUBBLOCK)
  note trigger / key-off                    for each of 32 voices:
  period -> increment                         mix_voice(accum, n)
  emit TickBlock -----------------------→   clip -> int16 L/R -> DMA buffer

sample DMA from flash -> SRAM             (all sample data in SRAM)
LCD (optional, tiled)                     DMA -> PIO I2S -> DAC
transport: play/stop/seek
```

The player runs on Core 0 because it is a pure function of song state, needs
nothing the mixer owns, and has roughly three million cycles per 20 ms tick
to do about 4000 cycles of work. It also keeps pattern-data flash reads off
Core 1, so Core 1's working set stays purely SRAM and never thrashes the XIP
cache. Core 0 already holds playback position for the display, too.

Timing is still sample-accurate: Core 0 computes *what* to apply; Core 1's
`tick_remaining` counter decides *exactly which sample* it lands on. Who
does the arithmetic and when it takes effect are separable.

### Inter-Core IPC: Ordered Tick Handoff

The shared `ParamExchange` is latest-wins — correct for MIDI, where the
newest note state is the truth and a dropped intermediate update is
harmless. That is not safe here: each tick block contains events (note
triggers, retriggers, sample offsets) that must be consumed exactly once, in
order. A small ring with a one-tick lookahead replaces it instead:

```
Core 0: compute tick N+1, write into ring slot, advance head
Core 1: on tick_remaining == 0, take slot at tail, advance tail,
        push ack through reverse FIFO (non-blocking — Core 1 never stalls)
Core 0: wakes on ack, computes tick N+2
```

A 2-deep ring with head/tail indices (see Decision Record). With 20 ms of
slack per slot even a strict ping-pong is safe, but the ring makes tempo
transitions and jitter harmless.

#### TickBlock contents

```
struct TickBlock {
    uint32_t samples_per_tick;      // 44100 * 2.5 / bpm — per-block, not a global
    ChannelTick ch[32];
};

struct ChannelTick {
    uint32_t inc;                   // Q8.24, 0 = channel silent
    int32_t  tgt_volL, tgt_volR;    // Q15, post-envelope, post-pan
    uint32_t sample_id;             // index into resident sample table
    uint32_t start_pos;             // Q18.14, for triggers and 9xx offset
    uint8_t  trigger;               // generation counter — retrigger detect
    uint8_t  flags;                 // NOTE_ON, NOTE_CUT, KEY_OFF
};
```

`samples_per_tick` is a field in the block, not a global, because `Fxx`
tempo changes have to take effect at the tick boundary they belong to, not
at whatever moment Core 0 happens to write a global.

Effects invisible to Core 1: `Bxx` (jump), `Dxx` (break), `EEx` (pattern
delay), `EDx` (note delay), and speed (ticks-per-row). These only change
which row Core 0 reads next or how long it holds.

#### Startup and underrun

Core 0 primes two blocks before Core 1 starts. If Core 1 ever finds the
ring empty at a tick boundary it renders silence, not stale parameters — a
visible dropout on the profiling pin is far better than a subtle timing
glitch.

### Rendering Pipeline

#### Three timing domains

At 44.1 kHz, 150 MHz, 125 BPM:

| Domain | Period | Work |
|---|---|---|
| Tick | 882 frames (20 ms) | Pattern row, all effects, envelope points, note on/off, period → increment |
| DMA buffer | 256 frames (5.8 ms) | IRQ, buffer swap, wake Core 1 |
| Sub-block | ≤64 frames (1.45 ms) | Ramp deltas, voice state load/store, increment latch |
| Sample | 1 frame (22.7 µs) | Interpolate, 2 multiplies, 2 accumulates, advance |

The three are independent: buffers do not align to ticks, and sub-blocks are
cut short wherever a tick boundary falls (13 full 64-frame blocks plus a
50-frame remainder, at 125 BPM). The sub-block is not a unit of output — it
is a unit of parameter constancy. Within one, every voice's volume ramp is
linear and its increment is fixed.

Cycle budget: 150 MHz / 44100 = 3401 cycles per output frame.

#### Render loop

```cpp
void render_buffer(int16_t *out, int frames) {
    int done = 0;
    while (done < frames) {
        if (tick_remaining == 0) {
            tick = ring_take();                     // ack to Core 0
            if (!tick) { render_silence(...); break; }
            samples_per_tick = tick->samples_per_tick;
            apply_tick(tick);                       // latch inc/targets/triggers
            tick_remaining = samples_per_tick;
        }
        int n = min3(frames - done, tick_remaining, SUBBLOCK);

        memset(accum, 0, n * 2 * sizeof(int32_t));
        for (int v = 0; v < 32; v++)
            mix_voice(&voice[v], accum, n);
        clip_store(accum, out + done * 2, n);       // __ssat, int32 -> int16

        done += n;
        tick_remaining -= n;
    }
}
```

#### Voice mixer

```cpp
void mix_voice(Voice *v, int32_t *acc, int n) {
    if (!v->active) return;

    int32_t volL = v->cur_volL, volR = v->cur_volR;      // Q15, current
    int32_t dL = (v->tgt_volL - volL) / n;               // per-sample ramp step
    int32_t dR = (v->tgt_volR - volR) / n;

    uint32_t pos = v->pos;                                // Q18.14
    uint32_t inc = v->inc;                                // pre-shifted to Q18.14
    const int8_t *s = v->data;

    while (n > 0) {
        int run = min(n, samples_to_loop_end(v, pos, inc));
        for (int i = 0; i < run; i++) {
            uint32_t idx = pos >> 14;
            int32_t   f  = pos & 0x3FFF;
            int32_t   a  = s[idx] << 8, b = s[idx + 1] << 8;
            int32_t  smp = a + (((b - a) * f) >> 14);
            acc[0] += (smp * volL) >> 15;
            acc[1] += (smp * volR) >> 15;
            acc += 2;
            pos  += inc;
            volL += dL;  volR += dR;
        }
        n -= run;
        if (n) { wrap_loop(v, &pos); if (!v->active) break; }
    }
    v->pos = pos;  v->cur_volL = volL;  v->cur_volR = volR;
}
```

Notes:

- `samples_to_loop_end()` hoists the wrap test out of the per-sample path.
  Ping-pong loops use a direction flag (`TrackerVoice::backward`) and a
  mirrored read: a per-*batch* branch picks the forward or backward inner
  loop and the matching run-length helper, so a plain forward loop or
  one-shot voice pays zero added cost in the per-sample path. The boundary
  reflection itself (`wrap_ping_pong()`, `mixer.h`) is signed 64-bit and
  only runs when a voice actually crosses a loop edge — not a per-sample
  cost either.
- `s[idx + 1]` reads one past the end at the boundary. The host converter
  appends a guard sample (loop-start value for looped, last value for
  one-shot) so no bounds check is needed.
- `/n` costs one divide per output frame. When `n == SUBBLOCK` — the common
  case — multiply by a precomputed reciprocal and only divide on the ragged
  remainder blocks.
- Outer loop is voice, inner loop is samples. Voice state stays in
  registers for the whole run.

#### Volume ramping — and what does not need it

Volume must ramp: a step change in amplitude is a discontinuity in the
output waveform, i.e. an audible click, and XM retriggers constantly.
Reaching the target by the end of the sub-block gives a 1.45 ms ramp — short
enough to sound instant, long enough to be silent.

Pitch must not ramp: a stepped increment is a frequency discontinuity, not
an amplitude one — no click. Vibrato and portamento stepping once per tick
is what real trackers do and is part of the characteristic sound.

#### Sub-block size

The trade is per-voice setup cost against control resolution. Setup is
~20–40 cycles per voice per sub-block. At 64 frames that amortises to
noise; at 16 frames it is meaningfully more overhead; at 256 there is
audible ramp lag on fast retriggers. 64 is the default.

Control resolution is decoupled from the DMA buffer size, so buffer size
becomes a pure latency-vs-IRQ-overhead choice. The tracker has no live
input, so a larger buffer is viable if IRQ overhead ever needs trading
against latency (see Decision Record).

### Fixed-Point Formats

**Increment: `uint32_t`, Q8.24.** The increment is a ratio, not an index:
`inc = f_note / f_mix`. XM's nominal sample rate is 8363 Hz, so at 44.1 kHz
a sample at its base note has inc ≈ 0.19; worst realistic case is ~16–32×.
Eight integer bits covers this with room to spare.

**Position: `uint32_t`, Q18.14.** Detune error in cents ≈ 1731 / (increment
in LSBs):

| Format | inc = 1.0 | inc ≈ 0.19 (typical) | 2 oct below base |
|---|---|---|---|
| Q22.10 (subtractive engine) | 1.7 ¢ | 8.9 ¢ | 35 ¢ |
| Q18.14 (this engine) | 0.11 ¢ | 0.56 ¢ | 2.2 ¢ |

XM's own finetune resolution is ~0.78 ¢, so Q18.14 stays below the format's
own error floor; the subtractive engine's Q22.10 format would be audibly out
of tune for sample playback (beating in any unison), since the typical case
already spends most of its fractional resolution.

Q18.14 caps sample length at 262,144 samples (256 KB at 8-bit); the host
converter enforces this at convert time.

The increment is stored Q8.24 in the `TickBlock` and pre-shifted to Q18.14
by Core 1 once at latch time, so the shift is free.

### Panning

Pan lives in the shared `VoiceNoteBase` struct, not a tracker-specific
field (resolves `architecture.md`'s open question 1) — for a tracker it is
not optional: XM has per-channel default panning, panning envelopes, `8xx`
and `E8x` commands, and stereo separation is central to how tracked music
sounds.

The mixer takes pre-resolved `volL`/`volR` Q15 pairs rather than a
volume+position pair, so the pan law is applied on Core 0 once per tick
rather than per sample.

### Song Blob Format

A host-side tool (`tools/xm2t00t/`) converts `.xm` into a t00t-native binary
blob linked as `const` data — the device never parses XM. Single flat blob,
fixed-size headers, every offset a byte offset from blob start (never a
pointer). `blob_format.py` declares every struct once and is used both to
pack the blob and to generate `blob_format.h`, its C++ mirror, so the two
views of the layout can't drift apart.

`SongHeader -> order_table, PatternHeader[]+Event[] (6 bytes/cell),
InstrumentHeader[] (keymap, envelopes in tick units, vibrato) ->
SampleHeader[]`, each sample carrying a precomputed 96-entry Q8.24 note →
increment table (`periods.py`, linear and Amiga frequency modes) so the
device never runs period math. XM effects (including the `Exy`/`Xxy`/`Fxx`
commands that overload one letter for several meanings) are normalized into
named `Effect`/`VolEffect` enums (`effects.py`) rather than left as raw
nibbles for the device to switch on. `sample_data_offset`/`sample_data_bytes`
mark one uninterrupted PCM+guard-byte region meant for a straight `memcpy`
into SRAM.

v1 hard limits, enforced at convert time with an actionable message and a
non-zero exit (no blob written): any single sample over the Q18.14 cap
(262,144 frames), or total sample data over an SRAM budget (`--budget-kb`,
default 380 KB). No dynamic-loading simulator yet (see Future/TODO) — just
a static sum.

**Known limitation**: the Amiga-mode period table and both modes' finetune
handling are implemented from XM/FT2's public formulas and sanity-checked
(note C-4, finetune 0 → exactly 8363 Hz; one octave up doubles the
frequency), not verified bit-exact against a real FT2 period dump.

### Memory Strategy

SRAM is 520 KB total. After code, stacks, DMA buffers and mixer scratch,
realistically 350–400 KB is available for sample data.

Sample data must live in SRAM: the XIP cache is 8 KB, and 32 voices reading
scattered addresses at non-integer strides would thrash it continuously and
evict Core 0's code too. This applies equally to flash and to PSRAM over
QMI CS1. The mixer inner loop is `__not_in_flash_func`.

v1: the module must fit in SRAM. Modules are baked into flash as `const`
data (no SD on `breadboard_rp2350`) and copied to SRAM at song load. See
Future/TODO for dynamic loading.

## Status and Plan

### Performance

32 voices ≈ 30% of Core 1 (~31.5 cycles/frame/voice, flat regardless of
loop direction or interpolation, including a deliberately worst-case tight
loop in the mix). Linear interpolation costs about a third more than
nearest-neighbour but is the default (see Decision Record). Idle and
per-voice duty cycle are identical at DMA buffer sizes 256 and 512.
Measured on `breadboard_rp2350`. Full tables: `history_tracker.md`.

### Future / TODO

- **Long tail of FT2 quirks** (tracked in #25, deferred, no natural
  completion point): `E60` pattern loop (including nested/interacting
  cases), envelope handling on note-off, portamento with a changed
  instrument, arpeggio wraparound at high speeds, and open-ended
  corpus-driven fixes against a real module set.
- **Retrofit sub-block parameter ramping to the subtractive engine** — the
  pattern is proven here; not yet ported. Predicted meaningful voice-cost
  reduction there (see `history_tracker.md` for the reasoning).
- **Dynamic sample loading** (phase 2) — a full design (host-computed load
  schedule, Belady's MIN eviction, static placement, a deterministic
  converter-side simulator) exists but is not built; blocked on the FT2
  quirk tail settling player semantics first (#23). See
  `history_tracker.md` for the design.
- **Host-side SRAM size reduction** beyond the current 8-bit conversion —
  trimming past `loop_end`, truncating to actual reach, deduplicating
  identical instruments, and per-sample decimation from known increments
  are all designed but not implemented. See `history_tracker.md`.
- **Global effects** (delay/reverb) as a stereo send — not XM-spec
  behaviour, but there is Core 1 headroom for it now.
- **Known issue, unscoped**: key-off does not deactivate a `TrackerVoice`
  in `mixer.h` — a channel that has been key-off'd (or a song that has just
  ended) without being retriggered stays `active` and fully mixed at zero
  output forever; only a one-shot sample playing through to its natural end
  clears it. Harmless for a short demo loop; a long song that racks up
  key-offs without retriggering those channels would burn Core 1 cycles on
  voices nobody can hear.

## Decision Record

1. **XM (FastTracker 2) is the target format.** Its channel count is fixed
   in the header (2–32), so the voice budget is known at load time and the
   active-voice bitmap still fits one `uint32_t`; it has no per-voice
   resonant filter, so voice cost stays interpolate + scale + accumulate;
   its instruments (envelopes, multi-sample mapping) are musically rich
   enough to be worth the work; and it has a large free test corpus (Mod
   Archive) plus a reference renderer (`openmpt123`). Rejected: **MOD**
   (the limitation being escaped, not a starting point — falls out nearly
   free once XM works); **S3M** (simpler, but musically weaker for the same
   mixer work); **IT** (New Note Actions make peak voice count unbounded,
   breaking both the fixed-channel model and the bitmap — a separate
   project). Multi-format support later is feasible without an
   architecture change: the voice/mixer engine is already format-agnostic;
   only the player and loader are XM-specific, and the loader is offline
   Python, so adding MOD/S3M is mostly host-side work.
2. **Ordered `TickBlock` ring (2 slots) replaces latest-wins `ParamExchange`**
   for this engine — tick events (note triggers, retriggers, sample
   offsets) must be consumed exactly once, in order; latest-wins is safe
   for MIDI (newest state is truth) but would silently drop rows of notes
   here. 20 ms of slack per slot is sufficient, and the ring makes tempo
   transitions and jitter harmless.
3. **32 channels confirmed as the target.** Measured cost lands inside the
   predicted range and comfortably under the ≤50%-of-Core-1 goal. Full
   measurements: `history_tracker.md`.
4. **Linear interpolation, no nearest-neighbour build flag.**
   Nearest-neighbour costs meaningfully less but audibly aliases; linear
   already clears budget with room to spare, so there is no pressure to
   trade it away. The code (`mix_voice_nearest()`) stays in `mixer.h`,
   ready to wire behind a flag later if a future voice-count push needs the
   headroom.
5. **DMA buffer size stays 256.** No measurable idle-duty-cycle difference
   from 512 at this sample rate; 512 would only add latency for no
   offsetting benefit.
6. **Ring atomics are `std::atomic<uint32_t>` head/tail**, not hand-rolled
   ARM barriers — chosen specifically because `player.h` must stay
   host-buildable (`tools/host_render` links it with the host compiler, no
   pico-sdk headers allowed).
7. **Ping-pong loops use a direction flag with a mirrored read**, not
   host-side loop unrolling. Core 1 has cycle headroom to spare, but the
   module's SRAM sample budget does not; unrolling every ping-pong region
   at conversion time would spend from the constrained resource to save
   from the one with slack.
8. **Effect memory is per-command, not global.** Porta up/down and tone
   porta each keep a separate memory slot, matching FT2 rather than
   sharing one — a continuous effect must be restated every row it runs on.
9. **Key-off does not cut a voice directly.** It sets a flag the
   envelope/fadeout machinery consumes every tick; behavior then depends on
   whether the instrument has an enabled volume envelope. Verified against
   `openmpt123`, which turned out to disagree with FT2's own replayer
   source on this point — `openmpt123` is this harness's oracle, so the
   device player follows it.
10. **Envelope evaluation is a fresh interpolation each tick**, not FT2's
    own incremental delta-accumulation (which exists on 1990s hardware to
    avoid a per-tick division Core 0 has cycles to spare for) —
    behaviourally equivalent for well-formed envelopes.
11. **Volume-column commands are a second, independent active-effect
    slot**, since XM allows an effect-column and volume-column continuous
    effect on the same row simultaneously.
12. **`E3x` (glissando control) was tried and reverted.** The mechanism
    was trivial, but snapping tone portamento's audible pitch to the
    nearest semitone each tick is not spec-clear enough to be bounded: two
    reasonable implementations both diverged from `openmpt123` within 1-2
    ticks of the glide starting. Deferred (#25) rather than force-fit.
13. **Display is tile-rendered, Core-0-only, no reverse channel.** Core 0
    already holds playback position, so nothing from Core 1 is needed.
14. **MIDI transport routes through a new `TRANSPORT` Router category**,
    not a bespoke switch — `src/midi/midi_dispatch.h`/`midi_controller_generic.h`
    gained generic Transport dispatch (Start/Continue/Stop) specifically to
    fit this engine, the first module to actually need it. Program Change
    stays `CONFIGURATION`, the same standing convention every other
    migrated module uses, but with its own meaning here: seeking to an
    order rather than selecting a preset — `CONFIGURATION`'s `value.index`
    field means whatever a module's own Handler decides it means.

## Glossary

- **Order**: a position in the song's play sequence; each order references
  one pattern.
- **Pattern**: a grid of rows × channels, the tracker's unit of musical
  data.
- **Row**: one horizontal slice of a pattern — one note/effect event per
  channel, played for the tick(s) the current speed dictates.
- **Tick**: the effect-update rate within a row (speed = ticks per row);
  distinct from the audio sample rate.
- **Channel**: a fixed slot in the song format (2–32); maps 1:1 to a mixer
  voice in this engine (see Deviation From the Shared Layer Model).
- **TickBlock**: the per-tick data Core 0 hands to Core 1 — see Inter-Core
  IPC.
- **Ping-pong loop**: a sample loop that alternates playback direction at
  each boundary instead of jumping back to the loop start.
- **Guard sample**: an extra sample appended past a sample's real data so
  the interpolator's one-ahead read never goes out of bounds.
- **Q8.24 / Q18.14**: fixed-point formats — the number after the dot is the
  fractional bit count.
