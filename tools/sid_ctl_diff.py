#!/usr/bin/env python3
"""Exact control-plane conformance: t00t's chip primitives against reSID.

The FM module's attempt 1 failed because every check ran through ears on
hardware, and a composite sound cannot tell you which of a dozen chained curves
is wrong (history_fm.md §1). Attempt 2's fix, adopted here from the first commit
rather than retrofitted: split verification into a control plane compared
*exactly* and a signal plane compared spectrally, because a spectral score can
go green with two errors cancelling and an exact diff cannot.

Most of a SID voice lands on the exact side. The envelope is a two-counter
state machine, the noise register is a deterministic bit sequence, the waveform
logic is combinational, and the DAC tables are tables. Only the filter and the
assembled sound genuinely need spectra (tools/sid_compare.py).

    tools/sid_ref/.venv/bin/python tools/sid_ctl_diff.py
    tools/sid_ref/.venv/bin/python tools/sid_ctl_diff.py --only lfsr --verbose

Exit status is non-zero if any gated domain fails, so this is usable as a
commit gate rather than as something to read.
"""

import argparse
import os
import subprocess
import sys

REF = "tools/sid_ref/resid_dump"
T00T = "tools/host_render/t00t_chip_dump"

# Master clock, for expressing envelope timing errors in samples rather than in
# cycles. The two engines run on different clocks and the *only* honest unit
# for their disagreement is time.
CLOCK_HZ = 985248.0
RATE_HZ = 44100.0
CYCLES_PER_SAMPLE = CLOCK_HZ / RATE_HZ


def run(binary, domain, model, count=None):
    if not os.path.exists(binary):
        sys.exit(f"{binary} not found -- build it first "
                 f"(tools/sid_ref: make; tools/host_render: make -f Makefile.chip)")
    cmd = [binary, "--domain", domain, "--model", model]
    if count is not None:
        cmd += ["--count", str(count)]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        rows.append(line.split(","))
    return rows


class Result:
    def __init__(self, name, gated=True):
        self.name = name
        self.gated = gated
        self.lines = []
        self.ok = True

    def fail(self, msg):
        self.ok = False
        self.lines.append(msg)

    def note(self, msg):
        self.lines.append(msg)


# ---------------------------------------------------------------------------

def diff_env(model, verbose):
    """Envelope trajectories.

    Compared as "at what time does the counter first reach level k", which is
    the same question on both clocks. The tolerance is a timing one: reSID is
    cycle-exact and t00t quantises envelope steps to the 44.1 kHz sample grid,
    so a correct implementation is expected to differ by up to about one
    sample per step, and by a fixed *relative* amount over a whole segment if
    a rate is wrong.

    Both halves are reported, because they fail differently: a quantisation
    difference is bounded in samples and does not grow, and a wrong rate table
    entry is bounded in percent and does grow.
    """
    r = Result("env")
    ref = {(a, int(b), int(c)): int(d) for a, b, c, d in run(REF, "env", model)}
    t00 = {(a, int(b), int(c)): int(d) for a, b, c, d in run(T00T, "env", model)}

    missing = set(ref) - set(t00)
    extra = set(t00) - set(ref)
    if missing:
        r.fail(f"t00t never reached {len(missing)} of {len(ref)} reference levels "
               f"(e.g. {sorted(missing)[:3]})")
    if extra:
        r.note(f"t00t reported {len(extra)} levels the reference did not")

    # A point passes if it is within EITHER bound, because the two failure
    # modes are different shapes and each bound is blind to one of them:
    #
    #   * sample-grid quantisation gives a bounded *absolute* error. At the
    #     fastest rate the envelope steps 2.5 times per sample, so a correct
    #     implementation can be a couple of samples out and never more.
    #   * a wrong rate table entry, exponential threshold or segment divider
    #     gives a bounded *relative* error -- and one that grows without limit
    #     in absolute terms as the trajectory gets longer.
    #
    # Gating on absolute alone flags the 24-second decay at rate 15, where 169
    # samples of accumulated rounding is 0.016% and inaudible. Gating on
    # relative alone flags the third step of a fast attack, where one sample of
    # quantisation is 5% of an elapsed 0.45 ms. Neither is a defect; requiring
    # both to be exceeded is what makes the gate mean "the shape is wrong".
    ABS_SAMPLES = 3.0
    REL = 0.005

    worst_rel, worst_rel_key = 0.0, None
    worst_abs, worst_abs_key = 0.0, None
    violations = []
    for key in sorted(set(ref) & set(t00)):
        d = abs(t00[key] - ref[key])
        samples = d / CYCLES_PER_SAMPLE
        rel = d / ref[key] if ref[key] > 0 else 0.0
        if samples > worst_abs:
            worst_abs, worst_abs_key = samples, key
        if rel > worst_rel and ref[key] > 20 * CYCLES_PER_SAMPLE:
            worst_rel, worst_rel_key = rel, key
        if samples > ABS_SAMPLES and rel > REL:
            violations.append((key, samples, rel))

    r.note(f"worst absolute error {worst_abs:.2f} samples at {worst_abs_key}")
    r.note(f"worst relative error {worst_rel * 100:.3f}% at {worst_rel_key}")
    r.note(f"{len(violations)} of {len(set(ref) & set(t00))} points exceed both "
           f"{ABS_SAMPLES} samples and {REL * 100:.1f}%")

    if violations:
        violations.sort(key=lambda v: -v[2])
        for key, samples, rel in violations[:5]:
            r.fail(f"{key}: {samples:.1f} samples / {rel * 100:.2f}% "
                   f"(reSID {ref[key]} cy, t00t {t00[key]} cy)")

    if verbose:
        for stage in ("attack", "decay", "release"):
            for rate in range(16):
                k = (stage, rate, 0 if stage != "attack" else 255)
                if k in ref and k in t00:
                    r.note(f"  {stage:8s} rate {rate:2d}: reSID {ref[k]:9d} cy, "
                           f"t00t {t00[k]:9d} cy ({(t00[k] - ref[k]) / CYCLES_PER_SAMPLE:+.1f} samples)")
    return r


