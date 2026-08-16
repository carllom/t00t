#!/usr/bin/env python3
"""opl_ctl_diff -- exact control-plane conformance: t00t vs Nuked-OPL3
(module_opl.md's Future/TODO, mirroring tools/fm_ctl_diff.py's structure).

The frequency-multiplier table, the KSL ROM, and the TL scale are literal
register tables both sides claim to be the same as real hardware's --
those diff exactly, zero tolerance. The envelope generator's rate/level
curves are t00t's own linear-ramp approximation of a real chip process
(env_opl.h's own header comment); those diff as resampled trajectories
against Nuked-OPL3's real per-sample simulation, with a tolerance loose
enough to accept that approximation but tight enough to catch an actual
regression in it.

Run:
    tools/opl_ref/nuked_dump must be built (tools/opl_ref: make)
    tools/host_render/build/t00t_opl_ctl_dump must be built (repo root: make host)

    python3 tools/opl_ctl_diff.py
    python3 tools/opl_ctl_diff.py --only eg/attack
    python3 tools/opl_ctl_diff.py --verbose
"""

import argparse
import csv
import io
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
NUKED_DUMP = REPO / "tools" / "opl_ref" / "nuked_dump"
T00T_DUMP = REPO / "tools" / "host_render" / "build" / "t00t_opl_ctl_dump"

# Trajectory comparison grid. Both dumps are already per-sample (44100 Hz,
# ~0.0227 ms/sample) -- unlike fm_ctl_diff's Dexed-vs-t00t comparison, neither
# side here advances its envelope in coarser control blocks, so this grid
# exists only to smooth sample-to-sample interpolation noise, not to paper
# over a block-size mismatch.
GRID_MS = 1.0

# Nuked-OPL3's snap from silence to the ceiling on an instant attack
# (opl_combined_rate>=60, env_opl.h) lands one sample earlier or later than
# Nuked's own two-sample eg_rout/eg_out update lag -- a real, bounded,
# single-sample alignment artefact, not an envelope error. Excluded from
# every trajectory comparison the same way fm_ctl_diff excludes Dexed's
# first control block.
SKIP_MS = 0.1


def run(exe, args):
    if not exe.exists():
        sys.exit(f"error: {exe} not built.\n"
                 f"  tools/opl_ref:   make\n"
                 f"  repo root:       make host")
    res = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"error: {exe.name} {' '.join(args)} failed:\n{res.stderr}")
    return res.stdout


def parse_csv(text):
    lines = [l for l in text.splitlines() if l and not l.startswith("#")]
    r = csv.reader(lines)
    header = next(r)
    return header, [tuple(row) for row in r if row]


def cmp_exact_table(nuked_csv, t00t_csv, verbose):
    """Register tables: every row must match exactly, keyed on all but the
    last column (which may be a string tag, e.g. KSL's "rom"/"shift"). No
    tolerance -- both sides claim to be the same real hardware table, so any
    difference at all is a real finding."""
    hd, rd = parse_csv(nuked_csv)
    ht, rt = parse_csv(t00t_csv)
    if hd != ht:
        return False, f"column mismatch: {hd} vs {ht}", []

    dd = {row[:-1]: float(row[-1]) for row in rd}
    dt = {row[:-1]: float(row[-1]) for row in rt}
    if dd.keys() != dt.keys():
        return False, f"key sets differ ({len(dd)} vs {len(dt)} rows)", []

    bad = [(k, dd[k], dt[k]) for k in dd if dd[k] != dt[k]]
    if not bad:
        return True, f"{len(dd)} rows identical", []
    worst = sorted(bad, key=lambda b: -abs(b[1] - b[2]))[:8]
    detail = [f"{hd[:-1]}={k}  nuked={d:g}  t00t={t:g}  (delta {t - d:+g})"
              for k, d, t in worst]
    return False, f"{len(bad)}/{len(dd)} rows differ", detail


