#!/usr/bin/env python3
"""opl_compare -- scores t00t's OPL engine against a Nuked-OPL3 reference render.

The signal-plane half of module_opl.md's Future/TODO ("fm_regress.py-equivalent
spectral/perceptual regression"), for the OPL module. Renders the same patch
through both tools/opl_ref/nuked_render and tools/host_render/render_opl_patch
and scores them with tools/fm_compare.py's own comparator -- that scorer is
generic over any two mono-normalisable WAVs of a given note, so it is reused
here unmodified rather than re-implemented (nfft/hop/harmonic-tracking are
appropriate to OPL's own note range too, the same reasoning tools/sid_compare.py
already gets reused unmodified for the AY module).

Patch registers come from `render_opl_patch --list`'s CSV (mult/ksl/tl/ar/dr/
sl/rr/egt/ksr/ws per operator, feedback, algorithm), not a hand-copied Python
table -- src/engines/opl/patches.h stays the one place patch data is defined.

Usage:
    opl_compare.py --patch 0 --note 57 --vel 127
    opl_compare.py --all --note 60                  # every patch in patches.h
    opl_compare.py REF.wav TEST.wav --note 48        # score an existing pair
"""

import argparse
import csv
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
sys.path.insert(0, str(REPO))
import fm_compare  # noqa: E402  -- reused compare()/print_report()/read_wav_f32

NUKED_RENDER = REPO / "opl_ref" / "nuked_render"
T00T_RENDER = REPO / "host_render" / "build" / "render_opl_patch"


def list_patches():
    if not T00T_RENDER.exists():
        sys.exit(f"error: {T00T_RENDER} not built -- see tools/opl_ref/README.md / `make host`")
    r = subprocess.run([str(T00T_RENDER), "--list"], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: {T00T_RENDER} --list failed:\n{r.stderr}")
    lines = [l for l in r.stdout.splitlines() if l and not l.startswith("#")]
    return list(csv.reader(lines))


def op_args(row, lo, hi):
    """mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws -- nuked_render's own --op0/--op1 order."""
    return ",".join(row[lo:hi])


def render_pair(patch_idx, row, note, vel, gate, tail, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    ref = outdir / f"ref_p{patch_idx:02d}_n{note}v{vel}.wav"
    test = outdir / f"t00t_p{patch_idx:02d}_n{note}v{vel}.wav"

    op0 = op_args(row, 2, 12)
    op1 = op_args(row, 12, 22)
    feedback, algo = row[22], row[23]

    for exe in (NUKED_RENDER, T00T_RENDER):
        if not exe.exists():
            sys.exit(f"error: {exe} not built -- see tools/opl_ref/README.md / `make host`")

    common = ["--note", str(note), "--vel", str(vel), "--gate", str(gate), "--tail", str(tail)]
    r = subprocess.run([str(NUKED_RENDER), "--op0", op0, "--op1", op1, "--feedback", feedback,
                        "--algo", algo, *common, "--out", str(ref)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: nuked_render failed for patch {patch_idx}:\n{r.stderr}")

    r = subprocess.run([str(T00T_RENDER), "--patch", str(patch_idx), *common, "--out", str(test)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: render_opl_patch failed for patch {patch_idx}:\n{r.stderr}")
    return ref, test


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wavs", nargs="*", help="REF.wav TEST.wav (omit when using --patch/--all)")
    p.add_argument("--patch", type=int, help="patch index (see render_opl_patch --list)")
    p.add_argument("--all", action="store_true", help="sweep every patch in patches.h")
    p.add_argument("--note", type=int, default=57, help="MIDI note (default 57 = A3)")
    p.add_argument("--vel", type=int, default=127)
    p.add_argument("--gate", type=float, default=1.0)
    p.add_argument("--tail", type=float, default=2.0)
    p.add_argument("--outdir", default="tools/opl_ref/out", help="where rendered pairs go")
    p.add_argument("--json", help="write the full scorecard here")
    p.add_argument("--quiet", action="store_true", help="summary table only")
    args = p.parse_args()

    results = []
    if args.patch is not None or args.all:
        rows = list_patches()
        indices = range(len(rows)) if args.all else [args.patch]
        for i in indices:
            if i < 0 or i >= len(rows):
                sys.exit(f"error: --patch {i} out of range (patches.h holds {len(rows)} patches)")
            row = rows[i]
            ref, test = render_pair(i, row, args.note, args.vel, args.gate, args.tail,
                                    Path(args.outdir))
            r = fm_compare.compare(ref, test, args.note, args.gate, label=f"{i:02d} {row[1]}")
            results.append(r)
            if not args.quiet:
                fm_compare.print_report(r, verbose=False)
    else:
        if len(args.wavs) != 2:
            p.error("give REF.wav TEST.wav, or use --patch/--all")
        r = fm_compare.compare(args.wavs[0], args.wavs[1], args.note, args.gate,
                               label=f"{Path(args.wavs[0]).name} vs {Path(args.wavs[1]).name}")
        results.append(r)
        fm_compare.print_report(r)

    if len(results) > 1:
        print("\n=== summary ===")
        print(f"{'patch':<16} {'harm MAE':>9} {'atkMAE':>7} {'env MAE':>8} {'cent x':>7} {'harm%':>6} {'level':>8}")
        for r in results:
            cov = r["harmonic_coverage"]
            centroid = f"{r['centroid_ratio']:.2f}" + ("" if cov >= 0.3 else "*")
            print(f"{r['label']:<16} {r['harmonic_mae_db']:>9.1f} {r['attack_harmonic_mae_db']:>7.1f} "
                  f"{r['env_mae_db']:>8.1f} {centroid:>7} "
                  f"{cov * 100:>5.0f}% {fm_compare.fmt(r['level_gap_db'], '+.1f'):>8}")
        agg = lambda k, rs=results: sum(r[k] for r in rs) / len(rs)
        print(f"{'MEAN':<16} {agg('harmonic_mae_db'):>9.1f} {agg('attack_harmonic_mae_db'):>7.1f} "
              f"{agg('env_mae_db'):>8.1f}")

    if args.json:
        import json
        Path(args.json).write_text(json.dumps(results, indent=2))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
