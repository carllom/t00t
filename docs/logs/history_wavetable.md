# T00T — Wavetable/Granular Module Development Log

### Skeleton + Measurement Rig

`module_wavetable.md`'s design (a bluesky proposal weighing a PPG/Wavestation/
granular module against splitting or refocusing the subtractive engine)
implemented as a minimal, buildable starting point: `src/osc/wavetable.h`
(the shared Q0.32 phase × wave-position bilinear read primitive) and
`src/engines/wavetable/` (`make ENGINE=wavetable`) — one wavetable partial
per voice, ADSR envelope, the existing delay/reverb/phaser/flanger/chorus/
bitcrusher/overdrive FX chain reused unmodified, no filter, no LFO, no
partial pool yet. Two procedurally-generated wavetables ship
(`tables.h`, band-limited additive morphs — no authored/ROM wave data exists
in this repo), selected by Program Change; CC1 drives a live wave-position
scan in place of the vibrato depth every other engine's mod wheel controls,
since this skeleton has no LFO to attach vibrato to.

The measurement rig (`rig.h`) followed the same shape as `fm`'s and `chip`'s
own rigs: a stripped N-partial mixer with no envelope/modulation/MIDI,
compile-time levers (`WT_RIG_PARTIALS`, `WT_RIG_BLOCK`, `WT_RIG_MODE` —
nearest / bilinear / bilinear+window), and a device-side self-cycling build
behind `WT_PROFILE=1` that sweeps partial counts on `PROFILE_PIN` the same
way the chip module's own rig does. `tools/host_render/render_wavetable_rig.cpp`
checks it for correctness (real, finite, non-clipping output) before any
hardware pass — confirmed passing for all three `WT_RIG_MODE` values and a
range of partial counts (8, 48) on the host build.

Both `make ENGINE=wavetable` (the real engine) and
`make ENGINE=wavetable WT_PROFILE=1` (the rig, including
`WT_RIG_PARTIALS=64 WT_RIG_MODE=2`) build clean against the real
`arm-none-eabi-gcc` cross-toolchain, zero warnings, and link a `t00t.uf2` --
confirmed `subtractive` and `chip CHIP_PROFILE=1` still build unaffected by
the CMakeLists.txt/Makefile changes.

### Hardware Voice-Count Sweep (default build: bilinear, block=16)

First hardware pass, `PROFILE_PIN` duty cycle on `breadboard_rp2350` at
44.1 kHz/150 MHz, default rig build (`make ENGINE=wavetable WT_PROFILE=1`,
i.e. `WT_RIG_MODE=1` bilinear, `WT_RIG_BLOCK=16`, `WT_RIG_PARTIALS=48`),
sweeping the built-in phase table (0, 1, 4, 8, 16, 32, 48 partials):

| Partials | Duty (%) | c/f (duty% × 3401) |
|---|---|---|
| 0  | 0.58  | 19.7 |
| 1  | 1.73  | 58.8 |
| 4  | 5.07  | 172.4 |
| 8  | 9.52  | 323.8 |
| 16 | 18.41 | 626.1 |
| 32 | 36.20 | 1231.2 |
| 48 | 53.56 | 1821.6 |

Striking linearity: every pairwise slope across the sweep lands in
37.8-39.1 c/f/partial, and a least-squares fit across all seven points gives

```
c/f = 22.4 (fixed per-buffer overhead) + 37.6 x partials
```

**~37.6 c/f per bilinear-read wavetable partial** — cheaper than
module_wavetable.md's ~45 c/f planning estimate (that estimate assumed a
similar tap count but hadn't accounted for how well the shared read
primitive's four-tap/three-lerp shape compiles). The ~22 c/f fixed cost
(buffer clear, GPIO toggle, FIFO push) matches the same small intercept
`history_chip.md`'s and `history_fm.md`'s own rigs found.

