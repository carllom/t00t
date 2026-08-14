# T00T — XM Tracker Module (Design Draft)

Design notes for a tracker module playback engine, built as a new build-time
module alongside the existing subtractive and 303/808 engines. This is a working
design document — decisions recorded here are provisional until code exists.

Target: RP2350 @ 150 MHz, PCM5122 I2S DAC, 44.1 kHz stereo output.

---

## Goals

- Play tracker modules with substantially more than the Amiga 4-channel limit.
- 32 channels of sample playback, stereo, at or below ~50% of Core 1.
- Reuse the existing dual-core structure, I2S output path, and build-flag
  modularity. Reuse the host-side sample tooling approach already used for the
  Fairlight sample corpus.
- Feed the sub-block rendering technique back into the existing engines as a
  performance improvement.

Non-goals (for now):

- IT format support (NNA / virtual voices / per-voice filters).
- Live editing or pattern entry on-device.
- Streaming from SD (no SD on the `breadboard_rp2350` target).

---

## Format Decision: XM (FastTracker 2)

XM is the target format. Rationale:

| Property | Consequence |
|---|---|
| Channel count fixed in header, 2–32 | Voice budget known at load time; no dynamic voice count |
| Max 32 channels | Active-voice bitmap still fits one `uint32_t` FIFO word |
| No per-voice resonant filter | Voice cost is interpolate + scale + accumulate, nothing more |
| Instruments with envelopes, multi-sample mapping | Musically rich enough to be worth the work |
| Large free corpus (Mod Archive) | Real test material, and reference renders via `openmpt123` |

Rejected alternatives:

- **MOD** — falls out nearly free once XM works; not a starting point because it
  is the limitation we are trying to escape.
- **S3M** — simpler (no envelopes), a reasonable stepping stone, but musically
  weaker for the same mixer work.
- **IT** — New Note Actions make peak voice count unbounded, which breaks both
  the fixed-channel voice model and the `uint32_t` bitmap. Plus per-voice
  filters. A separate project, not a variant.

### Multi-format support

Feasible, but the abstraction seam must be in the right place, and most of it
belongs on the host (see *Host Preprocessing* below):

1. **Voice/mixer engine** — fully format-agnostic. Knows nothing about patterns.
2. **Player/sequencer** — per format. Effect memory rules, tick-0 vs later-tick
   semantics, vibrato depth and waveform, portamento-with-instrument-change all
   genuinely differ between MOD/S3M/XM. Each format gets its own state machine,
   or one machine plus a quirk-flags word.
3. **Loader** — moves to the host converter entirely. Effect commands are
   normalised into an internal enum at build time, not at runtime.

Because the loader is offline, adding MOD or S3M later is mostly host-side
Python, not embedded C++.

---

## Deviation From the Existing Layer Model

`architecture.md` proposes a sequencer as another Core 0 input source calling
`voice_alloc_allocate()` and writing `VoiceParams`. **The tracker module does not
fit that shape**, and the deviation should be explicit:

- **`voice_alloc` is unused.** In XM, channel N *is* voice N. Fixed assignment,
  no allocation, no stealing, no age tracking.
- **`MAX_VOICES` becomes 32** for this engine. The reverse-FIFO bitmap is
  already a `uint32_t`, so no IPC change is needed — but it is exactly full.
- **`ParamExchange` semantics change** from latest-wins to ordered handoff
  (see below).
- **Stereo output is a prerequisite.** The current output stage clips mono and
  duplicates to L+R. Panning is not optional in a tracker.

Everything else — build-flag engine selection, Option A per-engine
`VoiceParams`, the I2S DMA path, the profiling pin — carries over unchanged.

---

## Core Split

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

**Rationale for putting the player on Core 0:** it is a pure function of song
state, needs nothing the mixer owns, and has three million cycles per 20 ms tick
to do ~4000 cycles of work. It also keeps pattern-data flash reads off Core 1,
so Core 1's working set stays purely SRAM and never thrashes the XIP cache. And
Core 0 already holds playback position for the display.

**Timing is still sample-accurate.** Core 0 computes *what* to apply; Core 1's
`tick_remaining` counter decides *exactly which sample* it lands on. Who does
the arithmetic and when it takes effect are separable.

---

## Inter-Core IPC: Ordered Tick Handoff

The existing `ParamExchange` is latest-wins — correct for MIDI, where the newest
note state is the truth, and a dropped intermediate update is harmless. **This is
not safe for a tracker.** Each tick block contains events (note triggers,
retriggers, sample offsets) that must be consumed exactly once, in order.
Overwriting an unconsumed block silently drops a row of notes.

Replace with a small ring and a one-tick lookahead:

```
Core 0: compute tick N+1, write into ring slot, advance head
Core 1: on tick_remaining == 0, take slot at tail, advance tail,
        push ack through reverse FIFO (non-blocking — Core 1 never stalls)
Core 0: wakes on ack, computes tick N+2
```

A 2- or 4-deep ring with head/tail indices. With 20 ms of slack per slot even a
strict ping-pong is safe, but the ring makes tempo transitions and jitter
harmless.

### TickBlock contents

