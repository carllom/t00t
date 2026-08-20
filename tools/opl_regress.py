#!/usr/bin/env python3
"""opl_regress -- the committed spectral/envelope regression gate for the OPL
engine, mirroring tools/fm_regress.py/fm_thresholds.json for the OPL module
(module_opl.md's Future/TODO; module_fm.md's own history_fm.md §6 describes
the FM-side gate this one copies the shape of).

`--update` sweeps every patch in src/engines/opl/patches.h against Nuked-OPL3
and writes what it measured into opl_thresholds.json. Plain `opl_regress.py`
re-runs the same sweep and fails if anything got worse. As with the FM gate,
the thresholds are measurements with headroom, not aspirations: they record
where the engine actually is, so CI answers "did this change make something
worse?", not "is this good enough?".

    python3 tools/opl_regress.py            # check
    python3 tools/opl_regress.py --update   # re-baseline
    python3 tools/opl_regress.py --only n60 # one note/velocity config

Runs after tools/opl_ctl_diff.py's control-plane conformance (#80) so this
baseline isn't locked in against curves already known to be wrong.

OPL has no bank converter (module_opl.md's Future/TODO) -- patches.h's five
hand-authored patches are the whole corpus, so unlike fm_regress.py there is
only one "bank" to sweep, and each config's row aggregates over those five
patches rather than over 32 voices pulled from a .syx.
"""

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import opl_compare  # noqa: E402

THRESHOLDS = REPO / "tools" / "opl_thresholds.json"

# Notes and velocities. Three-plus octaves apart, because key scale level,
# key scale rate and the velocity->TL folding (env_opl.h's env_opl_init())
# are all note/velocity-dependent and a single config cannot see any of
# them. The second velocity catches a velocity-sensitivity error, which
# env_opl_init() applies as a flat TL shift.
CONFIGS = [(36, 127), (48, 127), (60, 127), (72, 127), (48, 64)]

GATE_S, TAIL_S = 1.0, 1.5

# Same three metrics fm_regress.py gates on, and why: harmonic MAE is
# steady-state timbre, attack MAE is the first 100 ms (envelope-independent,
# where an inharmonic percussive patch's tracker is least likely to have
# already gone quiet on one side), env MAE is amplitude over time -- the axis
# a spectral score cannot see.
METRICS = ["harmonic_mae_db", "attack_harmonic_mae_db", "env_mae_db"]

# Headroom over the measured value, same rationale and same numbers as
# fm_thresholds.json: ordinary numerical drift should not fail the gate,
# only a real regression should. Absolute floor as well as a ratio, so a
# metric measuring near 0 dB today does not gate at noise.
TOL_RATIO = 1.30
TOL_FLOOR = 0.35


def aggregate(rows):
    """Mean and worst-single-patch, per metric, across this config's five
    patches -- both, deliberately, for the same reason fm_regress.py's own
    aggregate() takes both: a mean alone lets one patch fall apart while the
    others improve, a worst-case alone fails on a single patch's wobble."""
    agg = {}
    for m in METRICS:
        vals = [r[m] for r in rows]
        agg[m] = {"mean": round(sum(vals) / len(vals), 3), "worst": round(max(vals), 3)}
    return agg


def sweep(configs, verbose):
    result = {}
    patch_rows = opl_compare.list_patches()
    for note, vel in configs:
        rows = []
        for i, row in enumerate(patch_rows):
            ref, test = opl_compare.render_pair(i, row, note, vel, GATE_S, TAIL_S,
                                                REPO / "tools" / "opl_ref" / "out")
            rows.append(opl_compare.fm_compare.compare(ref, test, note, GATE_S,
                                                        label=f"{i:02d} {row[1]}"))
        key = f"n{note}v{vel}"
        result[key] = aggregate(rows)
        if verbose:
            a = result[key]
            print(f"  {key:12} harm {a['harmonic_mae_db']['mean']:5.2f}/"
                  f"{a['harmonic_mae_db']['worst']:5.2f}  "
                  f"atk {a['attack_harmonic_mae_db']['mean']:5.2f}/"
                  f"{a['attack_harmonic_mae_db']['worst']:5.2f}  "
                  f"env {a['env_mae_db']['mean']:5.2f}/{a['env_mae_db']['worst']:5.2f}")
    return result


def limit(v):
    return round(max(v * TOL_RATIO, v + TOL_FLOOR), 3)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--update", action="store_true", help="re-baseline instead of checking")
    p.add_argument("--only", help="substring filter on config key, e.g. n60 or v64")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args(argv)

    configs = [(n, v) for n, v in CONFIGS if not a.only or a.only in f"n{n}v{v}"]
    if not configs:
        sys.exit(f"error: --only {a.only!r} matched no config")

    print(f"sweeping {len(configs)} note/velocity config(s) x 5 patches against Nuked-OPL3...")
    measured = sweep(configs, not a.quiet)
    if not measured:
        sys.exit("error: no configs scored")

    if a.update:
        prev = json.loads(THRESHOLDS.read_text()) if THRESHOLDS.exists() else {"limits": {}}
        prev.setdefault("limits", {})
        prev["_comment"] = (
            "Committed spectral/envelope regression baseline (module_opl.md's "
            "Future/TODO). Generated by tools/opl_regress.py --update. `measured` "
            "is what the engine scored; `limits` is what CI fails above. Do not "
            "hand-edit: re-baseline, and say in the commit message why a number "
            "got worse."
        )
        prev["tolerance"] = {"ratio": TOL_RATIO, "floor_db": TOL_FLOOR}
        prev["measured"] = measured
        for key, agg in measured.items():
            prev["limits"][key] = {m: {k: limit(v) for k, v in agg[m].items()} for m in METRICS}
        THRESHOLDS.write_text(json.dumps(prev, indent=2, sort_keys=True) + "\n")
        print(f"\nwrote {THRESHOLDS.relative_to(REPO)} ({len(measured)} configs)")
        return 0

    if not THRESHOLDS.exists():
        sys.exit(f"error: {THRESHOLDS} missing -- run with --update first")
    limits = json.loads(THRESHOLDS.read_text())["limits"]

    failures, unchecked = [], []
    for key, agg in measured.items():
        if key not in limits:
            unchecked.append(key)
            continue
        for m in METRICS:
            for kind in ("mean", "worst"):
                got, cap = agg[m][kind], limits[key][m][kind]
                if got > cap:
                    failures.append((key, m, kind, got, cap))

    print()
    for key, m, kind, got, cap in failures:
        print(f"[FAIL] {key:12} {m} ({kind}) {got:.2f} dB > {cap:.2f} dB")
    for key in unchecked:
        print(f"[NEW ] {key:12} no committed threshold -- run --update to adopt it")
    if failures:
        print(f"\n{len(failures)} regression(s) against {THRESHOLDS.relative_to(REPO)}")
        return 1
    print(f"OK: {len(measured)} configs within committed thresholds"
          + (f", {len(unchecked)} new" if unchecked else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