At the same ≤50%-of-Core-1 target `module_tracker.md`/`module_fm.md` used
(1700.5 c/f), minus the ~22 c/f fixed cost: **~44-45 bilinear-read
partials**, unfiltered, no reverb. With a global reverb send reserved
(~272 c/f, `engine.md`'s Effects section): **~37 partials**.

This measures only `WT_RIG_MODE=1` (bilinear phase x wave-position, no
window) at `WT_RIG_BLOCK=16` — module_wavetable.md's other two levers
(`WT_RIG_MODE=0` nearest, `=2` bilinear+window; other `WT_RIG_BLOCK` values)
are still unmeasured. Nearest should undercut this per the original
estimate; the window mode's extra masked table read should cost roughly one
more `WT_TABLE_SIZE`-sized lookup's worth per sample, in the same ballpark
as this measurement's own per-tap cost.

### Hardware Voice-Count Sweep — Nearest and Bilinear+Window

Second and third passes, same rig/hardware/method as above, covering the
two remaining `WT_RIG_MODE` values:

| Partials | Nearest (mode 0) duty% / c/f | Bilinear+window (mode 2) duty% / c/f |
|---|---|---|
| 0  | 0.55 / 18.7  | 0.58 / 19.7 |
| 1  | 1.12 / 38.1  | 1.90 / 64.6 |
| 4  | 2.42 / 82.3  | 5.71 / 194.2 |
| 8  | 4.15 / 141.1 | 10.80 / 367.3 |
| 16 | 7.62 / 259.2 | 20.97 / 713.2 |
| 32 | 14.56 / 495.2| 41.32 / 1405.3 |
| 48 | 21.50 / 731.2| 61.28 / 2084.1 |

Both sweeps are as linear as the bilinear one above (pairwise slopes within
1-2% of each other within each mode). Least-squares fits:

```
Nearest:          c/f = 22.1 + 14.8 x partials
Bilinear+window:  c/f = 22.4 + 43.0 x partials
```

Fixed overhead lands at 18.7-22.4 c/f across all three modes — confirms
it's genuine per-buffer cost (buffer clear, GPIO toggle, FIFO push), not
something that varies with the read kernel.

Summary across all three `WT_RIG_MODE` values, all at `WT_RIG_BLOCK=16`:

| Mode | c/f/partial | Partials @ 50% budget | @ 50% minus reverb |
|---|---|---|---|
| 0: nearest | ~14.8 | ~113 | ~95 |
| 1: bilinear | ~37.6 | ~44-45 | ~37 |
| 2: bilinear+window | ~43.0 | ~39 | ~33 |

The window read itself costs only **~5.4 c/f/partial** on top of plain
bilinear (43.0 - 37.6) — one extra masked table read plus a multiply, in
line with module_wavetable.md's "window is a second masked table read"
reasoning. Nearest mode is markedly cheaper than the tracker's own
interpolated-fetch anchor (~31.4 c/f, module_tracker.md) — no loop-wrap
check and no run-batching overhead, on a power-of-two-masked table with no
loop concept at all.

This closes module_wavetable.md's own measurement gate ("sweeping partial
count and interpolation mode (nearest / bilinear / +window)") — all three
kernel variants now have real hardware numbers. `WT_RIG_BLOCK` alternatives
(8/32 vs. the default 16) remain unmeasured, but are expected to be a minor
effect next to these per-kernel differences, per the tracker/FM modules'
own findings for their equivalent sub-block-size levers.

**Still not decided from this data alone**: `MAX_PARTIALS` for the real
engine, since that also depends on whether/how a partial pool (Two-Level
Allocation) and a voice-level filter get built, both still future work. The
skeleton's current `MAX_VOICES=16` (one partial per voice, no pool) sits
comfortably inside even the most expensive measured kernel's unfiltered
ceiling (~33 partials, bilinear+window with reverb reserved), with room to
grow once a partial pool exists to make use of the headroom.

### Real PPG Wave Data + Per-Voice Filter

Following explicit direction to use the real PPG factory wave/wavetable
data already recovered under `tools/ppg/` (see that directory's own
README) and to take PPG Wave 2.2/2.3's hardware architecture as design
guidance rather than a spec to follow literally.

Fetched a secondary architecture summary
([ppg.synth.net/wave22](https://ppg.synth.net/wave22/)) and cross-checked
it against this repo's own byte-exact ROM extraction. Where they disagreed
on wave/table structure (128 vs. 64 samples/waveform, "well over 2000"
waves vs. 244, 32 banks vs. 29 tables), trusted the extraction — it had
already been confirmed byte-for-byte against independent community
research, the secondary source hadn't. Kept the secondary source only for
architectural color the ROM dump can't answer: 8-voice polyphony, 2
oscillators/voice, one SSM2044 (4-pole/24 dB) filter+VCA per voice (not
shared), and the wave-position modulation source list (EG/velocity/mod
wheel/aftertouch). Recorded as module_wavetable.md's new "PPG Architecture
Reference" section.

**Converter**: `tools/ppg/convert_ppg_waves.py` reads
`extract/w23_waves.bin` (244 x 64-byte waveforms) and
`extract/w23_wavetables.json` (29 tables' sparse authored keyframe lists,
confirmed every table starts at slot 0 and ends at slot 0x3C) and emits
`src/engines/wavetable/ppg_waves.h` — waveforms converted to signed 16-bit
(`(byte-128)<<8`), and each table's keyframe list pre-expanded to a full
64-entry `WaveTableIndexEntry` array (linear interpolation between
authored slots, held flat past the last one) at *conversion* time, not on
device. Output is gitignored (added to `.gitignore` alongside the FM
module's `patches.h`) since it's derived from third-party ROM content.
Wavetable 13's two out-of-range waveform references (244/245, a known
EVU-expansion-board quirk per `tools/ppg/README.md`) are clamped to the
last real waveform (243) with a printed warning, rather than silently
reading out of bounds or dropping the table.

**Bug caught by the host correctness tool, not by inspection**: the first
version of `osc/wavetable.h` had `WT_TABLE_SIZE = 128` (an arbitrary
pre-real-data guess), but real PPG waveforms are 64 samples. Wiring the
converted data straight into a new host tool
(`tools/host_render/render_ppg_waves.cpp` — range-checks every generated
table's wave indices, then renders a few real tables' wave-position sweeps
to WAV) segfaulted immediately: the read kernels compute each waveform's
stride as `wave * WT_TABLE_SIZE`, so at the wrong table size every read
past waveform 0 landed outside the 244-waveform array. Fixed by changing
`WT_TABLE_BITS` from 7 to 6 (64 samples), which needed no other code
change — the read kernels' instruction count doesn't depend on table size,
only a mask/shift constant does, so the existing hardware-measured
per-partial costs (37.6/14.8/43.0 c/f) still apply unchanged. This is
exactly the kind of error the "verify the conversion before wiring it into
a real engine" step exists to catch before it reaches hardware, not after.

**Wave sharing across tables required no engine change.** Real PPG tables
reuse the same underlying waveform across multiple wavetables (e.g.
waveform 101 appears in several), which doesn't fit `tables.h`'s original
per-table-own-keyframe-buffer shape. Turned out unnecessary to change:
`osc/wavetable.h`'s read functions already just index
`keyframes[wave * WT_TABLE_SIZE + i]`, so pointing every real-PPG
`WaveTable.keyframes` at one shared 244-waveform pool (`ppg_wave_data`)
and letting each table's index entries hold absolute pool indices (exactly
what the ROM format already encodes) worked with the primitive completely
unmodified — confirms the earlier design choice (module_wavetable.md's
Decision Record entry 2/7) generalized further than originally scoped for.

**Per-voice filter**: two `src/filter.h` `SVFilter` instances per voice,
ticked in series, approximating the SSM2044's 4-pole slope from two
2-pole passes -- deliberately basic, not a ladder model. Fixed cutoff/
resonance constants; the existing ADSR also drives cutoff, same
"one envelope, two destinations" shape subtractive's own filter uses.
Filter state resets on retrigger. This directly reverses
module_wavetable.md's original Decision Record entry 6 (a chip-style
shared `FilterBus`) once the real hardware architecture (filter+VCA per
voice, not shared) and the voice-count math both pointed the same
direction — see entry 12.

**`MAX_VOICES` reduced from 16 to 8**, matching real PPG polyphony
exactly. The measured *bare partial* costs alone would support far more
voices, but the filtered per-voice chassis (partial + envelope + two SVF
passes) has no hardware measurement yet. Estimated budget math: 8 voices
lands around ~39% of Core 1 (comfortable margin even if the filter
estimate is low); 16 voices would land around ~79% (no margin, and
unverified). Chose the number with headroom rather than ship an unmeasured
combination at the higher voice count — see module_wavetable.md's Decision
Record entry 13.

**Verified**: `render_ppg_waves` and `render_wavetable_rig` both pass on
the host build after the table-size fix; the full host_render suite still
builds clean. `make ENGINE=wavetable` (real engine, real PPG data, filter
included) and `make ENGINE=wavetable WT_PROFILE=1` (rig) both build clean
against the real `arm-none-eabi-gcc` toolchain, zero warnings, `t00t.uf2`
produced (91.8 KB flash / 203.5 KB SRAM `.bss` at `MAX_VOICES=8`, well
inside both the flash and SRAM budgets). Confirmed `subtractive` and `fm`
still build unaffected.

**Not yet done**: no hardware measurement of the combined filtered
per-voice chassis (module_wavetable.md's top Future/TODO item) -- the
8-voice choice above is a conservative estimate, not a confirmed number.
