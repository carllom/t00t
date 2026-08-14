#!/usr/bin/env python3
"""Reference-diff harness driver (#17): the one-command entry point module_tracker.md's
"Testing" section and issue #17 ask for.

    python3 tools/host_render/diff_xm.py

Pipeline per module, both for the checked-in synthetic fixtures
(tools/xm2t00t/xm_synth.py) and any real corpus found in `xm/` (gitignored,
same convention as tools/xm2t00t/test_xm2t00t.py):

    .xm --[xm2t00t.py convert --raw-blob]--> blob
    blob --[render_xm_device render]--> device WAV + per-tick trace CSV
    .xm --[openmpt123 --render, --filter 2 to match our linear interpolation]--> reference WAV
    (device WAV, reference WAV) --[windowed RMS in dB, see diff_wavs()]--> report

Tolerance policy (see diff_wavs() for the numbers): a fixed, justified
windowed-RMS-in-dB threshold, chosen to sit well above the noise floor our own
lossy 8-bit conversion guarantees (module_tracker.md: "sample-exact equality with
openmpt123 is not achievable") but well below the level a genuinely wrong note
or a timing slip produces -- applied AFTER correcting for a measured, global,
roughly-constant ~11-12 dB level gap this harness turned up between our
engine's channel-volume convention (linear vol/64 of full Q15 scale, "no
headroom") and libopenmpt's default XM render level (almost certainly its own
internal mix-level/headroom compatibility handling, not part of the raw XM
spec). That gap is real and worth its own investigation (reported via
`gain_ratio` below), but it is a level-*convention* question, not a
correctness one -- baking a guessed correction factor into the engine here,
under a diff-harness issue, would be worse than reporting it and moving on.
Folding an unexamined constant gain mismatch into the pass/fail metric would
also just make the tolerance useless (everything either passes trivially
because the threshold had to be opened wide enough to swallow it, or fails
on every fixture for the same non-bug reason).

The synthetic fixtures are built to stay entirely inside src/engines/tracker/
player.h's current (notes-only, no effects, no envelopes) scope, so they are
asserted against that tolerance -- a regression there is a real bug. Real
corpus modules almost certainly use effects/envelopes player.h doesn't
implement yet (#19-22); those are run and reported for visibility (this is
the "turns every subsequent correctness question into a diff" tooling
module_tracker.md asks for) but never assert pass/fail here.
"""

from __future__ import annotations

import glob
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from dataclasses import dataclass
from typing import List, Optional, Tuple

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
XM2T00T_DIR = os.path.join(REPO_ROOT, "tools", "xm2t00t")
BUILD_DIR = os.path.join(HERE, "build")
RENDER_BIN = os.path.join(BUILD_DIR, "render_xm_device")
CORPUS_DIR = os.path.join(REPO_ROOT, "xm")

sys.path.insert(0, XM2T00T_DIR)

# Windowed RMS window, in frames. 512 frames (~11.6ms at 44.1kHz) is short
# enough to localize a divergence to roughly one tick (a tick is >= a few
# hundred frames at any sane BPM) but long enough to average out the few-
# sample ripple a note-trigger transient leaves even when both renderers
# agree on everything (our sub-block volume ramp and libopenmpt's own
# internal ramping settle on different sample-by-sample paths to the same
# target).
WINDOW_FRAMES = 512

# Tolerance, in dB relative to the reference track's global peak, applied
# AFTER the gain_ratio correction in diff_wavs(). Justification (module_tracker.md:
# "choosing [a tolerance] is part of this slice") -- empirically measured,
# not just theoretical:
#
# Once interpolation filter (--filter 2) and overall level (gain_ratio) are
# both matched, what's left is dominated by *onset transients*: our sub-block
# volume ramp (module_tracker.md: reach target by the end of a 64-frame sub-block)
# and libopenmpt's own internal declick ramping are two independently-
# implemented answers to "how do we get from 0 (or the previous level) to the
# target without a click", and they settle along different sample-by-sample
# paths to the same destination. Measured on the checked-in fixtures (both
# gain-corrected, worst 512-frame window): notes_basic peaks at -19.1 dB
# right at its note-on; retrig_and_keyoff peaks at -11.7 dB at its row-2
# retrigger -- in both cases the *only* windows anywhere near the threshold
# are the ones straddling a note-on/retrigger boundary (verified directly
# against the per-tick trace), never a sustained region. -10 dB clears both
# with a little margin. A wrong note, a dropped trigger, or a gross timing
# slip instead replaces the signal with something uncorrelated of comparable
# amplitude to the signal itself -- error at or above 0 dB, not a marginal
# nudge -- so -10 dB stays unambiguously on the "known-good" side of that
# separation even though it's far looser than the ~-45..-50 dBFS quantization
# floor alone would suggest. Tightening this further is real future work
# (matching libopenmpt's ramp *shape*, not just reaching the same target) but
# isn't needed to make the harness useful now.
THRESHOLD_DB = -10.0

