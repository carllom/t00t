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
