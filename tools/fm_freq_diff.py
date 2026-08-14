#!/usr/bin/env python3
"""fm_freq_diff -- operator frequency conformance: t00t vs Dexed (history_fm.md §4, F5).

The one conformance test that cannot be a table diff. Every other control-plane
quantity is compared numerically by tools/fm_ctl_diff.py, but Dexed's
`Dx7Note::osc_freq()` is a private method with no accessor, so coarse / fine /
detune / fixed-frequency handling has to be measured from rendered audio.

The standard is still exact, only the method differs: each test voice
(tools/fm_ref/make_freq_bank.py) is a single carrier producing a bare sine, so
the spectral peak IS the operator frequency, and the two engines are compared in
cents. A pure sine measured over two seconds locates to well under a cent, which
is far tighter than any audible pitch error.

Setup:
    tools/fm_ref/.venv/bin/python tools/fm_ref/make_freq_bank.py
    make -C tools/fm_ref
    make -C tools/host_render -f Makefile.fm

Run:
    tools/fm_ref/.venv/bin/python tools/fm_freq_diff.py
    tools/fm_freq_diff.py --only detune --verbose
"""

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_compare import read_wav_f32  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
BANKS = REPO / "tools" / "fm_ref" / "banks"
DEXED_RENDER = REPO / "tools" / "fm_ref" / "dexed_render"
T00T_RENDER = REPO / "tools" / "host_render" / "render_fm_patch"
SYX2PATCH = REPO / "tools" / "syx2patch.py"
PATCHES_H = REPO / "src" / "engines" / "fm" / "patches.h"
SCRATCH = Path("/tmp/claude-1000/-home-carl-source-t00t/fm_freq")

# A frequency this low needs an impractically long window to resolve, and
# nothing musical lives there. Fixed-mode coarse 0 / fine 0 is 1 Hz.
MIN_MEASURABLE_HZ = 20.0

# Tolerance. One cent is already inaudible; three leaves room for the spectral
# estimator itself without admitting anything that would be heard as mistuned.
TOL_CENTS = 3.0


def measure_hz(path):
    """Frequency of the dominant partial, by parabolic interpolation on the
    magnitude peak of a Hann-windowed FFT."""
    x, sr = read_wav_f32(path)
    # Skip the first 100 ms so the envelope's attack does not smear the peak,
    # and use a power-of-two window from the sustained portion.
    start = int(0.1 * sr)
    n = 1 << 17
    seg = x[start:start + n]
    if len(seg) < n:
        seg = np.pad(seg, (0, n - len(seg)))
    if np.abs(seg).max() < 1e-9:
        return 0.0
    spec = np.abs(np.fft.rfft(seg * np.hanning(n)))
    k = int(np.argmax(spec))
    if 0 < k < len(spec) - 1:
        a, b, c = spec[k - 1], spec[k], spec[k + 1]
        denom = a - 2 * b + c
        k = k + (0.5 * (a - c) / denom if denom else 0.0)
    return k * sr / n


def cents(f_test, f_ref):
    if f_ref <= 0 or f_test <= 0:
        return None
    return 1200.0 * np.log2(f_test / f_ref)


def render(exe, bank, voice, note, out):
    res = subprocess.run(
        [str(exe), "--syx", str(bank), "--voice", str(voice), "--note", str(note),
         "--vel", "99", "--gate", "3.0", "--tail", "0.1", "--out", str(out)],
        capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"error: {exe.name} failed on {bank.name} voice {voice}:\n{res.stderr}")


def run_bank(name, note, verbose):
    bank = BANKS / f"freq_{name}.syx"
    if not bank.exists():
        sys.exit(f"error: {bank} missing -- run tools/fm_ref/make_freq_bank.py")

    # The t00t side renders whatever patches.h holds, so it has to be rebuilt
    # per bank. That is the real device conversion path, which is the point:
    # this test covers syx2patch.py's op_ratio/op_detune_cents/op_fixed_hz as
    # much as it covers the engine.
    subprocess.run([sys.executable, str(SYX2PATCH), "convert", str(bank), str(PATCHES_H)],
                   capture_output=True, text=True, check=True)
    subprocess.run(["make", "-C", str(REPO / "tools" / "host_render"), "-f", "Makefile.fm"],
                   capture_output=True, text=True, check=True)

    SCRATCH.mkdir(parents=True, exist_ok=True)
    names = subprocess.run([str(DEXED_RENDER), "--syx", str(bank), "--list"],
                           capture_output=True, text=True).stdout.splitlines()

    rows, skipped = [], 0
    for v in range(32):
        label = names[v][4:].strip() if v < len(names) else f"v{v}"
        if label == "UNUSED":
            continue
        ref_wav, test_wav = SCRATCH / "ref.wav", SCRATCH / "test.wav"
        render(DEXED_RENDER, bank, v, note, ref_wav)
        render(T00T_RENDER, bank, v, note, test_wav)
        f_ref, f_test = measure_hz(ref_wav), measure_hz(test_wav)
        if f_ref < MIN_MEASURABLE_HZ:
            skipped += 1
            continue
        rows.append((label, f_ref, f_test, cents(f_test, f_ref)))

    bad = [r for r in rows if r[3] is None or abs(r[3]) > TOL_CENTS]
    worst = max((abs(r[3]) for r in rows if r[3] is not None), default=0.0)
    msg = (f"{len(rows) - len(bad)}/{len(rows)} within {TOL_CENTS:g} cents, "
           f"worst {worst:.2f} cents")
    if skipped:
        msg += f" ({skipped} below {MIN_MEASURABLE_HZ:g} Hz, unmeasurable)"

    detail = []
    if bad:
        detail.append(f"{'voice':<10} {'dexed Hz':>11} {'t00t Hz':>11} {'cents':>9}")
        for label, fr, ft, c in sorted(bad, key=lambda r: -abs(r[3] or 0))[:12]:
            detail.append(f"{label:<10} {fr:>11.3f} {ft:>11.3f} {c:>+9.2f}")
    elif verbose:
        detail.append(f"{'voice':<10} {'dexed Hz':>11} {'t00t Hz':>11} {'cents':>9}")
        for label, fr, ft, c in rows[:12]:
            detail.append(f"{label:<10} {fr:>11.3f} {ft:>11.3f} {c:>+9.2f}")
    return not bad, msg, detail


# Detune is the one setting whose real behaviour is note-dependent (Dexed's
# `detuneRatio` falls off with the note's own log frequency), so it is measured
# at three octaves rather than one. Everything else is note-invariant by
# construction and one note is enough.
TESTS = [("coarse", 48), ("fine", 48),
         ("detune", 24), ("detune", 48), ("detune", 84),
         ("fixed", 48)]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", help="run only tests whose name contains this")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    saved = PATCHES_H.read_bytes() if PATCHES_H.exists() else None
    results = []
    try:
        for name, note in TESTS:
            label = f"freq/{name}@n{note}"
            if args.only and args.only not in label:
                continue
            ok, msg, detail = run_bank(name, note, args.verbose)
            results.append((label, ok))
            print(f"[{'PASS' if ok else 'FAIL'}] {label:<20} {msg}")
            for line in detail:
                print(f"         {line}")
    finally:
        # Leave patches.h as it was found: this script rewrites it per bank, and
        # silently swapping the user's converted bank for a synthetic sweep
        # would poison the next fm_compare run.
        if saved is not None:
            PATCHES_H.write_bytes(saved)
            subprocess.run(["make", "-C", str(REPO / "tools" / "host_render"),
                            "-f", "Makefile.fm"], capture_output=True)

    passed = sum(1 for _, ok in results if ok)
    print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