OPENMPT_ARGS = [
    "--render", "--no-float", "--samplerate", "44100", "--channels", "2",
    # Filter taps 2 == linear interpolation (matches mixer.h's mix_voice()
    # exactly, per module_tracker.md open question 2 / #16 -- the mixer never grew
    # a nearest/cubic/sinc build flag). Using openmpt123's own higher-quality
    # default here would make interpolation choice, not player correctness,
    # the dominant source of "divergence".
    "--filter", "2",
    "--gain", "0", "--stereo", "100",
    "--output-type", "wav",
]


@dataclass
class DiffReport:
    name: str
    dev_frames: int
    ref_frames: int
    peak_ref: int
    peak_dev: int
    gain_ratio: float  # least-squares best-fit scalar k in (k*dev ~= ref); see module docstring
    worst_err_db: float
    divergence_frame: Optional[int]
    divergence_time_s: Optional[float]
    location: Optional[str]
    passed: bool  # only meaningful when the caller asserts a tolerance


def ensure_build() -> None:
    subprocess.run(["make", "host"], cwd=REPO_ROOT, check=True,
                    stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not os.path.exists(RENDER_BIN):
        raise RuntimeError(f"{RENDER_BIN} missing after `make host`")


def convert_to_blob(xm_path: str, blob_path: str) -> None:
    prefix = blob_path + "_unused"
    subprocess.run(
        [sys.executable, os.path.join(XM2T00T_DIR, "xm2t00t.py"), "convert",
         xm_path, prefix, "--raw-blob", blob_path, "--raw-blob-only"],
        check=True, stdout=subprocess.DEVNULL,
    )


def render_device(blob_path: str, wav_path: str, trace_path: str, max_seconds: float = 180.0) -> None:
    subprocess.run(
        [RENDER_BIN, "render", blob_path, wav_path, trace_path, str(max_seconds)],
        check=True, stdout=subprocess.DEVNULL,
    )


def render_reference(xm_path: str, wav_path: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        local_xm = os.path.join(tmp, os.path.basename(xm_path))
        shutil.copyfile(xm_path, local_xm)
        subprocess.run(["openmpt123", *OPENMPT_ARGS, local_xm], cwd=tmp, check=True,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        produced = local_xm + ".wav"
        if not os.path.exists(produced):
            raise RuntimeError(f"openmpt123 did not produce {produced}")
        shutil.move(produced, wav_path)


def load_wav_stereo(path: str) -> np.ndarray:
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit PCM"
        n = w.getnframes()
        raw = w.readframes(n)
        data = np.frombuffer(raw, dtype="<i2")
        ch = w.getnchannels()
        data = data.reshape(-1, ch)
        if ch == 1:
            data = np.repeat(data, 2, axis=1)
        return data.astype(np.float64)


def parse_trace(path: str) -> List[Tuple[int, int, int, int, int, str]]:
    rows = []
    with open(path) as f:
        next(f)  # header
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            frame, order, pattern, row, tick, triggers = line.split(",", 5)
            rows.append((int(frame), int(order), int(pattern), int(row), int(tick), triggers))
    return rows


def locate(trace: List[Tuple[int, int, int, int, int, str]], frame: int) -> str:
    # trace is frame-ascending (one row per tick, emitted in playback order).
    best = None
    for entry in trace:
        if entry[0] <= frame:
            best = entry
        else:
            break
    if best is None:
        return "before the first tick"
    f, order, pattern, row, tick, triggers = best
    loc = f"order {order} (pattern {pattern}) row {row} tick {tick}, {frame - f} frames into it"
    if triggers:
        loc += f" [nearest triggers: {triggers}]"
    return loc


def diff_wavs(name: str, dev_path: str, ref_path: str, trace_path: str,
              threshold_db: float = THRESHOLD_DB) -> DiffReport:
    dev_full = load_wav_stereo(dev_path)
    ref_full = load_wav_stereo(ref_path)
    dev_len, ref_len = len(dev_full), len(ref_full)
    n = min(dev_len, ref_len)
    dev, ref = dev_full[:n], ref_full[:n]

    peak_ref = float(np.max(np.abs(ref))) if n else 0.0
    peak_dev = float(np.max(np.abs(dev))) if n else 0.0
    peak_scale = max(peak_ref, 1.0)  # avoid div-by-zero on a silent reference

    # Least-squares best-fit scalar k minimizing ||k*dev - ref||: the global
    # level-convention correction the module docstring explains. dev_energy
    # near 0 (an all-silent device render -- itself the loudest possible
    # signal, i.e. a real bug) leaves k at 1.0 rather than dividing by ~0.
    dev_energy = float(np.sum(dev.astype(np.float64) ** 2))
    gain_ratio = float(np.sum(dev * ref) / dev_energy) if dev_energy > 0 else 1.0
    dev_scaled = dev * gain_ratio

    trace = parse_trace(trace_path)

    worst_db = -math.inf
    divergence_frame = None
    nwin = n // WINDOW_FRAMES
    for i in range(nwin):
        s, e = i * WINDOW_FRAMES, (i + 1) * WINDOW_FRAMES
        diff = dev_scaled[s:e] - ref[s:e]
        rms = float(np.sqrt(np.mean(diff * diff)))
        err_db = 20.0 * math.log10(max(rms, 1e-9) / peak_scale)
        worst_db = max(worst_db, err_db)
        if divergence_frame is None and err_db > threshold_db:
            divergence_frame = s

    passed = divergence_frame is None
    return DiffReport(
        name=name, dev_frames=dev_len, ref_frames=ref_len,
        peak_ref=int(peak_ref), peak_dev=int(peak_dev), gain_ratio=gain_ratio, worst_err_db=worst_db,
        divergence_frame=divergence_frame,
        divergence_time_s=(divergence_frame / 44100.0) if divergence_frame is not None else None,
        location=locate(trace, divergence_frame) if divergence_frame is not None else None,
        passed=passed,
    )


def run_one(xm_path: str, name: str, workdir: str) -> DiffReport:
    blob_path = os.path.join(workdir, name + ".t00tblob")
    dev_wav = os.path.join(workdir, name + "_device.wav")
    trace_path = os.path.join(workdir, name + "_trace.csv")
    ref_wav = os.path.join(workdir, name + "_reference.wav")

    convert_to_blob(xm_path, blob_path)
    render_device(blob_path, dev_wav, trace_path)
    render_reference(xm_path, ref_wav)
    return diff_wavs(name, dev_wav, ref_wav, trace_path)


def print_report(r: DiffReport, asserted: bool) -> None:
    status = "PASS" if r.passed else ("FAIL" if asserted else "DIVERGES (informational)")
    gain_db = 20.0 * math.log10(r.gain_ratio) if r.gain_ratio > 0 else float("-inf")
    print(f"{status}  {r.name}: dev={r.dev_frames}f ref={r.ref_frames}f "
          f"peak(dev/ref)={r.peak_dev}/{r.peak_ref} gain_ratio={r.gain_ratio:.3f} ({gain_db:+.1f}dB) "
          f"worst_window={r.worst_err_db:.1f}dB")
    if r.divergence_frame is not None:
        print(f"       first divergence at frame {r.divergence_frame} "
              f"({r.divergence_time_s:.3f}s): {r.location}")


def main() -> int:
    ensure_build()

    failures = []

    import xm_synth
    with tempfile.TemporaryDirectory() as workdir:
        print("== synthetic regression fixtures (notes-only, asserted) ==")
        for fixture_name, builder in xm_synth.FIXTURES.items():
            xm_path = os.path.join(workdir, fixture_name + ".xm")
            with open(xm_path, "wb") as f:
                f.write(builder())
            r = run_one(xm_path, fixture_name, workdir)
            print_report(r, asserted=True)
            if not r.passed:
                failures.append(fixture_name)

        print("\n== real corpus (xm/, gitignored; informational -- effects/envelopes not implemented yet) ==")
        corpus = sorted(glob.glob(os.path.join(CORPUS_DIR, "*.xm")))
        if not shutil.which("openmpt123"):
            print("SKIP  openmpt123 not installed")
        elif not corpus:
            print(f"(corpus dir {CORPUS_DIR} has no .xm files -- populate it with any "
                  "legally-obtained XM modules to see informational reports)")
        else:
            for xm_path in corpus:
                base = os.path.splitext(os.path.basename(xm_path))[0]
                try:
                    r = run_one(xm_path, base, workdir)
                    print_report(r, asserted=False)
                except subprocess.CalledProcessError as exc:
                    print(f"ERROR {base}: {exc}")

    print()
    if failures:
        print(f"{len(failures)} regression fixture(s) FAILED: {', '.join(failures)}")
        return 1
    print("all regression fixtures passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
