#!/usr/bin/env python3
"""fm_ctl_diff -- exact control-plane conformance: t00t vs Dexed (fm2.md §3, F1).

The half of the harness that does not involve audio, spectra, or judgement.

Everything the DX7 does except the per-sample operator kernel runs at control
rate and produces numbers: level curves, rate curves, key level/rate scaling,
velocity, the LFO, the pitch EG, the algorithm table. Those diff against Dexed's
numbers directly. That converts most of "does it sound right?" into pass/fail,
and — unlike a spectral score — it cannot go green with two errors cancelling.

Run:
    tools/fm_ref/.venv/bin/python tools/fm_ctl_diff.py
    tools/fm_ctl_diff.py --only eg          # one test
    tools/fm_ctl_diff.py --verbose          # show worst offending rows
"""

import argparse
import csv
import importlib.util
import io
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
DEXED_DUMP = REPO / "tools" / "fm_ref" / "dexed_dump"
T00T_DUMP = REPO / "tools" / "host_render" / "t00t_ctl_dump"

# Trajectory comparison grid. Coarser than either engine's control rate on
# purpose: t00t steps its EG every FM_BLOCK=16 samples (0.36 ms) and Dexed every
# N=64 (1.45 ms), so anything finer than the slower of the two would score a
# pure block-size artefact as an error. 5 ms is comfortably above both and still
# far finer than any DX7 envelope feature worth hearing.
GRID_MS = 5.0


def run(exe, args):
    if not exe.exists():
        sys.exit(f"error: {exe} not built.\n"
                 f"  tools/fm_ref:      make dexed_dump\n"
                 f"  tools/host_render: make -f Makefile.fm t00t_ctl_dump")
    res = subprocess.run([str(exe), *args], capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"error: {exe.name} {' '.join(args)} failed:\n{res.stderr}")
    return res.stdout


def parse_csv(text):
    r = csv.reader(io.StringIO(text))
    header = next(r)
    rows = [tuple(float(v) for v in row) for row in r if row]
    return header, rows


def cmp_exact_table(name, dexed_csv, t00t_csv, verbose):
    """Integer tables: every row must match exactly, keyed on all but the last
    column. No tolerance — both sides claim to be the same ported function, so
    any difference at all is a real finding."""
    hd, rd = parse_csv(dexed_csv)
    ht, rt = parse_csv(t00t_csv)
    if hd != ht:
        return False, f"column mismatch: {hd} vs {ht}", []

    dd = {row[:-1]: row[-1] for row in rd}
    dt = {row[:-1]: row[-1] for row in rt}
    if dd.keys() != dt.keys():
        return False, f"key sets differ ({len(dd)} vs {len(dt)} rows)", []

    bad = [(k, dd[k], dt[k]) for k in dd if dd[k] != dt[k]]
    if not bad:
        return True, f"{len(dd)} rows identical", []
    worst = sorted(bad, key=lambda b: -abs(b[1] - b[2]))[:8]
    detail = [f"{hd[:-1]}={tuple(int(v) for v in k)}  dexed={d:g}  t00t={t:g}  (delta {t - d:+g})"
              for k, d, t in worst]
    return False, f"{len(bad)}/{len(dd)} rows differ", detail


def resample(rows, col, grid):
    t = np.array([r[0] for r in rows])
    v = np.array([r[col] for r in rows])
    return np.interp(grid, t, v)


