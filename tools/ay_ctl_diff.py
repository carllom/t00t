#!/usr/bin/env python3
"""Exact control-plane conformance: t00t's AY-3-8910/YM2149 primitives
against ayumi.

Mirrors tools/sid_ctl_diff.py's role (module_chip.md §11.1's control-plane/signal-
plane split) for the second chip in this module. Unlike SID's envelope,
AY's is a plain ramp counter with no piecewise-exponential segments, so
every domain here is bit-exact -- there is no sample-quantisation tolerance
to argue about the way tools/sid_ctl_diff.py's `env` domain needs one.

    tools/ay_ref/.venv/bin/python tools/ay_ctl_diff.py
    tools/ay_ref/.venv/bin/python tools/ay_ctl_diff.py --only noise --verbose

Exit status is non-zero if any domain fails, so this is usable as a commit
gate rather than as something to read.
"""

import argparse
import os
import subprocess
import sys

REF = "tools/ay_ref/ayumi_dump"
T00T = "tools/host_render/t00t_ay_dump"


def run(binary, domain):
    if not os.path.exists(binary):
        sys.exit(f"{binary} not found -- build it first "
                 f"(tools/ay_ref: make; tools/host_render: make -f Makefile.ay)")
    cmd = [binary, "--domain", domain]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        rows.append(line.split(","))
    return rows


class Result:
    def __init__(self, name):
        self.name = name
        self.lines = []
        self.ok = True

    def fail(self, msg):
        self.ok = False
        self.lines.append(msg)

    def note(self, msg):
        self.lines.append(msg)


def diff_tone(verbose):
    """Per-period toggle sequence. Combinational counter logic -- exact."""
    r = Result("tone")
    ref = {(int(a), int(b)): int(c) for a, b, c in run(REF, "tone")}
    t00 = {(int(a), int(b)): int(c) for a, b, c in run(T00T, "tone")}
    n = min(len(ref), len(t00))
    bad = [k for k in sorted(set(ref) & set(t00)) if ref[k] != t00[k]]
    r.note(f"{n} ticks compared across {len({k[0] for k in ref})} periods, {len(bad)} mismatches")
    if bad:
        k = bad[0]
        r.fail(f"first mismatch at period={k[0]} tick={k[1]}: ayumi {ref[k]}, t00t {t00[k]}")
        if verbose:
            for k in bad[:10]:
                r.note(f"  period={k[0]} tick={k[1]}: ayumi {ref[k]}  t00t {t00[k]}")
    return r


def diff_noise(verbose):
    """The shared 17-bit LFSR's output-bit sequence. Bit-exact.

    This is the domain that would catch a wrong tap pair the way
    tools/sid_ctl_diff.py's lfsr domain catches SID's -- a wrong feedback
    polynomial gives a different sequence with a different spectrum, and
    nothing but a listening test would otherwise have found it.
    """
    r = Result("noise")
    ref = run(REF, "noise")
    t00 = run(T00T, "noise")
    n = min(len(ref), len(t00))
    bad = [i for i in range(n) if ref[i][1] != t00[i][1]]
    r.note(f"{n} shifts compared, {len(bad)} mismatches")
    if bad:
        r.fail(f"first mismatch at index {bad[0]}: ayumi {ref[bad[0]][1]}, t00t {t00[bad[0]][1]}")
        if verbose:
            for i in bad[:10]:
                r.note(f"  [{i}] ayumi {ref[i][1]}  t00t {t00[i][1]}")
    return r


def diff_envelope(verbose):
    """The (shape, tick) -> level ramp, all 16 raw shape codes. Bit-exact."""
    r = Result("envelope")
    ref = {(int(a), int(b)): int(c) for a, b, c in run(REF, "envelope")}
    t00 = {(int(a), int(b)): int(c) for a, b, c in run(T00T, "envelope")}
    bad = [k for k in sorted(set(ref) & set(t00)) if ref[k] != t00[k]]
    r.note(f"{len(ref)} points compared across 16 shapes, {len(bad)} mismatches")
    if bad:
        k = bad[0]
        r.fail(f"first mismatch at shape={k[0]} tick={k[1]}: ayumi {ref[k]}, t00t {t00[k]}")
        if verbose:
            for k in bad[:10]:
                r.note(f"  shape={k[0]} tick={k[1]}: ayumi {ref[k]}  t00t {t00[k]}")
    return r


def diff_dac(verbose):
    """The AY/YM DAC tables, as compiled into src/chip/ay_envelope.h.

    Diffed against ayumi.c's own tables rather than merely regenerated from
    them, so the check covers the header actually in the tree -- a typo in
    the independently re-typed float literals shows up here, not by ear.

    Tolerance, not exact: ayumi.c's tables are `double`; ay_envelope.h's are
    `float` (deliberately -- the DAC output feeds a real-time mix on an
    embedded target, same choice speaker_sim.h already made). float32's own
    precision is ~1.2e-7 relative; 1e-6 absolute is comfortably above that
    noise floor while still catching an actual mistyped digit, which is
    orders of magnitude larger.
    """
    r = Result("dac")
    ref = {(a, int(b)): float(c) for a, b, c in run(REF, "dac")}
    t00 = {(a, int(b)): float(c) for a, b, c in run(T00T, "dac")}
    for model in ("ay", "ym"):
        keys = [k for k in ref if k[0] == model and k in t00]
        bad = [k for k in keys if abs(ref[k] - t00[k]) > 1e-6]
        r.note(f"  {model}: {len(bad)}/{len(keys)} differ")
        if bad:
            r.fail(f"{model} DAC table differs at {len(bad)} entries "
                   f"(first [{bad[0][1]}]: ayumi {ref[bad[0]]}, t00t {t00[bad[0]]})")
            if verbose:
                for k in bad[:10]:
                    r.note(f"    [{k[1]}] ayumi {ref[k]:.10g}  t00t {t00[k]:.10g}")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["tone", "noise", "envelope", "dac"], default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    domains = [args.only] if args.only else ["tone", "noise", "envelope", "dac"]
    results = []
    for d in domains:
        if d == "tone":
            results.append(diff_tone(args.verbose))
        elif d == "noise":
            results.append(diff_noise(args.verbose))
        elif d == "envelope":
            results.append(diff_envelope(args.verbose))
        elif d == "dac":
            results.append(diff_dac(args.verbose))

    failed = 0
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        print(f"[{status}] {r.name}")
        for line in r.lines:
            print(("       " + line) if not line.startswith(" ") else "     " + line)
        if not r.ok:
            failed += 1
    print()
    print(f"{len(results) - failed}/{len(results)} domains pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
