#!/usr/bin/env python3
"""Fit t00t's SVF to reSID's measured filter response and emit src/chip/sid_tables.h.

module_chip.md §5.1 asks for "a pluggable cutoff LUT instead of svf_compute_f_half's
linear map", and warns:

    The 6581 cutoff curve varied enormously between physical chips. Any curve
    is *a* 6581, not *the* 6581. The LUT must be documented as sampled from a
    named reference (reSID's, or a specific chip), never presented as
    canonical.

This is the sampling. tools/sid_ref/resid_probe.cpp records reSID's response
over the cutoff and resonance grids; this script fits src/chip/sid_filter.h's
SVF to each grid point and writes the tables out with their provenance.

The fit is done against t00t's *actual* difference equation -- its exact
z-domain transfer function, derived from the recurrence rather than from an
idealised biquad. The SVF is a two-pass (2x-oversampled) Chamberlin topology
whose cutoff warps noticeably above a few kHz, so fitting an ideal response and
converting would put a systematic error into exactly the top octave where the
6581 curve is steepest. Deriving the real one costs nothing offline and removes
that class of error entirely.

Also emitted here, because they belong to the same header and have the same
"measured from reSID" provenance:

  * the 8-bit envelope and 12-bit waveform R-2R DAC tables, derived from the
    ladder's measured resistor ratio rather than copied from reSID (see
    r2r_vbit), which module_chip.md does not mention but §3's signal-path test keeps;
  * the C64 board output network coefficients (module_chip.md §10's "free bonus");
  * the filter saturation scale.

Usage:
    tools/sid_ref/.venv/bin/python tools/fit_6581_filter.py \\
        --probe-dir tools/sid_ref/out --out src/chip/sid_tables.h

numpy only, no scipy -- same constraint as the FM module's tooling.
"""

import argparse
import json
import os
import sys

import numpy as np

SAMPLE_RATE = 44100.0
NPER = 8192

# The band the fit cares about. Below 40 Hz the probe has little energy and the
# board's high-pass is in the way; above 16 kHz reSID's resampler is rolling off
# and t00t's SVF is past the point where a two-pass Chamberlin means anything.
FIT_LO_HZ = 40.0
FIT_HI_HZ = 16000.0

# Upper bound on the SVF's Q15 half-frequency coefficient.
#
# NOT src/filter.h's svf_compute_f_half clamp of 15564 ("F=0.95 maximum"),
# which is a single-pass Chamberlin stability figure. src/chip/sid_filter.h
# runs the loop twice per sample, and the two-pass system's spectral radius
# stays under 1 well past that: measured here, |eig(A^2)| < 1 up to F ~ 39950
# at the lightest damping in the resonance table and F ~ 27100 at the heaviest.
#
# Copying the 15564 clamp into the fit was a real bug and worth recording: it
# capped the fitted cutoff at about 4.5 kHz, so every register value from
# fc = 1360 upward -- a third of the range -- landed on the same table entry
# and the top of the filter sweep would have gone silent-flat instead of
# opening. 22000 is chosen instead as the point past which the lowpass no
# longer has a -3 dB corner below Nyquist at all, with the stability margin
# above it left unused.
SVF_F_MAX = 22000

# Voice-sum to int16 scale factor; see the generated header for the rationale.
SID_MIX_SHIFT = 7


# ---------------------------------------------------------------------------
# Probe analysis
# ---------------------------------------------------------------------------

def load_probe(path_f32, path_json):
    meta = json.load(open(path_json))
    n = meta["samples_per_run"]
    runs = meta["runs"]
    data = np.fromfile(path_f32, dtype=np.float32).reshape(len(runs), n)
    return meta, runs, data