def cmp_trajectory(name, dexed_csv, t00t_csv, tol_mae, tol_max, unit, verbose, col=1,
                   floor=None, skip_ms=0.0):
    """Time series: resample both onto a common grid and compare.

    `floor` clamps both sides before comparing. The EG needs it: the two engines
    represent "silent" differently — Dexed's `level_` starts at 0, which lands
    about -90 dB below the shared reference, while env_dx.h starts at
    its own floor far below that. Both are inaudible, but the numeric
    gap between them is enormous and would otherwise dominate `max` on every
    single EG case and drown out the real differences further up the curve.
    """
    hd, rd = parse_csv(dexed_csv)
    ht, rt = parse_csv(t00t_csv)
    if not rd or not rt:
        return False, "empty trajectory", []

    end = min(rd[-1][0], rt[-1][0])
    grid = np.arange(0.0, end, GRID_MS / 1000.0)
    d = resample(rd, col, grid)
    t = resample(rt, col, grid)
    if floor is not None:
        d = np.maximum(d, floor)
        t = np.maximum(t, floor)

    err = np.abs(d - t)

    # `skip_ms` excludes the reference's own first control block from the
    # scored region. Dexed advances its envelope once per 64 samples (1.45 ms)
    # and t00t once per FM_BLOCK (16 samples, 0.36 ms), so inside that first
    # block the reference has no information: a fast attack is already at its
    # target in Dexed's first emitted sample while t00t shows the three
    # intermediate steps it actually took to get there. Both reach the same
    # level in the same 1.45 ms; only the sampling differs. Scoring that region
    # measures the block-size deviation (documented, deliberate) rather than
    # the envelope. The excluded region's own worst error is still reported, so
    # a real defect hiding there stays visible instead of being defined away.
    skipped_note = ""
    if skip_ms > 0.0:
        keep = grid >= skip_ms / 1000.0
        if keep.any() and not keep.all():
            skipped_note = f"  [first {skip_ms:g} ms excluded, max there {err[~keep].max():.2f}]"
            err, grid = err[keep], grid[keep]

    mae, mx = float(err.mean()), float(err.max())
    at = grid[int(np.argmax(err))]
    ok = mae <= tol_mae and mx <= tol_max
    msg = (f"MAE {mae:.2f} {unit} (tol {tol_mae:g}), "
           f"max {mx:.2f} {unit} @ {at * 1000:.0f} ms (tol {tol_max:g}){skipped_note}")

    detail = []
    if not ok and verbose:
        detail.append(f"{'time':>9} {'dexed':>10} {'t00t':>10} {'delta':>9}")
        step = max(1, len(grid) // 20)
        for i in range(0, len(grid), step):
            detail.append(f"{grid[i] * 1000:>8.0f}m {d[i]:>10.2f} {t[i]:>10.2f} {t[i] - d[i]:>+9.2f}")
    return ok, msg, detail


def measure_hz(rows, col=1):
    """Fundamental frequency of a control-rate trajectory, by FFT peak.

    The trajectory is uniformly sampled (one row per control block), so a plain
    rFFT of the mean-removed signal with a Hann window locates the fundamental;
    parabolic interpolation on the peak bin gets well inside the 1/duration bin
    spacing, which matters at the slow end where a 3 s capture holds only a few
    cycles.
    """
    t = np.array([r[0] for r in rows])
    v = np.array([r[col] for r in rows])
    dt = (t[-1] - t[0]) / (len(t) - 1)
    v = (v - v.mean()) * np.hanning(len(v))
    spec = np.abs(np.fft.rfft(v))
    spec[0] = 0.0
    k = int(np.argmax(spec))
    if 0 < k < len(spec) - 1:
        a, b, c = spec[k - 1], spec[k], spec[k + 1]
        denom = a - 2 * b + c
        k = k + (0.5 * (a - c) / denom if denom else 0.0)
    return k / (len(v) * dt)


def cmp_lfo_rate(verbose):
    """LFO frequency across all 100 rate values.

    Split out from the waveform comparison deliberately, for the same reason F0
    splits timbre from envelope: if the two LFOs run at different speeds their
    waveforms drift out of phase and a sample-by-sample diff reports ~0.5 mean
    error on every waveform, which looks like six broken waveforms rather than
    one broken rate table.
    """
    bad, rows = [], []
    for rate in range(0, 100, 3):
        a = ["--what", "lfo", "--lfo", f"{rate},0,99,99,1,4", "--dur", "6.0"]
        _, rd = parse_csv(run(DEXED_DUMP, a))
        _, rt = parse_csv(run(T00T_DUMP, a))
        hz_d, hz_t = measure_hz(rd), measure_hz(rt)
        ratio = hz_t / hz_d if hz_d > 0 else float("inf")
        rows.append((rate, hz_d, hz_t, ratio))
        if abs(ratio - 1.0) > 0.05:      # 5%: well inside "same LFO speed" perceptually
            bad.append((rate, hz_d, hz_t, ratio))
    if not bad:
        return True, f"{len(rows)} rate values within 5%", []
    worst = sorted(bad, key=lambda r: -abs(r[3] - 1.0))[:8]
    detail = [f"rate {r:>2}: dexed {d:>7.3f} Hz  t00t {t:>7.3f} Hz  (x{q:.2f})"
              for r, d, t, q in worst]
    return False, f"{len(bad)}/{len(rows)} rate values off by >5%", detail


def cmp_lfo_shape(waveform, verbose):
    """Waveform shape over one period, each side normalised to its own measured
    period so a rate error (covered by cmp_lfo_rate) cannot show up here too."""
    a = ["--what", "lfo", "--lfo", f"40,0,99,99,1,{waveform}", "--dur", "6.0"]
    _, rd = parse_csv(run(DEXED_DUMP, a))
    _, rt = parse_csv(run(T00T_DUMP, a))

    NBINS = 128

    def one_period(rows):
        hz = measure_hz(rows)
        if hz <= 0:
            return None
        t = np.array([r[0] for r in rows])
        v = np.array([r[1] for r in rows])
        # Fold many cycles onto one and bin-average by phase. Averaging (rather
        # than interpolating a scatter of duplicated phases) is what makes this
        # robust: each bin gets many independent samples from different cycles,
        # so control-block sampling jitter averages out instead of being picked
        # up as shape error. Skips the first second so no start-up transient or
        # delay ramp leaks in.
        keep = (t > 1.0) & (t < 5.0)
        phase = (t[keep] * hz) % 1.0
        idx = np.minimum((phase * NBINS).astype(int), NBINS - 1)
        total = np.bincount(idx, weights=v[keep], minlength=NBINS)
        count = np.bincount(idx, minlength=NBINS)
        if (count == 0).any():
            return None
        return total / count

    pd_, pt = one_period(rd), one_period(rt)
    if pd_ is None or pt is None:
        return False, "could not measure a period", []
    err = np.abs(pd_ - pt)
    mae, mx = float(err.mean()), float(err.max())

    # `max` alone would fail even a correct sawtooth, whose one-sample
    # discontinuity lands in a different phase bin on the two sides. Judge on
    # MAE and let the detail carry the shape.
    ok = mae <= 0.05

    # A half-cycle roll is checked explicitly because it is the single most
    # likely LFO defect and the most confusable one: for a symmetric waveform
    # (triangle, square, sine) a half-cycle phase offset and a polarity
    # inversion are indistinguishable from the shape alone. Reporting "matches
    # when rolled half a cycle" turns a vague shape mismatch into a one-line
    # finding. Note this cannot be a global phase-origin error if the sawtooths
    # pass as-is, since a roll would break those instead.
    rolled = float(np.abs(pd_ - np.roll(pt, NBINS // 2)).mean())
    msg = f"shape MAE {mae:.3f} (tol 0.05), max {mx:.3f}"
    if not ok and rolled <= 0.05:
        msg += f"  -- but MAE {rolled:.3f} when t00t is rolled half a cycle"

    detail = []
    if not ok and verbose:
        idx = np.linspace(0, NBINS - 1, 8).astype(int)
        detail.append("phase " + " ".join(f"{p:>6.2f}" for p in np.linspace(0, 1, 8, endpoint=False)))
        detail.append("dexed " + " ".join(f"{pd_[i]:>6.2f}" for i in idx))
        detail.append("t00t  " + " ".join(f"{pt[i]:>6.2f}" for i in idx))
    return ok, msg, detail


def cmp_lfo_sample_hold(verbose):
    """Sample & hold, which shape-folding cannot judge: it is a PRNG sequence,
    so the two sides legitimately hold different values in a different order.
    What must match is the *behaviour* -- one new value per LFO cycle, held
    constant in between, spanning the full 0..1 range. Which value comes first
    is inaudible and is not compared."""
    a = ["--what", "lfo", "--lfo", "40,0,99,99,1,5", "--dur", "6.0"]
    out = []
    for exe in (DEXED_DUMP, T00T_DUMP):
        _, rows = parse_csv(run(exe, a))
        t = np.array([r[0] for r in rows])
        v = np.array([r[1] for r in rows])
        hz = measure_hz(rows)
        changes = int(np.count_nonzero(np.abs(np.diff(v)) > 1e-9))
        span = t[-1] - t[0]
        out.append((changes / span, float(v.min()), float(v.max()), hz))

    (cd, lod, hid, _), (ct, lot, hit, _) = out
    ratio = ct / cd if cd > 0 else float("inf")
    ok = abs(ratio - 1.0) <= 0.10 and abs((hit - lot) - (hid - lod)) <= 0.15
    msg = (f"steps/s dexed {cd:.2f} vs t00t {ct:.2f} (x{ratio:.2f}), "
           f"range dexed {lod:.2f}..{hid:.2f} vs t00t {lot:.2f}..{hit:.2f}")
    return ok, msg, []


def cmp_algorithms(dexed_csv, verbose):
    """t00t's 32-algorithm table lives in tools/syx2patch.py, not in C++, so it
    is imported rather than dumped from a binary. The bus-flag bytes should be
    byte-identical to Dexed's `FmCore::algorithms` — both claim to be the same
    table."""
    spec = importlib.util.spec_from_file_location("syx2patch", REPO / "tools" / "syx2patch.py")
    mod = importlib.util.module_from_spec(spec)
    # Register before exec: syx2patch defines @dataclass types, and dataclasses
    # resolves a class's own module out of sys.modules while processing it.
    sys.modules["syx2patch"] = mod
    spec.loader.exec_module(mod)

    _, rows = parse_csv(dexed_csv)
    bad = []
    for alg, op, in_bus, out_bus, fb_in, fb_out, carrier in rows:
        flags = mod.DX7_ALGORITHMS[int(alg)][int(op)]
        got = ((flags >> 4) & 3, flags & 3, 1 if flags & 0x40 else 0, 1 if flags & 0x80 else 0)
        want = (int(in_bus), int(out_bus), int(fb_in), int(fb_out))
        if got != want:
            bad.append((int(alg) + 1, int(op), want, got))
    if not bad:
        return True, f"{len(rows)} (algorithm, operator) entries identical", []
    detail = [f"alg {a} op {o}: dexed(in,out,fb_in,fb_out)={w} t00t={g}" for a, o, w, g in bad[:10]]
    return False, f"{len(bad)}/{len(rows)} entries differ", detail


def cmp_routing(dexed_csv, verbose):
    """The modulation GRAPH each algorithm decodes to, not the bytes it decodes
    from.

    `cmp_algorithms` above compares the raw bus-flag bytes and has passed
    192/192 since F1 -- while `decode_algorithm()` was silently dropping two
    thirds of the modulation edges in algorithm 22. Identical inputs to a lossy
    decoder still produce identical inputs. F7 (fm2.md §5.20) found that the
    expensive way, so the decoder's *output* is now checked too.

    The reference side is Dexed's `FmCore::render` bus behaviour, re-derived
    here from the same flag bytes: a bus keeps its contents until an operator
    writes it WITHOUT the add flag, so every operator that reads it in the
    meantime is modulated by every operator currently in it. That is the one
    rule the old decoder got wrong (it emptied the bus on first read).
    """
    spec = importlib.util.spec_from_file_location("syx2patch", REPO / "tools" / "syx2patch.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["syx2patch"] = mod
    spec.loader.exec_module(mod)

    def reference_edges(flags):
        contents = {0: [], 1: [], 2: []}
        has = {0: True, 1: False, 2: False}
        edges = set()
        for op in range(6):
            f = flags[op]
            add = bool(f & 0x04)
            in_bus, out_bus = (f >> 4) & 3, f & 3
            if in_bus != 0 and has[in_bus]:
                for src in contents[in_bus]:
                    edges.add((src, op))
            if not (add and has[out_bus]):
                contents[out_bus] = []
            contents[out_bus].append(op)
            has[out_bus] = True
        return edges

    bad = []
    for alg in range(32):
        flags = mod.DX7_ALGORITHMS[alg]
        want = reference_edges(flags)
        decode = mod.decode_algorithm(flags)
        got = set()
        for j, r in enumerate(decode.routing):
            if r.target != "OUT":
                got.add((j, r.target))
            for t in r.extra_targets:
                got.add((j, t))
        if got != want:
            missing = sorted(want - got)
            extra = sorted(got - want)
            bad.append((alg + 1, missing, extra))
    if not bad:
        return True, "32 algorithms decode to Dexed's exact modulation graph", []
    detail = [f"alg {a}: missing edges {m}, spurious {e}" for a, m, e in bad[:8]]
    return False, f"{len(bad)}/32 algorithms decode to the wrong graph", detail


# Representative EG configs. Chosen to exercise the shapes real patches use and
# the ones fm2.md §1.1 predicts are broken, not to be exhaustive.
EG_CASES = [
    ("eg/instant-attack",  "99,99,99,99", "99,99,99,99", 99),   # reference config
    ("eg/epiano-carrier",  "95,50,35,60", "99,90,80,0",  99),   # the F0 baseline patch's shape
    ("eg/slow-attack",     "20,30,25,40", "99,80,70,0",  99),   # tests the rising curve family
    ("eg/very-slow-decay", "99,5,5,30",   "99,60,50,0",  99),   # tests §1.1(d)'s step<1 clamp
    ("eg/percussive",      "99,70,60,80", "99,50,0,0",   99),   # decay to silence while gated
    ("eg/low-outlevel",    "99,60,40,60", "99,85,75,0",  70),   # tests the TL composition
    # F6 (fm2.md §5.15) added the last two. The six above all have L2 != L1 and
    # all have L4 == 0, so between them they never once enter Env's
    # `targetlevel_ == level_` branch -- the ACCURATE_ENVELOPE `staticcount`
    # hold, which is the whole reason the statics[77] table exists -- and never
    # release UPWARDS. Both paths are ported in env_dx.h and neither was under
    # test; both are exercised by ROM1A #30 TRAIN, whose two remaining outlier
    # operators are precisely one of each.
    ("eg/static-hold",     "42,17,25,53", "99,99,99,0",  83),   # TRAIN op4: L1=L2=L3 -> two staticcount holds
    ("eg/rising-release",  "49,17,25,53", "99,99,99,98", 99),   # TRAIN op5: L4 > sustain -> release rises
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
        print(f"[{status}] {name:<26} {msg}")
        for line in detail:
            print(f"         {line}")

    def want(name):
        return not args.only or args.only in name

    # --- integer tables: exact ---
    for what, label in (("levels", "table/scaleoutlevel"),
                        ("scalerate", "table/scale-rate"),
                        ("scalelevel", "table/scale-level"),
                        ("velocity", "table/velocity")):
        if not want(label):
            continue
        record(label, *cmp_exact_table(label, run(DEXED_DUMP, ["--what", what]),
                                       run(T00T_DUMP, ["--what", what]), args.verbose))

    # --- algorithm routing table: exact ---
    if want("table/routing"):
        record("table/routing", *cmp_routing(None, args.verbose))
    if want("table/algorithms"):
        record("table/algorithms", *cmp_algorithms(run(DEXED_DUMP, ["--what", "algo"]), args.verbose))

    # --- amplitude EG trajectories ---
    # Tolerance rationale: 1 dB mean is under a just-noticeable loudness step and
    # is roughly what the two block sizes alone can account for at a stage
    # boundary; 3 dB peak allows one grid cell of transition skew without
    # allowing a genuinely different curve or landing level.
    for label, rates, levels, ol in EG_CASES:
        if not want(label):
            continue
        a = ["--what", "eg", "--rates", rates, "--levels", levels,
             "--outlevel", str(ol), "--gate", "2.0", "--dur", "4.0"]
        record(label, *cmp_trajectory(label, run(DEXED_DUMP, a), run(T00T_DUMP, a),
                                      tol_mae=1.0, tol_max=3.0, unit="dB",
                                      verbose=args.verbose, floor=-100.0,
                                      skip_ms=1.5))

    # --- pitch EG ---
    for label, rates, levels in (("pitcheg/flat", "99,99,99,99", "50,50,50,50"),
                                 ("pitcheg/sweep", "80,60,40,70", "80,60,50,50"),
                                 ("pitcheg/drop", "99,50,30,60", "20,40,50,50")):
        if not want(label):
            continue
        a = ["--what", "pitcheg", "--rates", rates, "--levels", levels,
             "--gate", "2.0", "--dur", "4.0"]
        record(label, *cmp_trajectory(label, run(DEXED_DUMP, a), run(T00T_DUMP, a),
                                      tol_mae=10.0, tol_max=30.0, unit="cents",
                                      verbose=args.verbose))

    # --- LFO: rate, then waveform shape, then the delay ramp ---
    if want("lfo/rate"):
        record("lfo/rate", *cmp_lfo_rate(args.verbose))
    for w in range(5):   # waveform 5 (sample & hold) has its own test below
        label = f"lfo/shape-w{w}"
        if want(label):
            record(label, *cmp_lfo_shape(w, args.verbose))
    if want("lfo/sample-hold"):
        record("lfo/sample-hold", *cmp_lfo_sample_hold(args.verbose))
    for label, lfo in (("lfo/delay-off", "40,0,99,99,1,0"),
                       ("lfo/delay-60", "40,60,99,99,1,0"),
                       ("lfo/delay-99", "40,99,99,99,1,0")):
        if not want(label):
            continue
        a = ["--what", "lfo", "--lfo", lfo, "--dur", "4.0"]
        record(label, *cmp_trajectory(label, run(DEXED_DUMP, a), run(T00T_DUMP, a),
                                      tol_mae=0.05, tol_max=0.25, unit="",
                                      verbose=args.verbose, col=2))

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
