#!/usr/bin/env python3
"""Signal-plane scorecard: t00t's CHIP_STRICT render against reSID's.

The other half of F0's verification. tools/sid_ctl_diff.py compares everything
that is a deterministic function -- envelope trajectories, the noise sequence,
the waveform logic, the DAC tables -- exactly. What is left is the filter and
the assembled sound, and those need spectra.

    tools/sid_ref/.venv/bin/python tools/sid_compare.py --all --json out/scorecard.json
    tools/sid_ref/.venv/bin/python tools/sid_compare.py out/ref.wav out/test.wav

Metrics are chosen to name a defect rather than to produce a single number:

  level gap        absolute loudness difference. Reported alone and never
                   folded into the shape metrics -- a wrong output gain and a
                   wrong filter are different bugs and must not average.
  band MAE         per-third-octave spectral error over frames where BOTH
                   renders are sounding. Third-octave rather than harmonic
                   tracking because this corpus sweeps pitch, sweeps cutoff and
                   includes noise, so there is often no f0 to track.
  attack MAE       the same, restricted to the first 100 ms, where essentially
                   everything is still sounding on both sides. Stays valid when
                   coactive_frac is low.
  centroid ratio   brightness, as a scale-free ratio. <1 reads as "too dull",
                   >1 as "too bright".
  envelope MAE     the timing axis: broadband RMS in dB over time.
  coactive_frac    how much of the reference's sounding time the spectral
                   numbers actually cover. A low value is itself a finding --
                   it means one side stopped early -- and it makes the band
                   numbers correspondingly less meaningful.

That last split is the FM module's hardest-won lesson (fm2.md §3.1): scoring
timbre wherever the *reference* sounds conflates timbre with envelope, because
an engine whose envelope dies early then reads as spectrally wrong for every
frame it is silent. Without the split, F0's baseline spectral numbers were pure
envelope artefact.

numpy only, no scipy.
"""

import argparse
import json
import os
import subprocess
import sys
import wave as wavemod

import numpy as np

RATE = 44100
NFFT = 2048
HOP = 512
FLOOR_DB = -80.0

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
STREAM_DIR = os.path.join(HERE, "sid_ref", "streams")
OUT_DIR = os.path.join(HERE, "sid_ref", "out")
RESID_RENDER = os.path.join(HERE, "sid_ref", "resid_render")
T00T_RENDER = os.path.join(HERE, "host_render", "render_sid")


