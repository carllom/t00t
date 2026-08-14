#!/usr/bin/env python3
"""fm_regress -- the committed timbral regression gate for the FM engine (F7).

This is history_fm.md §6, the recommendation the whole rewrite was organised around:

    "The original problem was never the DSP -- it was that the feedback loop ran
    through your ears and your memory, so nothing was ever pinned down and every
    fix could silently regress an earlier one. A checked-in threshold file makes
    timbral regression a failing command instead of a listening session."

`--update` sweeps every factory bank against Dexed and writes what it measured
into fm_thresholds.json. Plain `fm_regress.py` re-runs the same sweep and fails
if anything got worse. The thresholds are *measurements with headroom*, not
aspirations: they record where the engine actually is, so the gate answers "did
this change make something worse?" rather than "is this good enough?", which is
the question a regression suite can actually answer.

    tools/fm_ref/.venv/bin/python tools/fm_regress.py            # check
    tools/fm_ref/.venv/bin/python tools/fm_regress.py --update   # re-baseline
    tools/fm_regress.py --only rom1a                             # one bank

Both sides render from the same .syx, so the converter is under test too, not
just the engine. Nothing here needs hardware -- that is the point of it.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BANKS = REPO / "tools" / "fm_ref" / "banks"
COMPARE = REPO / "tools" / "fm_compare.py"
SYX2PATCH = REPO / "tools" / "syx2patch.py"
PATCHES_H = REPO / "src" / "engines" / "fm" / "patches.h"
THRESHOLDS = REPO / "tools" / "fm_thresholds.json"

# The factory ROM banks, plus the synthetic conformance banks whose whole
# purpose is to isolate one parameter (history_fm.md §5.11, §5.19). The synthetic ones
# matter here because a bank average can absorb a single-parameter regression
# that a bank built to expose that parameter cannot.
BANK_NAMES = ["rom1a", "rom1b", "rom2a", "rom2b", "rom3a", "rom3b", "rom4a", "rom4b",
              "fb_depth", "fb_depth_mod"]

# Notes and velocities. Three octaves apart, because key level scaling, key rate
# scaling and detune are all note-dependent and a C3-only sweep cannot see any
# of them -- F5 (history_fm.md §5.12) added multi-note coverage for exactly that
# reason. The second velocity catches velocity-sensitivity errors, which scale
# the modulators hardest and so show up as timbre rather than level.
CONFIGS = [(36, 100), (48, 100), (60, 100), (72, 100), (48, 40)]

# The three metrics worth gating on, and why these three:
#   harm  -- steady-state timbre, the "does it sound like a DX7" number
#   atk   -- the first 100 ms, envelope-independent; attacks are where FM
#            patches are recognisable and where F0's worst errors lived
#   env   -- amplitude over time, the axis a spectral score cannot see
# Level gap and centroid are reported by fm_compare but not gated: centroid is
# meaningless on the inharmonic patches (history_fm.md §5.9) and level gap is already
# implied by env.
METRICS = ["harmonic_mae_db", "attack_harmonic_mae_db", "env_mae_db"]

# Headroom over the measured value, so ordinary numerical drift (a compiler
# change, a different libm) does not fail the gate while a real regression does.
# Absolute floor as well as a ratio: without it a metric that measures 0.02 dB
# today would gate at 0.026 dB and fail on noise.
TOL_RATIO = 1.30
TOL_FLOOR = 0.35


def convert(bank: Path) -> None:
    r = subprocess.run([sys.executable, str(SYX2PATCH), "convert", str(bank), str(PATCHES_H)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: syx2patch failed on {bank.name}:\n{r.stdout}{r.stderr}")


def build() -> None:
    r = subprocess.run(["make", "-s", "-f", "Makefile.fm", "render_fm_patch"],
                       cwd=REPO / "tools" / "host_render", capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: host build failed:\n{r.stdout}{r.stderr}")


def score(bank: Path, note: int, vel: int):
    out = REPO / "tools" / "fm_ref" / "out" / f"regress_{bank.stem}_{note}_{vel}.json"
    r = subprocess.run([sys.executable, str(COMPARE), "--bank", str(bank), "--voices", "0-31",
                        "--note", str(note), "--vel", str(vel), "--gate", "2", "--quiet",
                        "--json", str(out)], capture_output=True, text=True, cwd=REPO)
    if r.returncode != 0:
        sys.exit(f"error: fm_compare failed on {bank.name} note {note} vel {vel}:\n{r.stderr}")
    rows = json.loads(out.read_text())
    if isinstance(rows, dict):
        rows = rows["patches"]
    return rows


def aggregate(rows):
    """Bank-level mean and worst-single-patch, per metric.

    Both, deliberately. A mean alone lets one patch fall apart while 31 improve;
    a worst-case alone fails the gate every time a single inharmonic patch's
    tracker wobbles. Requiring both to hold is what makes the gate specific.
    """
    agg = {}
    for m in METRICS:
        vals = [r[m] for r in rows]
        agg[m] = {"mean": round(sum(vals) / len(vals), 3), "worst": round(max(vals), 3)}
    return agg


def sweep(names, verbose):
    result = {}
    for name in names:
        bank = BANKS / f"{name}.syx"
        if not bank.exists():
            print(f"  skip {name}: {bank} not present "
                  f"(fetch_banks.sh for ROM banks, make_fb_bank.py for synthetic)")
            continue
        convert(bank)
        build()
        for note, vel in CONFIGS:
            rows = score(bank, note, vel)
            key = f"{name}/n{note}v{vel}"
            result[key] = aggregate(rows)
            if verbose:
                a = result[key]
                print(f"  {key:24} harm {a['harmonic_mae_db']['mean']:5.2f}/"
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
    p.add_argument("--only", help="substring filter on bank name")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args(argv)

    names = [n for n in BANK_NAMES if not a.only or a.only in n]
    if not names:
        sys.exit(f"error: --only {a.only!r} matched no bank")

    print(f"sweeping {len(names)} bank(s) x {len(CONFIGS)} note/velocity configs "
          f"x 32 voices against Dexed...")
    measured = sweep(names, not a.quiet)
    if not measured:
        sys.exit("error: no banks scored")

    if a.update:
        prev = json.loads(THRESHOLDS.read_text()) if THRESHOLDS.exists() else {"limits": {}}
        prev.setdefault("limits", {})
        prev["_comment"] = (
            "Committed timbral regression baseline (history_fm.md §6). Generated by "
            "tools/fm_regress.py --update. `measured` is what the engine scored; "
            "`limits` is what CI fails above. Do not hand-edit: re-baseline, and "
            "say in the commit message why a number got worse."
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
        print(f"[FAIL] {key:24} {m} ({kind}) {got:.2f} dB > {cap:.2f} dB")
    for key in unchecked:
        print(f"[NEW ] {key:24} no committed threshold -- run --update to adopt it")
    if failures:
        print(f"\n{len(failures)} regression(s) against {THRESHOLDS.relative_to(REPO)}")
        return 1
    print(f"OK: {len(measured)} configs within committed thresholds"
          + (f", {len(unchecked)} new" if unchecked else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