def welch_psd(x):
    x = x.astype(np.float64)
    x = x - x.mean()
    w = np.hanning(NPER)
    step = NPER // 2
    nseg = (len(x) - NPER) // step + 1
    acc = np.zeros(NPER // 2 + 1)
    for k in range(nseg):
        acc += np.abs(np.fft.rfft(x[k * step:k * step + NPER] * w)) ** 2
    return acc / nseg


def responses(data, runs):
    """dB response of every non-bypass run, relative to the bypass run.

    Both runs contain the same deterministic noise sequence through the same
    source, mixer and output stage, so the ratio is the filter alone.
    """
    freqs = np.fft.rfftfreq(NPER, 1.0 / SAMPLE_RATE)
    bypass_idx = next(i for i, r in enumerate(runs) if r["bypass"])
    ref = welch_psd(data[bypass_idx])
    out = []
    for i, r in enumerate(runs):
        if r["bypass"]:
            continue
        h_db = 10.0 * np.log10(np.maximum(welch_psd(data[i]) / ref, 1e-12))
        out.append((r, h_db))
    return freqs, out


# ---------------------------------------------------------------------------
# t00t's SVF, as an exact transfer function of src/chip/sid_filter.h's loop
# ---------------------------------------------------------------------------

def svf_response(f_half_q15, q_q15, mode_mask, freqs):
    """Magnitude response in dB of the two-pass SVF at the given frequencies.

    Exact, not simulated. One pass of src/chip/sid_filter.h's inner loop

        hp = x - lp - q*bp;  bp += f*hp;  lp += f*bp

    is the linear map  s' = A s + B x  with s = (lp, bp),

        A = [[1 - f^2,  f(1 - f q)],      B = (f^2, f)
             [   -f,      1 - f q  ]]

    and tick() applies it twice per sample against a held input, so

        s[n+1] = A^2 s[n] + (A + I) B x[n]

    with lp/bp read off s[n+1] and hp read off the intermediate state
    s1 = A s[n] + B x[n]. Evaluating (zI - A^2)^-1 at z = e^{jw} gives the
    response directly.

    An impulse-response FFT was the first version and is ~1000x slower --
    enough to matter, since the fit evaluates this tens of thousands of times.
    """
    f = f_half_q15 / 32768.0
    q = q_q15 / 32768.0
    A = np.array([[1.0 - f * f, f * (1.0 - f * q)],
                  [-f,          1.0 - f * q]])
    B = np.array([f * f, f])
    A2 = A @ A
    N = (A + np.eye(2)) @ B

    w = 2.0 * np.pi * np.asarray(freqs, dtype=float) / SAMPLE_RATE
    z = np.exp(1j * w)

    # (zI - A^2)^-1 N, for every z at once (2x2 inverse in closed form).
    a, b = A2[0, 0], A2[0, 1]
    c, d = A2[1, 0], A2[1, 1]
    m00, m01 = z - a, -b
    m10, m11 = -c, z - d
    det = m00 * m11 - m01 * m10
    s0 = (m11 * N[0] - m01 * N[1]) / det
    s1 = (-m10 * N[0] + m00 * N[1]) / det

    # lp and bp are read from the post-update state s[n+1] = z*S(z), hp from
    # the intermediate state. On its own each output's magnitude is unaffected
    # by that unit delay, but the mode *mask* sums them, and a relative delay
    # between summed terms is not a delay -- it is a different filter. Getting
    # this wrong is worth up to 18 dB on LP+BP+HP, which is precisely the
    # combination module_chip.md §5.1 exists to support.
    out = np.zeros_like(z)
    if mode_mask & 0x1:
        out = out + z * s0
    if mode_mask & 0x2:
        out = out + z * s1
    if mode_mask & 0x4:
        # (s0, s1) above is the transform of the *start-of-sample* state, so
        # the intermediate state the second pass sees is A s + B x directly.
        # (lp and bp are read one update later, which is a unit delay and so
        # leaves their magnitudes alone -- hp is the only output where getting
        # this wrong shows up, and it showed up as 6.5 dB.)
        i0 = A[0, 0] * s0 + A[0, 1] * s1 + B[0]
        i1 = A[1, 0] * s0 + A[1, 1] * s1 + B[1]
        out = out + (1.0 - i0 - q * i1)

    return 20.0 * np.log10(np.maximum(np.abs(out), 1e-12))


# The fit is restricted to where the target is still meaningfully above the
# stopband. Matching 60 dB of rolloff to a fraction of a dB is worthless, and
# including it lets the least-squares trade real passband error for stopband
# error it cannot fix anyway (a 2-pole SVF and reSID's 6581 do not have the
# same asymptotic slope).
#
# The first version clamped the target at -40 dB instead, which is subtly
# fatal: a nearly-closed filter's response is *also* flat at the clamp, so
# after mean-subtraction it matched every wide-open target perfectly and the
# fit returned F_half = 20 -- the minimum -- for the entire top third of the
# cutoff range. Worth stating plainly, because the failure produced a
# monotonic-looking table that would have sounded like the filter was
# backwards above fc ~ 1300.
FIT_FLOOR_DB = -35.0


def _fit_band(target_db, freqs):
    band = (freqs >= FIT_LO_HZ) & (freqs <= FIT_HI_HZ) & (target_db > FIT_FLOOR_DB)
    if band.sum() < 16:
        band = (freqs >= FIT_LO_HZ) & (freqs <= FIT_HI_HZ)
    return band


def _err(f_half, q, mode_mask, fb, tb_centred, cache):
    key = (f_half, q, mode_mask)
    m = cache.get(key)
    if m is None:
        m = svf_response(f_half, q, mode_mask, fb)
        cache[key] = m
    return float(np.mean((m - m.mean() - tb_centred) ** 2))


def fit_f_half(target_db, freqs, q_q15, mode_mask):
    """Find the Q15 F_half whose SVF response best matches target_db.

    Error is dB-domain least squares, level-matched first (a constant gain
    offset is not a cutoff error). Coarse log sweep, then local refinement --
    the objective is smooth and unimodal in F over the fit band.
    """
    band = _fit_band(target_db, freqs)
    fb, tb = freqs[band], target_db[band]
    tbc = tb - tb.mean()
    cache = {}

    def e(f):
        return _err(int(f), q_q15, mode_mask, fb, tbc, cache)

    best = int(min(np.unique(np.round(np.geomspace(20, SVF_F_MAX, 130)).astype(int)), key=e))
    lo, hi = max(20, int(best * 0.8)), min(SVF_F_MAX, int(best * 1.25) + 2)
    best = int(min(range(lo, hi + 1, max(1, (hi - lo) // 120)), key=e))
    lo, hi = max(20, best - 40), min(SVF_F_MAX, best + 40)
    best = int(min(range(lo, hi + 1), key=e))
    return best, e(best)


def fit_fq(target_db, freqs, mode_mask, f_hint):
    """Jointly fit F and Q.

    Used for the resonance sweep: on the 6581 raising resonance does not leave
    the cutoff where it was, so fitting Q alone against a fixed F charges the
    Q table for a shift that belongs to F. The search is small -- Q over a
    coarse grid, F refined around the hint for each.
    """
    band = _fit_band(target_db, freqs)
    fb, tb = freqs[band], target_db[band]
    tbc = tb - tb.mean()
    cache = {}

    best = None
    for q in np.unique(np.round(np.geomspace(200, 65534, 60)).astype(int)):
        lo, hi = max(20, int(f_hint * 0.4)), min(SVF_F_MAX, int(f_hint * 2.5) + 2)
        step = max(1, (hi - lo) // 60)
        f = int(min(range(lo, hi + 1, step), key=lambda x: _err(x, int(q), mode_mask, fb, tbc, cache)))
        lo2, hi2 = max(20, f - step), min(SVF_F_MAX, f + step)
        f = int(min(range(lo2, hi2 + 1), key=lambda x: _err(x, int(q), mode_mask, fb, tbc, cache)))
        e = _err(f, int(q), mode_mask, fb, tbc, cache)
        if best is None or e < best[2]:
            best = (f, int(q), e)
    return best


# ---------------------------------------------------------------------------
# DAC tables, from the ladder's physical constants
#
# Derived here rather than dumped from reSID, for two reasons.
#
# Licensing: reSID is GPL-2 and this repo is not, which is the whole reason
# fetch_resid.sh fetches rather than vendors. An earlier version of this script
# shelled out to `resid_dump --domain dac` and committed 4352 entries that were
# a mechanical reproduction of reSID's own constexpr constructor -- which puts
# back exactly the question the fetch arrangement exists to keep out.
#
# Testing: with the tables copied from reSID, sid_ctl_diff.py's `dac` domain
# compared reSID's table against a copy of reSID's table. It reported 0/256 and
# 0/4096 because it could not do anything else. Deriving them independently
# turns that domain into a real conformance test.
#
# What is taken from reSID is four numbers, and they are facts about the
# hardware rather than code: the ladder's resistor ratio is 2R/R = 2.20 on the
# 6581 with the bit-0 termination missing, and 2.00 with correct termination on
# the 8580. Dag Lem obtained those by measurement; dac.h documents them, and
# they are what makes a 6581 sound like a 6581 at low envelope levels.
#
# The method here is deliberately not reSID's. dac.h walks the ladder with
# repeated parallel substitution and source transformation; this solves the
# resistor network directly by nodal analysis and uses superposition, which is
# a different algorithm arriving at the same physics. Agreement between the two
# is therefore evidence, not tautology -- and the topology below was in fact
# settled by that agreement: the 6581 matches either way (an unterminated
# ladder has no termination node to place), but the 8580 only matches with the
# termination 2R going straight to ground at the LSB node rather than through
# another rail resistor.
# ---------------------------------------------------------------------------

def r2r_vbit(bits, ratio_2r_over_r, terminated):
    """Output voltage contributed by each bit, driven alone to 1 V.

    Ladder topology (reSID's dac.h draws the same one):

        bit n-1   bit n-2         bit 1     bit 0
           |         |              |         |
          2R        2R             2R        2R
           |         |              |         |
    Vo o---+----R----+----R-...--R--+----R----+---+
                                                  |
                                                 2R   (8580 only)
                                                  |
                                                 GND

    Conductances are in units of 1/R, so the rail is 1.0 and each leg is
    1/(2R/R). The network is linear, so one solve per bit plus superposition
    gives every code -- 12 solves instead of 4096.
    """
    g_rail = 1.0
    g_leg = 1.0 / ratio_2r_over_r

    g = np.zeros((bits, bits))
    for j in range(bits):
        g[j, j] += g_leg                     # this bit's own 2R leg
    for j in range(bits - 1):                # rail resistor to the next bit node
        g[j, j] += g_rail
        g[j + 1, j + 1] += g_rail
        g[j, j + 1] -= g_rail
        g[j + 1, j] -= g_rail
    if terminated:
        g[0, 0] += g_leg                     # 2R from the LSB node to ground

    vbit = np.empty(bits)
    for k in range(bits):
        i = np.zeros(bits)
        i[k] = g_leg                         # bit k driven to 1 V through its 2R
        vbit[k] = np.linalg.solve(g, i)[bits - 1]   # output at the MSB node
    return vbit


def r2r_table(bits, ratio_2r_over_r, terminated):
    """The full 2**bits-entry DAC table, scaled to 0 .. 2**bits - 1."""
    vbit = r2r_vbit(bits, ratio_2r_over_r, terminated)
    codes = np.arange(1 << bits)
    set_bits = ((codes[:, None] >> np.arange(bits)) & 1).astype(float)
    vo = set_bits @ vbit
    table = np.floor(((1 << bits) - 1) * vo + 0.5).astype(int)

    # Structural checks, in place of a normalisation constant.
    #
    # Both of the obvious invariants are false for one of the two models, and
    # finding out which way round taught more about the hardware than the
    # tables themselves did:
    #
    #   "all-ones is full scale" holds only WITHOUT termination. Driving every
    #   bit high on an unterminated ladder leaves no current path anywhere, so
    #   the network sits at 1 V. With the 8580's 2R to ground that code does
    #   draw current and the top sags -- reSID's 8580 8-bit table ends at 254,
    #   and it is right to.
    #
    #   "a DAC table is monotonic" holds only WITH termination. The 6581's
    #   ladder is not merely imprecise, it is non-monotonic: 19 descending
    #   steps in the 8-bit table and 347 in the 12-bit, worst case -129,
    #   clustered on the major carries (15->16, 31->32, 63->64) where the
    #   2.20 ratio and the missing bit-0 termination compound. dac.h says as
    #   much -- "pronounced errors for the lower 4 - 5 bits (e.g. the output
    #   for bit 0 is actually equal to the output for bit 1), resulting in DAC
    #   discontinuities" -- and it is a large part of why a quiet 6581 note
    #   sounds dirty rather than merely quiet.
    #
    # Recorded here because the 6581 table looks broken and is not. Anyone who
    # later "fixes" it by sorting or smoothing will have removed the thing
    # module_chip.md §3 says to keep.
    assert table[0] == 0, "zero code must be zero"
    if terminated:
        assert np.all(np.diff(table) >= 0), "a terminated ladder is monotonic"
    else:
        assert table[-1] == (1 << bits) - 1, "unterminated all-ones must be full scale"
    return [int(x) for x in table]


# The measured ladder constants. 6581: 2R/R = 2.20, bit-0 termination missing.
# 8580: 2.00 with correct termination, i.e. effectively linear.
DAC_RATIO = {"6581": (2.20, False), "8580": (2.00, True)}


def make_dac(model):
    ratio, term = DAC_RATIO[model]
    return r2r_table(8, ratio, term), r2r_table(12, ratio, term)


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def fmt_table(name, ctype, values, per_line, comment):
    lines = [comment, f"static const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        chunk = ", ".join(str(v) for v in values[i:i + per_line])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def one_pole_q15(cutoff_hz):
    """Q15 coefficient for y += (x - y) * k, k = 1 - exp(-2*pi*fc/fs)."""
    k = 1.0 - np.exp(-2.0 * np.pi * cutoff_hz / SAMPLE_RATE)
    return int(round(k * 32768.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe-dir", default="tools/sid_ref/out")
    ap.add_argument("--sid-ref-dir", default="tools/sid_ref",
                    help="where fetch_resid.sh lives, for the pinned SHA "
                         "recorded in the generated header")
    ap.add_argument("--out", default="src/chip/sid_tables.h")
    ap.add_argument("--resid-sha", default=None,
                    help="override the pinned SHA recorded in the header")
    ap.add_argument("--refit", action="store_true",
                    help="ignore the cached fit and redo it (~5 minutes)")
    args = ap.parse_args()

    # The fit is the slow part and the emission is the part that gets iterated
    # on. Caching the fitted grids keeps "change a comment in the header" from
    # costing five minutes, and makes it obvious when a table changed because
    # the fit changed rather than because the formatter did.
    cache_path = os.path.join(args.probe_dir, "fit_cache.json")

    sha = args.resid_sha
    if sha is None:
        script = os.path.join(args.sid_ref_dir, "fetch_resid.sh")
        try:
            for line in open(script):
                if line.startswith("RESID_SHA="):
                    sha = line.split("=", 1)[1].strip()
                    break
        except OSError:
            pass
    sha = sha or "unknown"

    cached = None
    if not args.refit and os.path.exists(cache_path):
        cached = json.load(open(cache_path))
        print(f"  (reusing fit from {cache_path}; --refit to redo it)", file=sys.stderr)

    if cached is None:
        res_q, res_f, grid_fc, grid_f, q0, fc_ref = run_fit(args)
        json.dump({"res_q": res_q, "res_f": res_f, "grid_fc": grid_fc,
                   "grid_f": [int(x) for x in grid_f], "q0": int(q0), "fc_ref": int(fc_ref)},
                  open(cache_path, "w"))
    else:
        res_q, res_f = cached["res_q"], cached["res_f"]
        grid_fc, grid_f = cached["grid_fc"], cached["grid_f"]
        q0, fc_ref = cached["q0"], cached["fc_ref"]

    # Interpolate the measured grid up to all 2048 register values. The grid is
    # coarse on purpose (a 2048-point probe would take hours); the curve is
    # smooth, and doing the interpolation here means the device does a table
    # lookup and no arithmetic at all.
    fc_full = np.arange(2048)
    f_full = np.interp(fc_full, grid_fc, grid_f)
    f_full = np.clip(np.round(f_full), 20, SVF_F_MAX).astype(int)

    emit(args, sha, res_q, res_f, f_full, fc_ref)


def run_fit(args):
    """The expensive half: fit F and Q against the probe recordings."""
    #
    # Ordered before the cutoff sweep on purpose. The cutoff fit needs a
    # damping value to hold fixed, and the right one is whatever the 6581 does
    # at res = 0 -- which is not a round number and not critical damping. An
    # earlier version guessed 45000 and fitted the whole cutoff curve against a
    # damping the chip never uses; every entry then carried that guess's error.
    # F and Q are fitted jointly here because raising resonance on a 6581 also
    # moves its cutoff, and charging the Q table for that shift would bake a
    # cutoff error into the resonance knob.
    rmeta, rruns, rdata = load_probe(os.path.join(args.probe_dir, "probe_res_lp.f32"),
                                     os.path.join(args.probe_dir, "probe_res_lp.json"))
    rfreqs, rresp = responses(rdata, rruns)
    fc_ref = rruns[1]["fc"]
    res_q, res_f = [], []
    hint = 7000
    for r, h_db in rresp:
        f, q, e = fit_fq(h_db, rfreqs, 0x1, hint)
        res_q.append(q)
        res_f.append(f)
        hint = f
        print(f"  res={r['res']:2d} -> Q={q:6d}  (F={f:5d}, rms {np.sqrt(e):.2f} dB)",
              file=sys.stderr)
    q0 = res_q[0]
    print(f"  (res-0 damping Q={q0} is the reference for the cutoff sweep;"
          f" res-sweep F drift {min(res_f)}..{max(res_f)} at fc={fc_ref})", file=sys.stderr)

    # --- cutoff sweep -------------------------------------------------------
    meta, runs, data = load_probe(os.path.join(args.probe_dir, "probe_fc_lp.f32"),
                                  os.path.join(args.probe_dir, "probe_fc_lp.json"))
    freqs, resp = responses(data, runs)

    grid_fc, grid_f, grid_e = [], [], []
    for r, h_db in resp:
        f_half, e = fit_f_half(h_db, freqs, q0, 0x1)
        grid_fc.append(r["fc"])
        grid_f.append(f_half)
        grid_e.append(np.sqrt(e))
        print(f"  fc={r['fc']:4d} -> F_half={f_half:5d}  (rms {np.sqrt(e):.2f} dB)",
              file=sys.stderr)
    print(f"  (cutoff fit rms: mean {np.mean(grid_e):.2f} dB, worst {np.max(grid_e):.2f} dB)",
          file=sys.stderr)

    # The physical curve is monotonic; fit noise is not. A running maximum is
    # the least-assuming way to impose that -- it never moves a point that is
    # already consistent with its neighbours, and a filter sweep that briefly
    # goes backwards is audible in a way a fraction of a dB is not.
    grid_f = list(np.maximum.accumulate(grid_f))

    return res_q, res_f, grid_fc, grid_f, q0, fc_ref


def emit(args, sha, res_q, res_f, f_full, fc_ref):
    # --- 8580 ---------------------------------------------------------------
    # module_chip.md §13.6: "6581 first, LUT-based, so 8580 is a table swap plus a
    # saturation bypass." The 8580 table is NOT measured here -- P6 owns that
    # (module_chip.md §1). What is emitted is reSID's own closed-form 8580 mapping,
    # w0 = 2*pi*12500*(fc+1)/2048 (filter.h's set_w0), converted into this
    # SVF's units. It is a placeholder with a correct shape, not a fit, and is
    # labelled as such in the header so P6 does not mistake it for measured.
    f_8580 = []
    for fc in range(2048):
        hz = 12500.0 * (fc + 1) / 2048.0
        f = int(round(np.pi * hz / SAMPLE_RATE * 32768.0))
        f_8580.append(int(np.clip(f, 20, SVF_F_MAX)))
    res_q_8580 = [int(np.clip(65534 - i * 4200, 2, 65534)) for i in range(16)]

    # --- DAC tables ---------------------------------------------------------
    dac8_6581, dac12_6581 = make_dac("6581")
    dac8_8580, _ = make_dac("8580")

    header = f'''#pragma once

#include <cstdint>

// GENERATED FILE -- do not edit by hand.
//
// Regenerate with:
//     cd tools/sid_ref && ./fetch_resid.sh && make
//     ./resid_probe --mode fc  --fc-step 16 --res 0 --fmode lp
//                   --out out/probe_fc_lp.f32  --meta out/probe_fc_lp.json
//     ./resid_probe --mode res --fc 1024 --fmode lp
//                   --out out/probe_res_lp.f32 --meta out/probe_res_lp.json
//     cd ../.. && tools/sid_ref/.venv/bin/python tools/fit_6581_filter.py
//
// PROVENANCE. module_chip.md §5.1: "The 6581 cutoff curve varied enormously between
// physical chips. Any curve is *a* 6581, not *the* 6581. The LUT must be
// documented as sampled from a named reference, never presented as canonical."
//
// These tables are sampled from reSID {sha[:12]}
// (daglem/reSID, 1.0-pre1), whose 6581 filter is itself a transistor-level
// model fitted to one measured chip -- Dag Lem's, with the op-amp and VCR
// parameters in its filter.cc. A different physical 6581 would give a
// different curve, and several of them differ by more than an octave in the
// mid-range. This is a reference, not the truth.
//
// The cutoff entries are not reSID's numbers converted; there are none to
// convert. They are the result of fitting src/chip/sid_filter.h's own two-pass
// SVF recurrence to reSID's measured magnitude response at each register
// value (tools/fit_6581_filter.py). They are therefore valid only for that
// exact recurrence: change the pass count, the fixed-point format or the
// integrator order and these must be refitted.

'''

    body = []
    body.append(fmt_table(
        "SID_6581_FC_Q15", "uint16_t", list(f_full), 16,
        "// 6581 cutoff register (11 bits) -> SVF Q15 half-frequency coefficient.\n"
        "// Measured at resonance 0 on the lowpass output. 4 KB of flash, and no\n"
        "// per-sample arithmetic at all on the device side."))
    body.append("")
    body.append(fmt_table(
        "SID_8580_FC_Q15", "uint16_t", f_8580, 16,
        "// 8580 cutoff. PLACEHOLDER, not measured -- module_chip.md §1 puts the 8580 model\n"
        "// at P6. This is reSID's own closed-form 8580 mapping (filter.h's set_w0:\n"
        "// cutoff = 12.5 kHz * (fc+1)/2048) converted into this SVF's units, so the\n"
        "// shape is right and the model switch is exercisable. Refit at P6."))
    body.append("")
    body.append(fmt_table(
        "SID_6581_RES_Q15", "uint16_t", res_q, 8,
        "// 6581 resonance nibble -> SVF Q15 damping. Large is damped, small is near\n"
        "// self-oscillation, matching src/filter.h's svf_compute_q convention."))
    body.append("")
    body.append(fmt_table(
        "SID_8580_RES_Q15", "uint16_t", res_q_8580, 8,
        "// 8580 resonance. PLACEHOLDER alongside SID_8580_FC_Q15; refit at P6."))
    body.append("")
    body.append(fmt_table(
        "SID_ENV_DAC_6581", "uint8_t", dac8_6581, 16,
        "// 6581 envelope DAC: 8-bit R-2R ladder, 2R/R = 2.20, bit-0 termination\n"
        "// missing. Emitted from reSID's own dac.h rather than reimplemented.\n"
        "// module_chip.md does not mention the DACs; §3's test (does it operate inside the\n"
        "// signal path?) puts them on the keep side, and this one costs 256 bytes."))
    body.append("")
    body.append(fmt_table(
        "SID_ENV_DAC_8580", "uint8_t", dac8_8580, 16,
        "// 8580 envelope DAC: 2R/R = 2.00 with correct termination, i.e. linear.\n"
        "// Kept as a table anyway so the model switch stays a table swap."))
    body.append("")
    body.append(fmt_table(
        "SID_WAVE_DAC_6581", "uint16_t", dac12_6581, 12,
        "// 6581 waveform DAC: 12-bit, same ladder. 8 KB of flash, which is why it\n"
        "// is behind CHIP_WAVE_DAC in sid_voice.h rather than unconditional --\n"
        "// F0 prices it, P1 decides. The 8580's is linear and has no table."))

    footer = f'''

// C64 board output network (module_chip.md §10's "free bonus"): the passive RC between
// the SID and the AV connector. Values are reSID's ExternalFilter, which names
// the components: w0lp = 1/(10k * 1nF) = 15.9 kHz, w0hp = 1/(1k * 10uF) =
// 15.9 Hz. The high-pass is not cosmetic here -- the 6581's 0x380
// waveform-DAC zero (see sid_osc.h) puts a real DC step on every gate, and
// without this it reaches the mix. See sid_filter.h's SidBoardFilter for why
// its state is held in Q16.
static constexpr int32_t SID_BOARD_LP_Q15 = {one_pole_q15(15915.0)};
static constexpr int32_t SID_BOARD_HP_Q15 = {one_pole_q15(15.915)};

// Input level at which the 6581 filter's saturation is fully in effect, in the
// voice-output units of sid_voice.h ((wave12 - wave_zero) * env8). One voice at
// full envelope peaks around {0x0c7f * 255}; this is set so a single voice stays
// essentially linear and three summed voices do not -- which is the property
// module_chip.md §5.2 depends on for shared-bus intermodulation to be real.
static constexpr int32_t SID_FILT_SAT_SCALE_6581 = {int(0x0c7f * 255 * 1.6)};

// The single scale factor between the voice contract (sid_voice.h) and the
// int16 the chip outputs, applied in sid_chip.h before the master volume.
//
// It is a right shift, and it is the *only* free gain in the whole chain --
// which is the point. Three 6581 voices at full envelope reach {3 * 0x0c7f * 255},
// and >> {SID_MIX_SHIFT} puts that at {(3 * 0x0c7f * 255) >> SID_MIX_SHIFT}, inside int16 with
// headroom for the resonant filter's own gain. Calibrated once against reSID's
// output level (tools/sid_compare.py reports the residual as `level gap`) and
// then left alone: the FM module's attempt 1 is the cautionary tale for what
// happens when a chain accumulates several of these and each is free to
// absorb the others' errors (history_fm.md §1.1a).
static constexpr int SID_MIX_SHIFT = {SID_MIX_SHIFT};
'''

    with open(args.out, "w") as f:
        f.write(header + "\n\n".join(body) + footer)
    print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
