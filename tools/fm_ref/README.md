# FM reference rig (F0)

Ground-truth comparison harness for the FM module rewrite. See history_fm.md
§3 for why it exists and §4 for how it gates the phases.

The short version: attempt 1 was validated by ear on hardware, which cannot tell
you *which* of a dozen chained nonlinear curves is wrong. This rig replaces that
loop with a numeric one — render the same note through Dexed and through t00t,
score the difference, act on the number.

## Setup

```bash
cd tools/fm_ref
./fetch_dexed.sh            # Dexed's msfa synthesis core, pinned commit -> msfa/
./fetch_banks.sh            # DX7 factory ROM sysex banks -> banks/
make                        # -> ./dexed_render

python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# t00t side: generate the patch bank, then build the matching renderer
cd ../..
python3 tools/syx2patch.py convert tools/fm_ref/banks/rom1a.syx src/engines/fm/patches.h
make -C tools/host_render -f Makefile.fm
```

`msfa/`, `banks/`, `.venv/` and `out/` are all gitignored — nothing third-party
is vendored into this repo, and both fetch scripts pin what they download.
Neither the Dexed core nor anything else here is ever linked into the device
firmware; this is host-only tooling.

## Use

Both renderers take the identical CLI, deliberately, so the compare script is a
thin diff rather than a translation layer:

```bash
./dexed_render                      --syx banks/rom1a.syx --voice 10 --note 48 --gate 2 --out out/ref.wav
../host_render/render_fm_patch      --syx banks/rom1a.syx --voice 10 --note 48 --gate 2 --out out/test.wav

.venv/bin/python ../fm_compare.py out/ref.wav out/test.wav --note 48 --gate 2
```

Whole-bank sweep, which is what the phase gates actually use:

```bash
.venv/bin/python ../fm_compare.py --bank banks/rom1a.syx --voices 0-31 \
    --note 48 --gate 2 --quiet --json out/scorecard.json
```

### Control-plane conformance (F1)

No audio, no spectra — just numbers, compared exactly:

```bash
make dexed_dump
make -C ../host_render -f Makefile.fm t00t_ctl_dump
.venv/bin/python ../fm_ctl_diff.py             # all tests
.venv/bin/python ../fm_ctl_diff.py --only eg --verbose
```

Operator frequency has its own suite, because it cannot be a table diff:

```bash
.venv/bin/python make_freq_bank.py       # once, or after changing the sweeps
.venv/bin/python ../fm_freq_diff.py
```

Every table test defines a shared domain both engines are converted into — see
`dexed_dump.cpp`'s header comment, which is the authority on those definitions.
Change one and you must change both sides together.

Add `--pcm16 PATH` to either renderer for a peak-normalised listenable file.
Never analyse those — the per-file normalisation destroys the level information
`fm_compare.py` reports as its own metric.

## What the metrics mean

| Metric | Reads as |
|---|---|
| `level gap` | absolute loudness difference. Reported alone; never folded into the shape metrics. |
| `harmonic MAE` / `p95` | per-harmonic spectral error over frames where **both** engines are sounding. `coactive_frac` says how much of the reference that covered — a low value is itself an envelope finding, and makes the spectral number correspondingly less meaningful. |
| `attack timbre MAE` | same, restricted to the first 100 ms, where essentially every patch is still sounding on both sides. The timbre number that stays valid when `coactive_frac` is low. |
| `spectral centroid` | brightness, in harmonic number. `x1.0` is a match; `<1` reads as "too thin/dull", `>1` as "too bright/buzzy". |
| `envelope MAE` + attack/sustain/release features | the timing axis — "delayed envelope", "weak sustain". |

## Files

| | |
|---|---|
| `fetch_dexed.sh` | Dexed msfa core at a pinned SHA. Bump deliberately: it moves every baseline number. |
| `fetch_banks.sh` | the eight DX7 factory ROM banks. |
| `shim/tuning.h` | standard-12-TET `TuningState`, replacing the JUCE/Surge microtuning one. |
| `shim/libMTSClient.h` | no-op MTS-ESP client, so the microtuning branch is never taken. |
| `dexed_render.cpp` | the reference renderer. |
| `wav32.h` | float32 (analysis) and normalised PCM16 (listening) WAV writers, shared with the t00t side. |
| `make_freq_bank.py` | synthetic banks sweeping coarse/fine/detune/fixed-frequency, for F5's frequency test. |
| `sine_table_ab.py` | the history_fm.md §3.2 interpolation check — what the non-interpolated table costs across a modulation-index sweep. |
| `dexed_dump.cpp` | control-plane trajectories as CSV (F1): EG, pitch EG, LFO, the integer tables, the algorithm table. |
| `../fm_compare.py` | the signal-plane scorecard. |
| `../fm_ctl_diff.py` | the control-plane conformance suite (F1). |
| `../fm_freq_diff.py` | operator frequency conformance in cents (F5) -- the one test that must render, since Dexed's `osc_freq()` is private. |
| `../host_render/t00t_ctl_dump.cpp` | the t00t side of the control-plane dump. |
| `../host_render/render_fm_patch.cpp` | the t00t side, same CLI. |