def diff_lfsr(model, verbose, count):
    """The noise sequence. Bit-exact; there is no tolerance to argue about.

    This is the domain that catches module_chip.md §4.2's tap error. The doc lists
    eight taps (22/20/16/13/11/7/4/2); the reference feeds back bit22 ^ bit17
    and those eight positions are the output scatter. A wrong tap pair gives a
    different sequence with a different spectrum, and nothing but a listening
    test would have found it.
    """
    r = Result("lfsr")
    ref = run(REF, "lfsr", model, count)
    t00 = run(T00T, "lfsr", model, count)
    n = min(len(ref), len(t00))
    bad = [i for i in range(n) if ref[i][1] != t00[i][1]]
    r.note(f"{n} shifts compared, {len(bad)} mismatches")
    if bad:
        r.fail(f"first mismatch at index {bad[0]}: reSID {ref[bad[0]][1]}, t00t {t00[bad[0]][1]}")
        if verbose:
            for i in bad[:10]:
                r.note(f"  [{i}] reSID {ref[i][1]:>5s}  t00t {t00[i][1]:>5s}")
    return r


def diff_wave(model, verbose):
    """Waveform logic over all 4096 accumulator phases.

    Split into two verdicts on purpose:

      * pure waveforms (triangle, sawtooth, pulse, and ring's MSB
        substitution) are combinational logic and must match exactly;
      * combined waveforms are a nonlinear analog artifact that reSID models
        with tables sampled from real chips, and module_chip.md §13.5 deliberately
        defers t00t to a bitwise AND until P6.

    The second is therefore reported, not gated. Reporting it is the point:
    "roughly TinySID grade" is an adjective, and this turns it into a number
    that P6 can be argued from.
    """
    r = Result("wave")
    ref = {(int(a), int(b), int(c)): int(d) for a, b, c, d in run(REF, "wave", model)}
    t00 = {(int(a), int(b), int(c)): int(d) for a, b, c, d in run(T00T, "wave", model)}

    PURE = {1: "triangle", 2: "sawtooth", 4: "pulse"}
    for wf in sorted({k[0] for k in ref}):
        for ring in (0, 1):
            keys = [k for k in ref if k[0] == wf and k[1] == ring and k in t00]
            if not keys:
                continue
            errs = [abs(ref[k] - t00[k]) for k in keys]
            mism = sum(1 for e in errs if e)
            label = PURE.get(wf, "combined")
            line = (f"  wf {wf} ring {ring} ({label:9s}): {mism:4d}/{len(keys)} differ, "
                    f"mean |err| {sum(errs) / len(errs):7.1f}, max {max(errs):4d}")
            if wf in PURE:
                if mism:
                    r.fail(line.strip() + "  <- pure waveform must be exact")
                elif verbose:
                    r.note(line)
            else:
                r.note(line + "   (AND approximation, module_chip.md §13.5 -- not gated)")
    return r


def diff_dac(model, verbose):
    """The R-2R DAC tables as compiled into the firmware.

    Diffed against reSID's dac.h rather than merely regenerated from it, so
    the check covers the generated header actually in the tree -- an edit, a
    stale file or a truncated table all show up here.
    """
    r = Result("dac")
    ref = {(int(a), int(b)): int(c) for a, b, c in run(REF, "dac", model)}
    t00 = {(int(a), int(b)): int(c) for a, b, c in run(T00T, "dac", model)}
    for bits in (8, 12):
        keys = [k for k in ref if k[0] == bits and k in t00]
        bad = [k for k in keys if ref[k] != t00[k]]
        r.note(f"  {bits}-bit DAC: {len(bad)}/{len(keys)} differ")
        if bad:
            # The 8580's 12-bit waveform DAC is linear and t00t stores no table
            # for it, emitting the identity instead; that is not a mismatch to
            # gate on unless the reference disagrees with the identity.
            r.fail(f"{bits}-bit DAC table differs at {len(bad)} entries "
                   f"(first {bad[0]}: reSID {ref[bad[0]]}, t00t {t00[bad[0]]})")
            if verbose:
                for k in bad[:10]:
                    r.note(f"    [{k[1]}] reSID {ref[k]:5d}  t00t {t00[k]:5d}")
    return r


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["env", "lfsr", "wave", "dac"], default=None)
    ap.add_argument("--model", default="6581", choices=["6581", "8580"])
    ap.add_argument("--count", type=int, default=200000,
                    help="LFSR shifts to compare (default 200000; the full "
                         "period is 2^23-1 and takes about a minute)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    domains = [args.only] if args.only else ["env", "lfsr", "wave", "dac"]
    results = []
    for d in domains:
        if d == "env":
            results.append(diff_env(args.model, args.verbose))
        elif d == "lfsr":
            results.append(diff_lfsr(args.model, args.verbose, args.count))
        elif d == "wave":
            results.append(diff_wave(args.model, args.verbose))
        elif d == "dac":
            results.append(diff_dac(args.model, args.verbose))

    failed = 0
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        print(f"[{status}] {r.name}")
        for line in r.lines:
            print(("       " + line) if not line.startswith(" ") else "     " + line)
        if not r.ok:
            failed += 1
    print()
    print(f"{len(results) - failed}/{len(results)} domains pass (model {args.model})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