def resample(rows, col, grid, sign=1.0):
    t = np.array([float(r[0]) for r in rows])
    v = np.array([float(r[col]) for r in rows]) * sign
    return np.interp(grid, t, v)


def cmp_eg_trajectory(nuked_csv, t00t_csv, tol_mae, tol_max, verbose):
    """One operator's envelope, in dB. Nuked-OPL3's `db` column (cols=
    time_s,eg_rout,eg_out,db) is 0 at the loudest and grows more POSITIVE
    with attenuation; t00t's own `db` column (cols=time_s,level,db) is 0 at
    the loudest and grows more NEGATIVE -- both internally consistent,
    opposite sign conventions, so Nuked's side is negated before comparing.

    `floor` clamps both sides -- Nuked's 511-step register floors near
    -95.8 dB, t00t's 15-octave level domain near -90.3 dB; both are
    inaudible, but the gap between those two floors would otherwise
    dominate every case's `max` once either side reaches silence.
    """
    _, rd = parse_csv(nuked_csv)
    _, rt = parse_csv(t00t_csv)
    if not rd or not rt:
        return False, "empty trajectory", []

    end = min(float(rd[-1][0]), float(rt[-1][0]))
    grid = np.arange(0.0, end, GRID_MS / 1000.0)
    d = resample(rd, 3, grid, sign=-1.0)
    t = resample(rt, 2, grid, sign=1.0)
    floor = -90.0
    d = np.maximum(d, floor)
    t = np.maximum(t, floor)

    err = np.abs(d - t)
    keep = grid >= SKIP_MS / 1000.0
    skipped_note = ""
    if keep.any() and not keep.all():
        skipped_note = f"  [first {SKIP_MS:g} ms excluded, max there {err[~keep].max():.2f}]"
        err, grid = err[keep], grid[keep]

    mae, mx = float(err.mean()), float(err.max())
    at = grid[int(np.argmax(err))]
    ok = mae <= tol_mae and mx <= tol_max
    msg = (f"MAE {mae:.2f} dB (tol {tol_mae:g}), "
           f"max {mx:.2f} dB @ {at * 1000:.0f} ms (tol {tol_max:g}){skipped_note}")

    detail = []
    if not ok and verbose:
        detail.append(f"{'time':>9} {'nuked':>10} {'t00t':>10} {'delta':>9}")
        step = max(1, len(grid) // 20)
        for i in range(0, len(grid), step):
            detail.append(f"{grid[i] * 1000:>8.0f}m {d[i]:>10.2f} {t[i]:>10.2f} {t[i] - d[i]:>+9.2f}")
    return ok, msg, detail


# Representative envelope configs, split by which stage dominates the
# window. All args after --domain eg are shared with both dump tools
# unmodified (see tools/opl_ref/README.md / t00t_opl_ctl_dump.cpp --help).
#
# Decay/release/percussive are a real linear ramp on real hardware too (a
# constant per-tick register increment, see env_opl.h's own header comment)
# so these hold FM's own tolerance (fm_ctl_diff.py's EG_CASES): tight enough
# to catch a real rate-table or key-scale regression.
EG_DECAY_CASES = [
    # name, ksl, tl, ar, dr, sl, rr, egt, ksr, note
    ("eg/decay-fast",   0, 0, 15, 12, 15, 1, 1, 0, 12),
    ("eg/decay-slow",   0, 0, 15, 4,  15, 1, 1, 0, 12),
    # High note + KSR on: exercises opl_combined_rate's key-scale
    # contribution, not just the register rate.
    ("eg/decay-keyscale", 0, 0, 15, 6, 15, 1, 1, 1, 93),
    ("eg/release-fast", 0, 0, 15, 15, 0, 10, 1, 0, 12),
    ("eg/release-slow", 0, 0, 15, 15, 0, 4,  1, 0, 12),
    # EGT off: decay must run through the sustain level to silence at the
    # RELEASE rate, not decay's own -- history_opl.md's own note on why this
    # isn't decay-rate-to-zero (see env_opl_advance's stage-2 case).
    ("eg/percussive",   0, 0, 15, 6,  8, 8,  0, 0, 12),
    # KSL's per-note attenuation, exercised through the full envelope
    # composition (not just table/ksl's raw ROM numbers) -- AR/DR/RR chosen
    # so the ceiling is reached and held, isolating the KSL contribution.
    ("eg/ksl-atten",    2, 0, 15, 0,  0, 0,  1, 0, 93),
]

# Attack is a genuine shape gap, not a tuning error: real hardware's attack
# is an exponential approach to the ceiling (Nuked-OPL3's
# `eg_inc = ~eg_rout >> (4-shift)`, proportional to the remaining distance);
# env_opl.h's is a linear ramp calibrated to the same real total duration
# (env_opl.h's own header comment, module_opl.md's Future/TODO). A linear
# ramp cannot reproduce that curve's fast-then-slower shape at any tolerance
# tight enough to also catch a real rate regression, so this tolerance is
# wide enough to accept the documented shape gap and still catch a rate
# table that's off by more than the shape gap itself.
EG_ATTACK_CASES = [
    ("eg/attack-fast",     0, 0, 12, 1, 0, 1, 1, 0, 12),
    ("eg/attack-slow",     0, 0, 3,  1, 0, 1, 1, 0, 12),
    ("eg/attack-keyscale", 0, 0, 6,  1, 0, 1, 1, 1, 93),
]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", help="run only tests whose name contains this")
    p.add_argument("--verbose", action="store_true", help="show worst offending rows")
    args = p.parse_args()

    results = []

    def record(name, ok, msg, detail):
        if args.only and args.only not in name:
            return
        results.append((name, ok, msg))
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name:<22} {msg}")
        for line in detail:
            print(f"         {line}")

    def want(name):
        return not args.only or args.only in name

    # --- register tables: exact ---
    for what, label in (("mult", "table/mult"), ("tl", "table/tl"), ("ksl", "table/ksl")):
        if not want(label):
            continue
        record(label, *cmp_exact_table(run(NUKED_DUMP, ["--domain", what]),
                                       run(T00T_DUMP, ["--domain", what]), args.verbose))

    # --- envelope trajectories ---
    def run_eg_case(ksl, tl, ar, dr, sl, rr, egt, ksr, note, gate, dur):
        a = ["--domain", "eg", "--mult", "1", "--ksl", str(ksl), "--tl", str(tl),
             "--ar", str(ar), "--dr", str(dr), "--sl", str(sl), "--rr", str(rr),
             "--egt", str(egt), "--ksr", str(ksr), "--note", str(note),
             "--vel", "127", "--gate", str(gate), "--dur", str(dur)]
        return run(NUKED_DUMP, a), run(T00T_DUMP, a)

    for name, ksl, tl, ar, dr, sl, rr, egt, ksr, note in EG_DECAY_CASES:
        if not want(name):
            continue
        nuked_csv, t00t_csv = run_eg_case(ksl, tl, ar, dr, sl, rr, egt, ksr, note,
                                          gate=1.0, dur=2.0)
        record(name, *cmp_eg_trajectory(nuked_csv, t00t_csv, tol_mae=0.5, tol_max=5.0,
                                        verbose=args.verbose))

    for name, ksl, tl, ar, dr, sl, rr, egt, ksr, note in EG_ATTACK_CASES:
        if not want(name):
            continue
        nuked_csv, t00t_csv = run_eg_case(ksl, tl, ar, dr, sl, rr, egt, ksr, note,
                                          gate=1.0, dur=2.0)
        record(name, *cmp_eg_trajectory(nuked_csv, t00t_csv, tol_mae=10.0, tol_max=45.0,
                                        verbose=args.verbose))

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