```
struct TickBlock {
    uint32_t samples_per_tick;      // 44100 * 2.5 / bpm — MUST be per-block
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

`samples_per_tick` **must be a field in the block, not a global**. `Fxx` tempo
changes have to take effect at the tick boundary they belong to, not at whatever
moment Core 0 happens to write a global.

Effects invisible to Core 1: `Bxx` (jump), `Dxx` (break), `EEx` (pattern delay),
`EDx` (note delay), and speed (ticks-per-row). These only change which row Core 0
reads next or how long it holds.

### Startup and underrun

Core 0 primes two blocks before Core 1 starts. If Core 1 ever finds the ring
empty at a tick boundary it renders **silence**, not stale parameters — a visible
dropout on the profiling pin is far better than a subtle timing glitch.

---

## Rendering Pipeline

### Three timing domains

At 44.1 kHz, 150 MHz, 125 BPM:

| Domain | Period | Work |
|---|---|---|
| Tick | 882 frames (20 ms) | Pattern row, all effects, envelope points, note on/off, period → increment |
| DMA buffer | 256 frames (5.8 ms) | IRQ, buffer swap, wake Core 1 |
| Sub-block | ≤64 frames (1.45 ms) | Ramp deltas, voice state load/store, increment latch |
| Sample | 1 frame (22.7 µs) | Interpolate, 2 multiplies, 2 accumulates, advance |

The three are independent: buffers do not align to ticks, and sub-blocks are cut
short wherever a tick boundary falls (13 full 64-frame blocks plus a 50-frame
remainder, at 125 BPM). **The sub-block is not a unit of output — it is a unit of
parameter constancy.** Within one, every voice's volume ramp is linear and its
increment is fixed.

Cycle budget: 150 MHz / 44100 = **3401 cycles per output frame**.

### Render loop

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

### Voice mixer

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

- **`samples_to_loop_end()` hoists the wrap test out of the per-sample path**,
  removing a compare+branch from ~30 cycles of work. Short loops just produce
  more `run` iterations. **Ping-pong (#21)** uses a direction flag
  (`TrackerVoice::backward`) and a mirrored read exactly as anticipated here:
  a per-*batch* branch picks the forward or backward inner loop and the
  matching (ceil-based, direction-mirrored) run-length helper, so a plain
  forward loop or one-shot voice pays zero added cost in the per-sample path.
  The boundary reflection itself (`wrap_ping_pong()`, mixer.h) is signed
  64-bit and only runs when a voice actually crosses a loop edge — not a
  per-sample cost either.
- **`s[idx + 1]` reads one past the end** at the boundary. The host converter
  appends a guard sample (loop-start value for looped, last value for one-shot)
  so no bounds check is needed.
- **`/n` costs ~1 divide per output frame** (~0.3%). When `n == SUBBLOCK` — the
  common case — multiply by a precomputed reciprocal and only divide on the
  ragged remainder blocks.
- Outer loop is voice, inner loop is samples. Voice state stays in registers for
  the whole run.

### Volume ramping — and what does not need it

**Volume must ramp.** A step change in amplitude is a discontinuity in the output
waveform, i.e. an audible click. XM retriggers constantly; instant 0 → full
transitions would make the whole thing crackle. Reaching the target by the end of
the sub-block gives a 1.45 ms ramp — short enough to sound instant, long enough
to be silent.

**Pitch must not ramp.** A stepped increment is a frequency discontinuity, not an
amplitude discontinuity — no click. Vibrato and portamento stepping once per tick
is what real trackers do and is part of the characteristic sound.

### Sub-block size

The trade is per-voice setup cost against control resolution. Setup is ~20–40
cycles per voice per sub-block. At 64 frames that is ~0.5 cycles/frame amortised
against a ~30-cycle inner loop — noise. At 16 frames it is ~2 cycles/frame (~7%
overhead). At 256 there is audible ramp lag on fast retriggers. **64 is the
default.**

Side effect: control resolution is now decoupled from the DMA buffer, so buffer
size becomes a pure latency-vs-IRQ-overhead choice. The tracker has no live
input, so 512 frames is viable to halve the IRQ rate.

---

## Fixed-Point Formats

**Increment: `uint32_t`, Q8.24.**
The increment is a ratio, not an index: `inc = f_note / f_mix`. XM's nominal
sample rate is 8363 Hz, so at 44.1 kHz a sample at its base note has
**inc ≈ 0.19**. Worst realistic case is ~16–32×. Eight integer bits is
bulletproof; there is no reason to spend more.

**Position: `uint32_t`, Q18.14.**
Detune error in cents ≈ 1731 / (increment in LSBs):

| Format | inc = 1.0 | inc ≈ 0.19 (typical) | 2 oct below base |
|---|---|---|---|
| Q22.10 (current engine) | 1.7 ¢ | **8.9 ¢** | 35 ¢ |
| Q19.13 | 0.21 ¢ | 1.1 ¢ | 4.3 ¢ |
| **Q18.14** | 0.11 ¢ | **0.56 ¢** | 2.2 ¢ |

XM's own finetune resolution is ~0.78 ¢, so Q18.14 stays below the format's own
error floor. The 22.10 format used by the subtractive engine is badly out of tune
for sample playback — audible beating in any unison — because the *typical* case
already spends most of the fractional resolution.

Q18.14 caps sample length at **262,144 samples** (256 KB at 8-bit). The host
converter enforces this and warns; anything longer was never going to fit
alongside 30 other instruments. If the cap proves annoying, split into
`pos_int`/`pos_frac` — one extra `ADC` in a ~30-cycle loop, ~3%.

The increment is stored Q8.24 in the TickBlock and pre-shifted to Q18.14 by
Core 1 once at latch time, so the shift is free.

---

## Panning

**Resolves open question 1 in `architecture.md`: pan goes in `VoiceNoteBase`.**

For a tracker it is not optional — XM has per-channel default panning, panning
envelopes, `8xx` and `E8x` commands, and stereo separation is central to how
tracked music sounds. The output stage must stop mono-duplicating to L+R.

The mixer takes pre-resolved `volL`/`volR` Q15 pairs rather than a
volume+position pair, so the pan law is applied on Core 0 once per tick rather
than per sample.

---

## Host Preprocessing

A host-side tool converts `.xm` into a t00t-native binary blob linked as `const`
data. **This is where most of the complexity lives**, and it follows the pattern
already used for the Fairlight sample corpus.

Responsibilities:

- Delta-decode sample data (XM stores per-sample deltas).
- Unpack pattern data (bitmask-byte compression).
- Normalise effect commands into an internal enum.
- Convert to 8-bit; align; append guard samples for the interpolator.
- Precompute period→increment tables (linear and Amiga frequency modes).
- Emit envelope points in tick units.
- Enforce the Q18.14 length cap; report peak working set.

### Size reduction, in order of effort

1. **8-bit conversion** — 2× immediately, and period-correct for the material.
2. **Trim past `loop_end`** — unreachable. XM samples are frequently padded there.
3. **Truncate to actual reach** — the simulator knows the furthest position any
   one-shot playback ever reaches. Mind `9xx` sample-offset commands: **#21
   landed `9xx` before this optimisation exists**, and a naive "furthest
   position a plain trigger reaches" simulator would truncate a sample
   shorter than some `9xx` offset the song actually uses still legitimately
   reaches (`tracker_trigger_note()`, player.h, sets `start_pos` directly
   from the offset — mid-sample, past wherever an un-offset trigger's own
   playback would have gotten to by the time it's cut). When this
   optimisation is built, the reach calculation must include every `9xx`
   param actually used against each sample, not just note-to-note pitch/
   duration.
4. **Deduplicate** — modules built from sample packs often carry byte-identical
   instruments.
5. **Per-sample decimation from known increments** — the simulator knows the exact
   set of increments the song uses for each sample. If a sample is only ever
   played at increment ≥ 1.0 (pitched up, common for high keyboard mappings),
   content above the resulting Nyquist is never audible. Resample offline to the
   minimum rate the song actually needs. Perceptually lossless, and only possible
   *because* of the dry run.

Estimate: these five cut typical modules by 50–70%, which may put most of the
corpus under the SRAM ceiling and reduce dynamic loading to an outlier feature.

### The deterministic simulator

The converter contains a full XM player that renders no audio but tracks state.
It is the source of truth for items 3 and 5 above, for the load schedule, and for
the seek manifests. **Build it in v1 even though dynamic loading is phase 2** —
it is the same code either way, and having it from day one makes the phase-2
feature mostly data-format work rather than a new subsystem.

### Testing

Render reference WAVs with `openmpt123` and diff against device output (or
against a host build of the same mixer). This is the only sane way to chase FT2's
idiosyncrasies: `E60` loop behaviour, envelope handling on note-off, portamento
with a changed instrument, arpeggio wraparound at high speeds.

**Implemented (#17).** `python3 tools/host_render/diff_xm.py` is the one-command
harness: for each module (the checked-in synthetic fixtures in
`tools/xm2t00t/xm_synth.py`, plus whatever real corpus is in `xm/`), it converts
to a blob, renders the device path via `tools/host_render/render_xm_device`
(the real `src/engines/tracker/player.h` + `mixer.h`, not a copy), renders the
reference via `openmpt123 --filter 2` (linear interpolation, matching
`mix_voice()` exactly), and diffs the two WAVs.

- **Tolerance metric**: windowed RMS-in-dB (512-frame windows) relative to the
  reference's peak, computed *after* correcting for a measured global level
  scalar (`gain_ratio`, least-squares best fit) between our engine's raw
  vol/64-of-full-scale channel-volume convention and libopenmpt's default XM
  render level — a real, ~11-20 dB, and apparently song-dependent gap
  (`gain_ratio` varies a lot across the real corpus; worth its own
  investigation later, e.g. whether libopenmpt's default XM mix level applies
  headroom keyed off channel count) that is a level-*convention* question, not
  a correctness one, so it's reported rather than silently baked into the
  engine. Threshold is **-10 dB**, set empirically off the two synthetic
  fixtures (see `diff_xm.py`'s `THRESHOLD_DB` comment for the measurements):
  the only windows that come anywhere close are the ones straddling a
  note-on/retrigger boundary — two independently-implemented declick ramps
  settling to the same target along different sample paths — never a
  sustained region. A wrong note or dropped trigger replaces the signal with
  something uncorrelated at comparable amplitude (error at or above 0 dB), so
  -10 dB stays unambiguously on the correct side of that gap.
- **Divergence localization**: `render_xm_device` emits a per-tick CSV trace
  (frame → order/pattern/row/tick + any note triggers that tick); the first
  window that exceeds tolerance is mapped back to the nearest preceding trace
  entry, so a failure reads as "order 2 (pattern 5) row 12 tick 0, channels
  3 and 7", not just a sample offset.
- **Regression corpus**: `tools/xm2t00t/xm_synth.py` builds two small,
  byte-valid `.xm` files from code (not checked-in binaries — `xm/` stays
  gitignored, real Mod Archive corpus, matching `test_xm2t00t.py`'s existing
  convention), deliberately staying inside the notes-only player's scope
  (center panning throughout — `pan.h`'s equal-power law vs whatever law FT2
  actually used is a real, permanent, out-of-scope divergence for this
  harness, not a bug). Both pass at -10 dB. The real corpus is run and
  reported too, but only informationally: any module using effects or
  envelopes (i.e. nearly all of them) diverges the moment it needs one, which
  is exactly the point — `#19`-`#22`/`#25` get a concrete, located baseline
  to work against instead of starting from nothing.

