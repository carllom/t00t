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
  more `run` iterations. Ping-pong needs a direction flag and a mirrored read.
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

## Performance Budget

Measured baseline from `engine.md`: Voice A (Fairlight 8-bit sample, linear
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
   one-shot playback ever reaches. Mind `9xx` sample-offset commands.
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

From `engine.md`: 2–3% per voice without LFO, 5–6% with. **The LFO roughly doubles
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
      `engine.md`'s tracker performance table for the full numbers.
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

---

## Open Questions

1. **Ring depth** — 2 or 4 tick slots? 2 is sufficient given 20 ms of slack;
   4 costs little and is more forgiving of tempo extremes.
2. **Global effects** — the existing delay/reverb/overdrive could run as a stereo
   send. Not XM-spec behaviour, but the engine is retro-lo-fi by intent. Costs
   budget, but #16 confirmed the 32-voice mixer leaves ~20 points of Core 1
   headroom, so this is now affordable to revisit.
3. **Ping-pong loop implementation** — direction flag with mirrored read inside
   `samples_to_loop_end()`, or unroll the loop region at conversion time and pay
   the memory?

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
4. TickBlock ring + `player_tick()` on Core 0. Order list, speed/BPM, note
   triggering, **no effects**. It already sounds like music here.
5. The effects covering ~90% of real usage: `0` arpeggio, `1`/`2` porta, `3` tone
   porta, `4` vibrato, `A` volume slide, `C` set volume, `B` jump, `D` pattern
   break, `F` speed/tempo.
6. Instruments, envelopes, key-off (note 97), the volume column, ping-pong loops.
7. Long tail of FT2 quirks, chased against `openmpt123` reference renders.
8. Retrofit sub-block rendering to the subtractive engine.
9. (Phase 2) Deterministic simulator → load schedule → dynamic sample loading.