def read_wav_f32(path):
    """Reads the float32 WAVs both renderers write (format tag 3)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    pos = 12
    fmt_tag = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = int.from_bytes(data[pos + 4:pos + 8], "little")
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt_tag = int.from_bytes(body[0:2], "little")
        elif cid == b"data":
            if fmt_tag == 3:
                return np.frombuffer(body, dtype="<f4").astype(np.float64)
            if fmt_tag == 1:
                return np.frombuffer(body, dtype="<i2").astype(np.float64) / 32768.0
            raise ValueError(f"{path}: unsupported WAV format tag {fmt_tag}")
        pos += 8 + size + (size & 1)
    raise ValueError(f"{path}: no data chunk")


def stft_db(x):
    n = 1 + max(0, (len(x) - NFFT) // HOP)
    if n <= 0:
        return np.zeros((0, NFFT // 2 + 1)), np.zeros(0)
    w = np.hanning(NFFT)
    frames = np.empty((n, NFFT // 2 + 1))
    rms = np.empty(n)
    for i in range(n):
        seg = x[i * HOP:i * HOP + NFFT]
        frames[i] = np.abs(np.fft.rfft(seg * w))
        rms[i] = np.sqrt(np.mean(seg ** 2))
    return frames, rms


def third_octave_bands(lo=40.0, hi=18000.0):
    """Band edges, third-octave. Returns index slices into an rfft bin array."""
    freqs = np.fft.rfftfreq(NFFT, 1.0 / RATE)
    edges = []
    f = lo
    while f < hi:
        edges.append(f)
        f *= 2.0 ** (1.0 / 3.0)
    edges.append(hi)
    bands = []
    for a, b in zip(edges, edges[1:]):
        idx = np.where((freqs >= a) & (freqs < b))[0]
        if len(idx):
            bands.append(idx)
    return bands


BANDS = third_octave_bands()


def band_db(mag_frames):
    out = np.empty((mag_frames.shape[0], len(BANDS)))
    for j, idx in enumerate(BANDS):
        out[:, j] = np.sqrt(np.mean(mag_frames[:, idx] ** 2, axis=1))
    return np.maximum(20.0 * np.log10(np.maximum(out, 1e-12)), FLOOR_DB)


def centroid(mag_frames):
    """Spectral centroid in Hz, per frame."""
    freqs = np.fft.rfftfreq(NFFT, 1.0 / RATE)
    e = mag_frames ** 2
    tot = e.sum(axis=1)
    with np.errstate(invalid="ignore", divide="ignore"):
        c = (e * freqs).sum(axis=1) / tot
    return np.where(tot > 0, c, 0.0)


def score(ref, test):
    n = min(len(ref), len(test))
    ref, test = ref[:n], test[:n]

    rmag, rrms = stft_db(ref)
    tmag, trms = stft_db(test)
    m = min(len(rrms), len(trms))
    rmag, tmag, rrms, trms = rmag[:m], tmag[:m], rrms[:m], trms[:m]

    ref_rms = float(np.sqrt(np.mean(ref ** 2)))
    test_rms = float(np.sqrt(np.mean(test ** 2)))
    level_gap = 20.0 * np.log10(max(test_rms, 1e-12) / max(ref_rms, 1e-12))

    rdb = np.maximum(20.0 * np.log10(np.maximum(rrms, 1e-12)), FLOOR_DB)
    tdb = np.maximum(20.0 * np.log10(np.maximum(trms, 1e-12)), FLOOR_DB)

    # Sounding = within 60 dB of that render's own peak. Relative to its own
    # peak, not to a shared one, so the level gap does not leak into which
    # frames count as active.
    r_on = rdb > (rdb.max() - 60.0)
    t_on = tdb > (tdb.max() - 60.0)
    coactive = r_on & t_on
    coactive_frac = float(coactive.sum() / max(r_on.sum(), 1))

    rb, tb = band_db(rmag), band_db(tmag)
    # Level-match before comparing shape: a constant gain difference is the
    # level gap, already reported, and must not be counted twice.
    def shape_mae(mask):
        if mask.sum() == 0:
            return float("nan"), float("nan")
        a, b = rb[mask], tb[mask]
        off = np.mean(b) - np.mean(a)
        d = np.abs((b - off) - a)
        return float(np.mean(d)), float(np.percentile(d, 95))

    band_mae, band_p95 = shape_mae(coactive)
    attack_frames = int(0.100 * RATE / HOP)
    amask = np.zeros(m, dtype=bool)
    amask[:attack_frames] = True
    attack_mae, _ = shape_mae(amask)

    rc, tc = centroid(rmag), centroid(tmag)
    good = coactive & (rc > 0) & (tc > 0)
    cent_ratio = float(np.mean(tc[good] / rc[good])) if good.sum() else float("nan")

    env_mae = float(np.mean(np.abs((tdb - (tdb.max() - rdb.max())) - rdb)))

    return {
        "level_gap_db": round(level_gap, 2),
        "band_mae_db": round(band_mae, 2),
        "band_p95_db": round(band_p95, 2),
        "attack_mae_db": round(attack_mae, 2),
        "centroid_ratio": round(cent_ratio, 3),
        "envelope_mae_db": round(env_mae, 2),
        "coactive_frac": round(coactive_frac, 3),
        "frames": int(m),
    }


def render_pair(stream, quiet=True):
    os.makedirs(OUT_DIR, exist_ok=True)
    name = os.path.splitext(os.path.basename(stream))[0]
    ref_wav = os.path.join(OUT_DIR, f"ref_{name}.wav")
    test_wav = os.path.join(OUT_DIR, f"t00t_{name}.wav")
    for binary, out in ((RESID_RENDER, ref_wav), (T00T_RENDER, test_wav)):
        if not os.path.exists(binary):
            sys.exit(f"{binary} not found -- build it first")
        subprocess.run([binary, "--stream", stream, "--out", out],
                       check=True, capture_output=quiet)
    return ref_wav, test_wav


HEADER = (f"{'stream':<20} {'level':>7} {'band':>7} {'p95':>7} "
          f"{'attack':>7} {'centroid':>9} {'env':>7} {'coact':>6}")


def row(name, s):
    return (f"{name:<20} {s['level_gap_db']:>+7.2f} {s['band_mae_db']:>7.2f} "
            f"{s['band_p95_db']:>7.2f} {s['attack_mae_db']:>7.2f} "
            f"{s['centroid_ratio']:>8.3f}x {s['envelope_mae_db']:>7.2f} "
            f"{s['coactive_frac']:>6.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref", nargs="?", help="reference WAV")
    ap.add_argument("test", nargs="?", help="t00t WAV")
    ap.add_argument("--stream", help="render and score one stream")
    ap.add_argument("--all", action="store_true", help="render and score the whole corpus")
    ap.add_argument("--json", help="write the scorecard here")
    args = ap.parse_args()

    results = {}
    if args.all or args.stream:
        streams = ([args.stream] if args.stream
                   else sorted(os.path.join(STREAM_DIR, f)
                               for f in os.listdir(STREAM_DIR) if f.endswith(".sidreg")))
        print(HEADER)
        print("-" * len(HEADER))
        for stream in streams:
            name = os.path.splitext(os.path.basename(stream))[0]
            ref_wav, test_wav = render_pair(stream)
            s = score(read_wav_f32(ref_wav), read_wav_f32(test_wav))
            results[name] = s
            print(row(name, s))
        if len(results) > 1:
            print("-" * len(HEADER))
            agg = {k: float(np.mean([r[k] for r in results.values()]))
                   for k in ("level_gap_db", "band_mae_db", "band_p95_db",
                             "attack_mae_db", "centroid_ratio", "envelope_mae_db",
                             "coactive_frac")}
            agg["frames"] = 0
            print(row("MEAN", {k: round(v, 3) for k, v in agg.items()}))
            results["_mean"] = {k: round(v, 3) for k, v in agg.items()}
    elif args.ref and args.test:
        s = score(read_wav_f32(args.ref), read_wav_f32(args.test))
        for k, v in s.items():
            print(f"  {k:<18} {v}")
        results[os.path.basename(args.test)] = s
    else:
        ap.print_help()
        return 2

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2, sort_keys=True)
        print(f"\nwrote {args.json}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