This slice also caught a real bug in `mixer.h` (#15/#16, already shipped):
`wrap_loop()` read/wrote `v->pos` while the loop that had just advanced
position was still holding the new value in a local — a wrap partway through
a multi-sample `mix_voice()` call (anything larger than one sub-block, i.e.
every real call from `tracker_render_buffer()`) discarded the advance and
wrapped from a stale position instead. Invisible to #15/#16's own tests
(`n == 1` per call in the loop-wrap test never reaches the buggy branch;
`test_full_mix()` only asserts aggregate stats), caught immediately by a
waveform-accurate reference diff. Fixed in `mixer.h`.

### xm2t00t converter (#14)

`tools/xm2t00t/` — a pure-Python, stdlib-only tool (no CMake project, unlike
`host_render/`) that turns a `.xm` module into a t00t-native binary blob. Per
this document, the device never parses XM: this runs once, offline, at build
time. This document's "Multi-format support" section (above) is why this is Python and
not C++ — it says adding MOD/S3M later is "mostly host-side Python", which
only makes sense if the XM loader already is.

```
xm2t00t.py convert <in.xm> <out_prefix>   # writes <out_prefix>_blob.h (a linkable
                                            # `static const uint8_t ..._blob_data[]`
                                            # array, same convention as samples/*.h),
                                            # prints the song-structure dump, applies
                                            # the Q18.14 + SRAM checks below
xm2t00t.py dump <in.xm>                    # prints the dump only, writes nothing
xm2t00t.py gen-header <out.h>              # (re)generate blob_format.h from
                                            # blob_format.py, the format's source of truth
```

#### Blob format

Single flat blob, fixed-size headers, every offset a byte offset from blob
start (never a pointer) — `blob_format.py` declares every struct once and is
used both to pack the blob and to generate `blob_format.h`, its C++ mirror,
so the two views of the layout can't drift apart.
`SongHeader -> order_table, PatternHeader[]+Event[] (6 bytes/cell), InstrumentHeader[]
(keymap, envelopes in tick units, vibrato) -> SampleHeader[]`, each sample
carrying a precomputed 96-entry Q8.24 note -> increment table (`periods.py`,
linear and Amiga frequency modes) so the device never runs period math. XM
effects (including the `Exy`/`Xxy`/`Fxx` commands that overload one letter for
several meanings) are normalized into named `Effect`/`VolEffect` enums
(`effects.py`) rather than left as raw nibbles for the device to switch on.
`sample_data_offset`/`sample_data_bytes` mark one uninterrupted PCM+guard-byte
region (guard = loop-start value if looped, last value if one-shot) meant for
a straight `memcpy` into SRAM — nothing else is interleaved into it.

v1 hard limits, both enforced at convert time with an actionable message and
a non-zero exit (no blob written): any single sample over the Q18.14 cap
(262,144 frames), or total sample data over an SRAM budget (`--budget-kb`,
default 380 KB — this document's stated 350-400 KB after code/stacks/DMA/mixer
scratch). No dynamic-loading simulator yet (this document: phase 2) — just a
static sum, per this document's own "v1 requires the module to fit in SRAM".

Not wired into `CMakeLists.txt` — #14 is host-only by design; a later mixer
issue links `blob_format.h`/a converted song into the tracker engine build.

**Caveat**: the Amiga-mode period table and both modes' finetune handling are
implemented from XM/FT2's public, well-documented formulas and sanity-checked
(note C-4, finetune 0 -> exactly 8363 Hz, the XM reference; one octave up
doubles the frequency) — not verified bit-exact against a real FT2 period
dump. That precision matters once something plays these increments back (the
mixer issue), not for a v1 loader.

---

## Performance Budget

Measured baseline from `history_subtractive.md`: Voice A (Fairlight 8-bit sample, linear
interpolated) = **5.9%**, i.e. ~200 cycles/frame. **32 of those is 190% of
Core 1** — this is the trap.

The 200 cycles are not sample interpolation. They are the surrounding per-sample
machinery: a float ADSR, a float-phase LFO with sine-table lookup, the
four-destination modulation chain, and `osc_sample_play()` dispatch. **A tracker
voice needs none of it per sample** — XM envelopes, vibrato, tremolo, autovibrato
and volume slides all update per tick.

Target for a stripped tracker voice: **25–40 cycles/frame** = 0.7–1.2% each, so
**32 channels ≈ 25–40% of Core 1**, leaving room for a limiter or a global
stereo effect send.

Verify with the existing profiling pin before writing any XM logic.

RP2350 accelerators worth evaluating (measure before committing):

- **M33 DSP extension** — `SMULBB`/`SMLABB`, `SMLAD` for the Q15 mix.
- **SIO interpolator blend mode** — computes `a + ((b - a) * alpha)` in hardware,
  which is exactly the lerp. Per-core, so Core 1 owns one outright with no
  save/restore. Caveat: 8-bit alpha = 256 steps. Inaudible summed across 32
  voices, but the DSP instructions may already be fast enough that setup
  overhead isn't worth it.

---

## Memory Strategy

SRAM is 520 KB total. After code, stacks, DMA buffers and mixer scratch,
realistically **350–400 KB for sample data**.

**Sample data must live in SRAM.** Do not mix directly out of XIP: the XIP cache
is 8 KB, and 32 voices reading scattered addresses at non-integer strides will
thrash it continuously *and* evict Core 0's code. Applies equally to flash and to
PSRAM over QMI CS1. Mixer inner loop gets `__not_in_flash_func`.

Baseline v1: **module must fit in SRAM.** Modules are baked into flash as `const`
data (no SD on `breadboard_rp2350`) and copied to SRAM at song load.

### Dynamic sample loading (phase 2)

Bandwidth check first: QSPI quad at ~75 MHz ≈ 35 MB/s, so a **64 KB sample loads
in ~2 ms by DMA** while a row at 125 BPM / speed 6 is 120 ms. This ratio collapses
the design — you need one or two rows of notice, not long-horizon streaming.

Because the song is deterministic, **the schedule is computed on the host, not by
Core 0 speed-playing at runtime**. Same answer, and offline it unlocks:

- **Belady's MIN eviction** — with the whole future known, evict the sample whose
  next use is furthest away. Provably optimal; no online heuristic can match it.
- **Static placement** — variable-size samples in a fixed pool is a fragmentation
  problem at runtime, but an interval-packing problem offline. Greedy first-fit
  over liveness intervals. No runtime allocator, no compaction.
- **Build-time verification** — the converter computes peak working set and
  rejects modules it cannot schedule, with a reason.

The MCU just executes a load script. Correctness requirements:

- **Liveness, not triggers.** A sample is evictable only when no channel is still
  *reading* it. Looped sustains with no note-off and long envelope releases hold
  samples past their last trigger. Easiest place to introduce rare,
  unreproducible clicks.
- **Load before evict.** Peak residency is working set *plus* the largest
  in-flight load.
- **Seeking**: precompute a required-resident-set manifest per order position.
  Jump → load set → resume. A full 400 KB reload is ~12 ms.

**What this does not fix:** peak simultaneous working set. A module with 700 KB of
instruments all live in one pattern is unplayable regardless. The scheme converts
a hard limit into a soft one. Favourable case: tracker modules have poor
count-weighted locality (the kick is everywhere) but decent *size*-weighted
locality — big samples tend to be the localised ones.

### Hardware escape hatch

RP2350 QMI CS1 supports PSRAM; boards like the Pimoroni Pico Plus 2 wire up 8 MB.
Makes single-module capacity a non-issue and loads faster. Does not remove the
SRAM working-set architecture (same random-access latency problem), and it is a
board change from bare `breadboard_rp2350` — a fork, not a drop-in.

If *flash* capacity limits how many modules ship in one firmware, ADPCM in flash
with decode-on-load gives ~4× without touching the runtime path.

---

## Display

Optional for this module and explicitly low priority. If included:

A full 240×284 16bpp framebuffer is **133 KB** — a quarter of SRAM, competing
directly with sample data. Use tile or single-line rendering into a small scratch
buffer, redrawn on row change (~5–20 Hz), not a persistent framebuffer.

Core 0 already holds playback position, so no reverse channel is needed. It runs
one tick ahead of what is audible; at 20 ms this is invisible.

---

## Feedback to the Existing Engines

From `history_subtractive.md`: 2–3% per voice without LFO, 5–6% with. **The LFO roughly doubles
voice cost** — a 0.1–20 Hz signal evaluated 44,100 times a second with a float
phase accumulator and a sine-table lookup. Three orders of magnitude of
oversampling.

Retrofit the sub-block skeleton to the subtractive engine:

- LFO value computed once per sub-block (~690 Hz, still 34× oversampled for a
  20 Hz LFO) and linearly ramped.
- ADSR level likewise.
- SVF `F_half` coefficient recomputed per sub-block and ramped, rather than per
  sample.

Must stay per-sample: the SVF two-pass state update and PolyBLEP correction —
both are per-sample by nature.

Plausibly **30–40% off the subtractive voice cost**, i.e. the difference between
16 voices at 75% and 16 voices with headroom. Proving the pattern in the tracker
engine first — where the voice loop is simple enough to get right in an afternoon
— is much lower risk than refactoring the subtractive engine directly.

---

## Settled Decisions

- [x] XM as the target format; MOD/S3M as cheap host-side additions later
- [x] 32 channels; `voice_alloc` unused; channel N = voice N
- [x] Player on Core 0, mixer on Core 1; timing still sample-accurate via
      Core 1's `tick_remaining`
- [x] Ordered TickBlock ring replaces latest-wins `ParamExchange` for this engine
- [x] `samples_per_tick` carried in the TickBlock, not a global
- [x] Sub-block rendering, 64 frames, cut at tick boundaries
- [x] Volume ramps per sub-block; pitch steps per tick (no ramp)
- [x] Increment Q8.24, position Q18.14
- [x] Pan moves into `VoiceNoteBase`; output stage becomes true stereo
- [x] Host converter owns loading, normalisation, and size reduction
- [x] v1 requires the module to fit in SRAM; dynamic loading is phase 2
- [x] **32 channels confirmed as the honest target** (#16). Measured on
      `breadboard_rp2350`, profiling pin, 256-frame DMA buffer, including one
      voice on a deliberately tight 4-sample loop to catch
      `samples_to_loop_end()`'s worst case: 8v 8.20%, 16v 15.3%, 24v 22.8%,
      32v 30.1%. Per-voice cost is flat across all four points at
      **~31.5 cycles/frame** (0.92% of Core 1), inside the 25-40 predicted
      range and comfortably under the ≤50%-of-Core-1 goal — 32 voices leaves
      ~20 points of headroom for a limiter or a global effect send. See
      `history_tracker.md`'s tracker performance table for the full numbers.
- [x] **Interpolation: linear, no nearest-neighbour build flag** (#16).
      Nearest-neighbour measured 20.1% at 32 voices vs linear's 30.1% —
      ~20.8 cycles/frame/voice vs ~31.5, a real 33% saving — and does audibly
      alias, as expected. Not adopted: linear already clears the 50% budget
      with room to spare, so there's no pressure trading it away for. The
      code (`mix_voice_nearest()` in `mixer.h`) stays in place, proven and
      ready to wire behind a flag later if a future voice-count push needs
      the headroom.
- [x] **DMA buffer size: keep 256** (#16). Idle duty cycle — the number that
      isolates fixed per-buffer/IRQ overhead — measured identical at 256 and
      512 (0.52% both), as did every voice-count point re-checked at 512.
      IRQ overhead is not measurable at this sample rate; 512 would only add
      latency (11.6ms round-trip -> 23.2ms) for no offsetting benefit, so
      256 stays the default for every engine.
- [x] **Ring depth: 2 tick slots** (#17, resolving open question 1 below).
      `player.h`'s `TickRing` picked the 2-slot side of the tradeoff: 20 ms of
      slack per tick is sufficient, and the host reference-diff harness that
      drives the ring today is single-threaded (produce, then immediately
      consume — no lookahead benefit from more slots). #18 inherits the
      constant as-is for the real cross-core case; nothing observed so far
      argues for 4.
- [x] **Ring atomics: `std::atomic<uint32_t>` head/tail, not hand-rolled ARM
      barriers** (#18). `push()`/`pop()` release-store their own index;
      `full()`/`empty()` acquire-load the other core's, the standard SPSC
      pattern — chosen over matching `ParamExchangeT`'s
      `__compiler_memory_barrier()` style specifically because `player.h`
      must stay host-buildable (`tools/host_render` links it with the host
      compiler, no pico-sdk headers allowed). The reverse multicore FIFO
      (unused by tracker's voice allocator — there isn't one) carries Core
      1's non-blocking "tick consumed" doorbell back to Core 0; Core 0's
      wake cadence for draining it comes for free from `output.cpp`'s
      existing DMA IRQ (~every 5.8ms at the default buffer size), so no new
      timer was needed.
- [x] **Ping-pong loop implementation: direction flag with a mirrored read**
      (#21, resolving open question 2 below), not host-side loop unrolling.
      Decided from the two constraints already on record rather than a fresh
      measurement of the losing option: #16 measured ~20 points of spare
      Core 1 headroom at 32 voices (8.20%/15.3%/22.8%/30.1% at 8/16/24/32v,
      ~31.5 cycles/frame/voice flat), while the module's *other* hard limit —
      350-400 KB of SRAM for sample data — has no such slack; unrolling every
      ping-pong loop region at conversion time spends from the constrained
      resource to save from the one with headroom, backwards from where the
      trade should go. `TrackerVoice::backward` (mixer.h) costs one `bool`
      per voice (32 bytes total) and one per-*batch* branch, not a per-sample
      one — see the "Render loop" notes above and `wrap_ping_pong()`'s own
      header comment for the mechanism (a signed-64-bit boundary reflection,
      resolved only when a voice actually crosses a loop edge).
      **Re-measured on real `breadboard_rp2350` hardware, profiling pin**
      (Carl, 2026-08-07), matching #16's own methodology — one voice on a
      deliberately tight loop plus a chorus of the rest, idle/8/16/24/32
      voices — except the tight voice is now ping-pong instead of forward:
      **0.7% idle, 8.19% (8v), 15.6% (16v), 23.0% (24v), 30.4% (32v)**.
      Indistinguishable from #16's forward-loop baseline (8.20%/15.3%/
      22.8%/30.1%) to within measurement noise — confirms the analytical
      prediction below: ping-pong's per-sample cost is identical to a plain
      forward loop's (same interpolate/scale/accumulate body, `pos -= inc`
      instead of `pos += inc`), and the direction-flag choice over host-side
      unrolling cost nothing measurable. Measured via
      `tools/xm2t00t/xm_synth.py`'s `voice_count_profile()` — a real song
      (not a rebuilt synthetic rig; audio_engine.cpp's #16-era phase-cycling
      code no longer exists post-#18) temporarily swapped in for
      `tracker_song_blob.h`, see that function's own docstring. Caught a
      real, separate finding along the way: the song's first draft used
      key-off to silence all channels between laps, and the duty cycle
      never dropped after the first lap even though the channels visibly
      went quiet — **key-off does not deactivate a `TrackerVoice`** in
      mixer.h, only its target volume; the mixer keeps fully interpolating
      and accumulating every ever-triggered channel at zero output forever
      unless it's a one-shot that plays through to its natural end. Not a
      #21 regression (pre-existing, unrelated to ping-pong/9xx) and out of
      scope here, but real — see Open Questions below.

---

## Open Questions

1. **Global effects** — the existing delay/reverb/overdrive could run as a stereo
   send. Not XM-spec behaviour, but the engine is retro-lo-fi by intent. Costs
   budget, but #16 confirmed the 32-voice mixer leaves ~20 points of Core 1
   headroom, so this is now affordable to revisit.
2. **Key-off doesn't free a `TrackerVoice`** — found while re-measuring #21's
   ping-pong cost on hardware (see that Settled Decisions entry). A channel
   that's key-off'd (or a whole song that just ends) leaves its voice
   `active` and fully mixed at zero output forever; only a one-shot sample
   playing through to its natural end ever clears `active`. Harmless for a
   short demo loop, but a real multi-minute song that racks up key-offs
   without retriggering those channels would burn Core 1 cycles on voices
   nobody can hear. `ECx` note-cut landed in #22 and does silence a channel
   at an exact tick (`tracker_apply_tick_note_cut()`, `TICK_NOTE_CUT` is now
   really set) -- but it only zeroes `vol64`, same as key-off's own
   near-instant no-envelope cut; it does **not** clear the mixer voice's
   `active` either, so it doesn't close this gap by itself. Fix is
   presumably having the envelope/fadeout machinery (or `ECx`'s handler)
   itself clear `active` once a channel's volume has fully decayed to 0 with
   no possibility of a sustain/loop bringing it back up. Not scoped to any
   issue yet.

---

## Build Order

Prerequisites are more interesting than the tracker itself. Do them first:

1. **Stereo output path**; pan in `VoiceNoteBase`.
2. ~~**Stripped 32-voice sample mixer**, sub-block structured, fed by a hardcoded
   test pattern. **Read the profiling pin.** This single measurement decides
   whether 32 or 16 channels is the honest target~~ — done (#15/#16). 32
   channels confirmed at ~30% of Core 1; everything after this is format
   work, not architecture work.
3. Host converter: parse XM, emit blob, dump song structure.
4. ~~TickBlock ring + `player_tick()` on Core 0. Order list, speed/BPM, note
   triggering, **no effects**.~~ — done (#18). Player *logic* (order/pattern
   walk, note triggering, `TickBlock`/`TickRing` shapes) landed early as
   `src/engines/tracker/player.h`, pulled forward by #17 so its reference-
   diff harness had a real player to drive instead of a copy. #18 added the
   real cross-core wiring: `TickRing` head/tail became genuinely atomic
   (see "Settled Decisions" above), Core 0 gained `player_task.cpp` (song
   load, resident SRAM sample table, ring priming/refill, play/stop/seek
   transport driven by MIDI Start/Continue/Stop/Program Change), and Core 1's
   `audio_engine.cpp` replaced the #15/#16 synthetic profiling rig with a
   real ring-consuming render loop that renders silence on ring-empty. A
   small synthetic demo song (`tracker_song_blob.h`, from
   `xm_synth.notes_basic()`) auto-plays on boot; swapping in a real module
   from `xm/` for a listening test is a one-command manual regenerate. It
   already sounds like music here.
5. ~~The effects covering ~90% of real usage: `0` arpeggio, `1`/`2` porta, `3` tone
   porta, `4` vibrato, `A` volume slide, `C` set volume, `B` jump, `D` pattern
   break, `F` speed/tempo.~~ — done (#19). All ten land in
   `player.h`'s `player_produce_tick()` as per-channel state machines, tick
   0 vs later-tick semantics enforced by only ever calling
   `tracker_apply_pitch_vol_effect()` when `!row_boundary` (arpeggio's own
   tick-0 contribution is 0 either way, so it's included in that same gate
   rather than special-cased). Pitch effects needed real period math on
   Core 0 for the first time — `tracker_note_to_period()`/
   `tracker_period_to_inc()` are a runtime C++ port of
   `tools/xm2t00t/periods.py`'s linear/Amiga formulas (double-precision, not
   fixed-point: 32 channels of `pow()` at ~50 Hz is nowhere near Core 0's
   budget) — but a channel with no active pitch effect still latches
   straight from the precomputed `note_increments` table, byte-identical to
   pre-#19 output. `B`/`D` share one row-level pending-jump/pending-break
   pair in `PlayerState`, resolved at the row's last tick so "B and D on the
   same row" (different channels) composes naturally: jump's order, break's
   row.
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
     getting this bit-exact against libopenmpt is explicitly the "long tail
     of FT2 quirks" (step 7 below), not this step's bar.
6. ~~Instruments, envelopes, key-off (note 97), the volume column, ping-pong
   loops.~~ — done (#20), except ping-pong loops (split out to #21 alongside
   9xx sample offset, tracked separately; see step 7). Multi-sample note→sample mapping
   and per-note relative-note/finetune were already in place from #17-#19
   (needed for pitch even before envelopes existed); #20's actual new
   surface is `player.h`'s `tracker_resolve_envelope_volpan()` (volume/
   panning envelopes with sustain and loop, run every tick independent of
   any pattern effect), `tracker_fadeout_tick()`, `tracker_autovibrato_delta()`,
   and the volume column's remaining bands (fine/coarse volslide, panslide,
   vol-column vibrato sharing the effect column's oscillator, vol-column
   tone porta with its own coarse rate table). The host-side blob format
   (`InstrumentHeader`, `EnvelopePoint`, envelope/autovibrato/fadeout
   fields) and the XM parser/writer were already complete from #14 — this
   step was entirely device-side (`player.h`) plus test fixtures.
   - **Key-off does not cut a voice directly.** It only sets a per-channel
     `key_off` flag that the envelope/fadeout machinery consumes every
     tick from then on. Verified against `openmpt123` (not just FT2's own
     replayer source, which turned out to disagree with it on one point —
     see below): an instrument with an *enabled* volume envelope releases
     through it (continuing past its sustain point, plus fadeout once the
     envelope's own last point is reached); an instrument with **no**
     volume envelope at all cuts almost instantly on key-off, regardless
     of its fadeout field. FT2's own replayer source (ft2-clone, a
     byte-accurate port) applies fadeout unconditionally, envelope or not
     — an earlier version of this implementation followed that literally,
     and diverged badly against `openmpt123` (`fadeout_basic`'s fixture);
     `openmpt123` is this harness's oracle, so the device player follows
     it, not the DOS original, on this one point.
   - **Envelope evaluation is a fresh interpolation each tick**, not FT2's
     own incremental Q8.8 delta-accumulation (which exists on 1990s
     hardware to avoid a per-tick division Core 0 has three million spare
     cycles for) — behaviourally equivalent for well-formed envelopes,
     which is the overwhelming majority of real content.
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
     fadeout, and the volume column's level/pan bands all diff clean. Vol-
     column vibrato and tone portamento are continuous pitch
     oscillators/glides, same category as the effect-column vibrato (#19:
     "not chased to bit-exactness... any small rate mismatch is permanent
     phase drift") — their *mechanism* (no retrigger, oscillates/glides,
     shared state) is instead pinned down with C++ unit tests in
     `render_xm_device.cpp`, matching that precedent rather than fighting
     it. Autovibrato is the same story and is covered the same way (a
     dedicated sweep/freeze/no-op unit test; not asserted in the audio
     diff harness).
7. ~~Ping-pong loops, `9xx` sample offset.~~ — done (#21). `mixer.h` gains a
   `TrackerVoice::backward` direction flag, a ceil-based `samples_to_loop_start()`
   mirroring the existing `samples_to_loop_end()`, and `wrap_ping_pong()`
   (signed 64-bit, resolved only at an actual boundary crossing — see the
   "Render loop" notes above and the Settled Decisions entry above for the
   direction-flag-vs-host-unroll tradeoff). `player.h`'s `tracker_trigger_note()`
   gets a new `Effect::SAMPLE_OFFSET` branch: memory (`sample_offset_memory`)
   is only written on a row that both carries `9xx` *and* actually triggers a
   note, and an offset at or past the target sample's length suppresses the
   whole trigger — both verified against `openmpt123`, not assumed from FT2's
   own documentation, which turned out to already be the harness's working
   convention (see #20's key-off entry above for the same kind of
   FT2-vs-`openmpt123` gap).
   - **Guard-sample correctness at every loop type turned out to need no
     code change.** The existing guard byte (`blob_writer.py`'s
     `_guard_byte()`: loop-start value for any looped sample, last value for
     one-shot) is only ever read at `s[idx+1]` when `idx+1 == num_samples`
     (the loop reaches the sample's physical end) — true regardless of loop
     *type*, since ping-pong's reflection happens in position space
     (`wrap_ping_pong()`), not by reading the buffer differently near an
     edge. Verified by tracing through, not just assumed; see `mixer.h`'s
     `TrackerSample` comment.
   - **The direction-flag reflection is not the textbook `2*boundary - pos`.**
     A *zero-overshoot* landing exactly on `loop_end_pos` (the increment
     divides the loop length exactly from a whole-sample start — rare, but
     the mixer must not hang on rare input) reflects to itself under the
     textbook formula, since `loop_end_pos` is an exclusive bound no read
     may land on. `wrap_ping_pong()` reflects around `loop_end_pos - 1` /
     `loop_start_pos + 1` instead, trading a 2-part-in-16384-of-a-sample
     inaudible bias on every bounce for guaranteed termination. Caught by
     `tools/host_render/render_tracker_mixer.cpp`'s
     `test_pingpong_exact_boundary()` — the harness hung indefinitely before
     this fix, not just produced a wrong answer, which is why that test
     exists as a permanent regression check rather than a one-off.
   - **The final, boundary-crossing step of a backward run is recomputed
     with signed 64-bit arithmetic from the batch's entry position, not
     trusted from the `uint32_t` the per-sample loop just advanced.** A
     ping-pong reflection routinely needs to represent a position before the
     loop start (or, symmetrically, past the loop end by more than one
     boundary width for a loop shorter than one increment), which a Q18.14
     `uint32_t` position cannot hold — unlike a plain forward loop's
     overshoot, which is always unsigned-safe because addition never wraps
     low. Resolved once per boundary crossing (`wrap_ping_pong()`), not a
     per-sample cost either way.
   - **`9xx` past the sample's end suppresses the *entire* trigger**, not
     just the offset (clamped to 0, or to the end) — matching `openmpt123`'s
     documented FT2-compatible behaviour ("notes with offset commands beyond
     the sample length are never triggered"). The instrument column still
     latches (matching every other "nothing to play" branch in
     `tracker_trigger_note()`, e.g. an unmapped sample-map entry); nothing
     else about the channel's state changes.
   - **Verified against `openmpt123`**: two new fixtures in
     `tools/xm2t00t/xm_synth.py` (`ping_pong_basic`, a tight ping-pong loop
     held long enough to bounce many times within one row;
     `sample_offset_basic`, a `9xx` trigger deep into a long two-toned
     sample plus a memory-reuse row plus a past-the-end suppressed trigger
     on a second, short instrument) both diff clean, asserted the same as
     every #17-#20 fixture. `render_xm_device.cpp` adds a C++ unit test for
     `9xx`'s exact mechanics (start_pos scaling, the next-to-a-note memory
     rule, the suppression bounds check) that the audio diff can't cheaply
     pin down, matching the #19/#20 precedent of pairing an audio fixture
     with a targeted unit test rather than one or the other.
   - **Re-measured on real hardware** (Carl, 2026-08-07): 0.7/8.19/15.6/
     23.0/30.4% idle/8v/16v/24v/32v with a ping-pong voice in the mix,
     indistinguishable from #16's forward-loop baseline. See the Settled
     Decisions entry above for the numbers and for the key-off/voice-
     lifecycle finding the measurement run turned up along the way (Open
     Questions item 2).
8. ~~Remaining `Exy` sub-commands.~~ — done and closed (#22, split from the
   original "FT2 quirk tail" issue 2026-08-08: the bounded/mechanical
   sub-commands below landed and closed here, the four named quirks and
   open-ended corpus-chasing were split out to #25, see step 9). `E1x`/`E2x`
   (fine porta up/down) and
   `EAx`/`EBx` (fine volume slide up/down) apply once, at tick 0 only, own
   memory slots separate from the continuous `1xx`/`2xx`/`Axy` commands.
   `E9x`/`Rxy` (retrigger) landed as the *general* Rxy form — full
   volume-change table, not just E9x's plain fixed-interval case — since
   both effect-column letters decode to the same `Effect::RETRIG_NOTE`
   enum value and E9x's decode already reaches that shared code with the
   volume-change nibble zeroed (effects.py strips it), so handling Rxy
   properly was no extra work, not scope creep. `ECx`/`EDx`/`EEx` (note
   cut, note delay, pattern delay) are invisible to Core 1 and only change
   what Core 0 reads or how long it holds a row/tick, same category as
   `Bxx`/`Dxx`'s existing row-level pending-effect handling: `EDx` defers a
   row's *entire* tick-0 processing (trigger, volume column, everything —
   it occupies the whole effect column slot, so nothing else can coexist
   with it on that cell anyway) to the tick within the row its param
   names; `EEx` holds the current row for `param` additional full-speed
   passes, with the held repeats' own tick 0s falling through to normal
   *continuation* handling (not a re-trigger) via one adjusted
   `row_boundary` computation (`tick_in_row == 0 && !pattern_delay_holding`)
   that every other per-channel dispatch already keyed off, rather than
   needing pattern-delay-specific branches sprinkled through
   `player_produce_tick()`.
   - **`E3x` (glissando control) was tried and reverted.** The mechanism
     (a persistent per-channel flag) was trivial, but snapping tone
     portamento's audible pitch to the nearest semitone each tick is not
     spec-clear enough to be bounded: two reasonable implementations
     (snapping a local copy for output only, vs. snapping the persisting
     glide state itself) both diverged from `openmpt123` within 1-2 ticks
     of the glide starting. Genuinely diff-driven quirk work, not the
     mechanical case the rest of this step turned out to be — moved to
     step 9's deferred list instead of force-fit or left half-working.
   - **Verified against `openmpt123`**: one new fixture per command
     (`fine_slides_basic`, `retrig_basic`, `note_cut_basic`,
     `note_delay_basic`, `pattern_delay_basic`) in `xm_synth.py`, asserted
     in `diff_xm.py` alongside every earlier fixture — all pass. Exact
     tick-scheduling behaviour the audio diff can't cheaply pin down (the
     precise delay/cut/retrigger tick, the exact row-hold length) gets a
     targeted C++ unit test in `render_xm_device.cpp` per command, matching
     the #19-#21 precedent — all pass.
9. Long tail of FT2 quirks, chased against `openmpt123` reference renders --
   tracked in **#25** (split out of #22, 2026-08-08, so #22 could close on
   its own done scope), **deferred, not scheduled**: `E60` pattern loop
   (including nested/interacting cases), `E3x` glissando control, envelope
   handling on note-off, portamento with a changed instrument, arpeggio
   wraparound at high speeds, and the open-ended corpus-driven work beyond
   those (run the regression set, find the first divergence, fix it, add the
   module to the set, repeat until a stated corpus of >=10 real modules
   passes). No natural completion point, so picking this back up is a
   deliberate decision, not automatic follow-on from anything else. #25
   blocks step 10 (#23) as a result.
10. Retrofit sub-block rendering to the subtractive engine.
11. (Phase 2) Deterministic simulator → load schedule → dynamic sample
    loading -- blocked by step 9 (#23 needs settled player semantics first).
