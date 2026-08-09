#!/usr/bin/env python3
"""fm_compare -- scores t00t's FM engine against a Dexed reference render.

The signal-plane half of fm2.md §3 (F0-c). Takes two mono float32 WAVs of the
same note -- one from tools/fm_ref/dexed_render, one from
tools/host_render/render_fm_patch -- and reports metrics chosen to name the
things ears reported vaguely during attempt 1:

  harmonic tracking   "too thin", "missing portions of sound"
  spectral centroid   "sine-like", "too bright"/"too dull"
  RMS envelope        "delayed envelope", "weak sustain"
  level               absolute loudness gap (reported, never folded in)

Deliberately NOT sample-accurate. Both renders start their note at t=0 at the
same sample rate, so no alignment search is done; the metrics are all magnitude
envelopes over time, which is the level of agreement fm2.md targets.

Requires numpy only (no scipy) -- see tools/fm_ref/requirements.txt.

Usage:
    fm_compare.py REF.wav TEST.wav --f0-note 48
    fm_compare.py --bank BANK.syx --voices 0-31 --note 48   # whole-bank sweep
    fm_compare.py ... --json scorecard.json
"""

import argparse
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
DEXED_RENDER = REPO / "tools" / "fm_ref" / "dexed_render"
T00T_RENDER = REPO / "tools" / "host_render" / "render_fm_patch"

# Everything below this, relative to a render's own peak, is treated as silence.
# Both engines reach an exact digital zero on release, and log(0) is not a useful
# comparison; -80 dB is well under anything audible in a lo-fi 16-bit context.
FLOOR_DB = -80.0

# STFT geometry. 2048 @ 44.1 kHz = 46 ms window / 21.5 Hz bins -- enough to
# resolve the harmonics of a C3 (130.8 Hz fundamental) cleanly, while the 256
# hop (5.8 ms) is fine enough to see a DX7 attack transient, which is the
# fastest thing being compared.
NFFT = 2048
HOP = 256

# Harmonics tracked. 40 x 130.8 Hz = 5.2 kHz, comfortably past where DX7 patches
# carry their character and well inside Nyquist for the notes this rig uses.
N_HARMONICS = 40


def read_wav_f32(path):
    """Minimal float32/PCM16 mono WAV reader. Avoids a scipy dependency."""
    data = Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")

    pos, fmt, samples, rate, channels = 12, None, None, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        (csize,) = struct.unpack("<I", data[pos + 4:pos + 8])
        body = data[pos + 8:pos + 8 + csize]
        if cid == b"fmt ":
            fmt, channels, rate = struct.unpack("<HHI", body[:8])
            (bits,) = struct.unpack("<H", body[14:16])
        elif cid == b"data":
            if fmt == 3 and bits == 32:
                samples = np.frombuffer(body, dtype="<f4").astype(np.float64)
            elif fmt == 1 and bits == 16:
                samples = np.frombuffer(body, dtype="<i2").astype(np.float64) / 32768.0
            else:
                raise ValueError(f"{path}: unsupported format tag {fmt}/{bits}-bit")
        pos += 8 + csize + (csize & 1)

    if samples is None:
        raise ValueError(f"{path}: no data chunk")
    if channels != 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return samples, rate


def stft_mag(x, nfft=NFFT, hop=HOP):
    """Magnitude STFT. Hann window, no padding -- frame t starts at sample t*hop."""
    if len(x) < nfft:
        x = np.pad(x, (0, nfft - len(x)))
    n_frames = 1 + (len(x) - nfft) // hop
    win = np.hanning(nfft)
    frames = np.lib.stride_tricks.sliding_window_view(x, nfft)[::hop][:n_frames]
    return np.abs(np.fft.rfft(frames * win, axis=1))


def harmonic_tracks(mag, rate, f0, n_harm=N_HARMONICS, nfft=NFFT):
    """H[k, t]: magnitude of harmonic k+1 over time.

    Each harmonic takes the peak of a +/-1 bin neighbourhood, which absorbs the
    sub-bin frequency error of a 21.5 Hz grid without letting adjacent harmonics
    (spaced ~6 bins at C3) leak into each other.
    """
    bin_hz = rate / nfft
    tracks = np.zeros((n_harm, mag.shape[0]))
    for k in range(n_harm):
        centre = int(round((k + 1) * f0 / bin_hz))
        if centre + 1 >= mag.shape[1]:
            break
        tracks[k] = mag[:, centre - 1:centre + 2].max(axis=1)
    return tracks


def to_db(x, ref):
    """Magnitude -> dB relative to `ref`, floored at FLOOR_DB."""
    with np.errstate(divide="ignore"):
        db = 20.0 * np.log10(np.maximum(x, 1e-30) / ref)
    return np.maximum(db, FLOOR_DB)


def rms_envelope(x, hop=HOP, win=1024):
    n_frames = max(1, 1 + (len(x) - win) // hop)
    frames = np.lib.stride_tricks.sliding_window_view(x, win)[::hop][:n_frames]
    return np.sqrt((frames ** 2).mean(axis=1))


def envelope_features(env, rate, hop, gate_s):
    """Attack/sustain/release timings from an RMS envelope, in seconds."""
    peak = env.max()
    if peak <= 0:
        return dict(attack_ms=None, peak_db=None, sustain_db=None, release_t60_ms=None)

    t = np.arange(len(env)) * hop / rate
    peak_i = int(np.argmax(env))

    # Attack: first crossing of -3 dB relative to peak.
    thresh = peak * (10 ** (-3.0 / 20.0))
    above = np.nonzero(env >= thresh)[0]
    attack_ms = t[above[0]] * 1000.0 if len(above) else None

    # Sustain: level in the last 10% of the gated portion.
    gate_i = min(len(env) - 1, int(gate_s * rate / hop))
    lo = max(0, int(gate_i * 0.9))
    sustain = env[lo:gate_i + 1].mean() if gate_i > lo else env[gate_i]

    # Release T60: time from key-up until 60 dB below the level at key-up.
    rel_ref = env[gate_i]
    release_ms = None
    if rel_ref > 0:
        target = rel_ref * (10 ** (-60.0 / 20.0))
        after = np.nonzero(env[gate_i:] <= target)[0]
        if len(after):
            release_ms = after[0] * hop / rate * 1000.0

    db = lambda v: 20.0 * math.log10(v / peak) if v > 0 else FLOOR_DB
    return dict(
        attack_ms=attack_ms,
        peak_db=20.0 * math.log10(peak),
        peak_time_ms=t[peak_i] * 1000.0,
        sustain_db=db(sustain),
        release_t60_ms=release_ms,
    )


def compare(ref_path, test_path, note, gate_s, label=""):
    ref, rate_a = read_wav_f32(ref_path)
    test, rate_b = read_wav_f32(test_path)
    if rate_a != rate_b:
        raise ValueError(f"sample rate mismatch: {rate_a} vs {rate_b}")
    n = min(len(ref), len(test))
    ref, test = ref[:n], test[:n]

    f0 = 440.0 * 2.0 ** ((note - 69) / 12.0)

    ref_peak, test_peak = np.abs(ref).max(), np.abs(test).max()
    level_gap_db = (20.0 * math.log10(test_peak / ref_peak)
                    if ref_peak > 0 and test_peak > 0 else None)

    # Every spectral metric below runs on peak-normalised signals: the absolute
    # level gap is reported on its own (above) and must not leak into the shape
    # comparison, or a merely-quiet engine would look spectrally wrong too.
    ref_n = ref / ref_peak if ref_peak > 0 else ref
    test_n = test / test_peak if test_peak > 0 else test

    mag_r, mag_t = stft_mag(ref_n), stft_mag(test_n)
    frames = min(mag_r.shape[0], mag_t.shape[0])
    mag_r, mag_t = mag_r[:frames], mag_t[:frames]

    h_r = harmonic_tracks(mag_r, rate_a, f0)
    h_t = harmonic_tracks(mag_t, rate_a, f0)
    ref_h_peak = max(h_r.max(), 1e-30)
    hr_db, ht_db = to_db(h_r, ref_h_peak), to_db(h_t, ref_h_peak)

    # Timbre and envelope are scored on separate frame sets, deliberately.
    #
    # Scoring timbre wherever the *reference* sounds conflates the two axes: an
    # engine whose envelope dies early then reads as spectrally wrong for every
    # frame it is silent, and a real timbre difference in the frames it does
    # sound gets averaged into noise. (Measured on the F0 baseline: the harmonic
    # error was dominated by frames where t00t had already decayed 60 dB, which
    # says nothing about its spectrum.) So spectral metrics run over frames where
    # BOTH are sounding, and `coactive_frac` reports how much of the reference
    # that actually covered -- a low value is itself the envelope finding, and
    # makes the spectral numbers alongside it correspondingly less meaningful.
    floor_lin = 10 ** (FLOOR_DB / 20.0)
    active_r = h_r.max(axis=0) > ref_h_peak * floor_lin
    test_h_peak = max(h_t.max(), 1e-30)
    active_t = h_t.max(axis=0) > test_h_peak * floor_lin
    coactive = active_r & active_t
    if not coactive.any():
        coactive = active_r if active_r.any() else np.ones(frames, dtype=bool)
    coactive_frac = float(coactive.sum() / max(active_r.sum(), 1))

    # Within the co-active window each side is referenced to its own peak, so
    # this is purely spectral shape -- level is already reported separately.
    hr_rel = to_db(h_r, ref_h_peak)
    ht_rel = to_db(h_t, test_h_peak)
    harm_err = np.abs(hr_rel - ht_rel)[:, coactive]
    harm_mae = float(harm_err.mean())
    harm_p95 = float(np.percentile(harm_err, 95))
    per_harmonic = (ht_rel - hr_rel)[:, coactive].mean(axis=1)

    # Attack-window timbre: the first 100 ms, where essentially every patch is
    # still sounding on both sides regardless of envelope bugs downstream. This
    # is the timbre number that stays meaningful when coactive_frac is low.
    atk_frames = max(1, int(0.100 * rate_a / HOP))
    atk = slice(0, min(atk_frames, frames))
    atk_r = to_db(h_r[:, atk], max(h_r[:, atk].max(), 1e-30))
    atk_t = to_db(h_t[:, atk], max(h_t[:, atk].max(), 1e-30))
    attack_harm_mae = float(np.abs(atk_r - atk_t).mean())
    active = coactive

    # Spectral centroid over the tracked harmonics, in harmonic number -- a
    # scale-free "how bright" that maps directly onto "thin"/"sine-like".
    idx = np.arange(1, h_r.shape[0] + 1)[:, None]
    cent_r = (h_r[:, active] * idx).sum(axis=0) / np.maximum(h_r[:, active].sum(axis=0), 1e-30)
    cent_t = (h_t[:, active] * idx).sum(axis=0) / np.maximum(h_t[:, active].sum(axis=0), 1e-30)

    # Log-spectral distance over the full magnitude spectrum: the single scalar
    # for tracking regressions across phases.
    full_ref_peak = max(mag_r.max(), 1e-30)
    lsd = float(np.abs(to_db(mag_r, full_ref_peak) - to_db(mag_t, full_ref_peak))[active].mean())

    env_r, env_t = rms_envelope(ref_n), rms_envelope(test_n)
    fr = envelope_features(env_r, rate_a, HOP, gate_s)
    ft = envelope_features(env_t, rate_a, HOP, gate_s)

    m = min(len(env_r), len(env_t))
    env_peak = max(env_r.max(), 1e-30)
    env_err = np.abs(to_db(env_r[:m], env_peak) - to_db(env_t[:m], env_peak))
    env_mae = float(env_err[to_db(env_r[:m], env_peak) > FLOOR_DB + 1].mean())

    return dict(
        label=label,
        note=note, f0_hz=round(f0, 2),
        level_gap_db=level_gap_db,
        harmonic_mae_db=harm_mae,
        harmonic_p95_db=harm_p95,
        attack_harmonic_mae_db=attack_harm_mae,
        coactive_frac=coactive_frac,
        centroid_ref=float(cent_r.mean()),
        centroid_test=float(cent_t.mean()),
        centroid_ratio=float(cent_t.mean() / max(cent_r.mean(), 1e-30)),
        lsd_db=lsd,
        env_mae_db=env_mae,
        ref_env=fr, test_env=ft,
        per_harmonic_db=[round(float(v), 1) for v in per_harmonic],
    )


def fmt(v, spec=".2f", dash="--"):
    return dash if v is None else format(v, spec)


def print_report(r, verbose=True):
    print(f"\n=== {r['label']}  (note {r['note']}, f0 {r['f0_hz']} Hz) ===")
    print(f"  level gap          {fmt(r['level_gap_db'], '+.1f')} dB   (test re ref; shape metrics are level-normalised)")
    print(f"  harmonic MAE       {r['harmonic_mae_db']:.1f} dB   p95 {r['harmonic_p95_db']:.1f} dB"
          f"   (over {r['coactive_frac'] * 100:.0f}% of the reference's sounding frames)")
    print(f"  attack timbre MAE  {r['attack_harmonic_mae_db']:.1f} dB   (first 100 ms, envelope-independent)")
    print(f"  spectral centroid  ref {r['centroid_ref']:.2f} -> test {r['centroid_test']:.2f} "
          f"(x{r['centroid_ratio']:.2f}) harmonics")
    print(f"  log-spectral dist  {r['lsd_db']:.1f} dB")
    print(f"  envelope MAE       {r['env_mae_db']:.1f} dB")
    a, b = r["ref_env"], r["test_env"]
    print(f"                     {'':>12} {'ref':>10} {'test':>10}")
    for key, unit, spec in (("attack_ms", "ms", ".1f"), ("peak_time_ms", "ms", ".1f"),
                            ("sustain_db", "dB", ".1f"), ("release_t60_ms", "ms", ".1f")):
        print(f"                     {key:>12} {fmt(a.get(key), spec):>10} {fmt(b.get(key), spec):>10}  {unit}")
    if verbose:
        ph = r["per_harmonic_db"]
        print("  per-harmonic error (test - ref, dB), harmonics 1-16:")
        print("   ", " ".join(f"{v:>6.1f}" for v in ph[:16]))
        print("  harmonics 17-32:")
        print("   ", " ".join(f"{v:>6.1f}" for v in ph[16:32]))


def render_pair(bank, voice, note, vel, gate, tail, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    ref = outdir / f"ref_v{voice:02d}_n{note}.wav"
    test = outdir / f"t00t_v{voice:02d}_n{note}.wav"
    common = ["--syx", str(bank), "--voice", str(voice), "--note", str(note),
              "--vel", str(vel), "--gate", str(gate), "--tail", str(tail)]
    for exe, out in ((DEXED_RENDER, ref), (T00T_RENDER, test)):
        if not exe.exists():
            sys.exit(f"error: {exe} not built -- see tools/fm_ref/Makefile "
                     f"and tools/host_render/Makefile.fm")
        res = subprocess.run([str(exe), *common, "--out", str(out)],
                             capture_output=True, text=True)
        if res.returncode != 0:
            sys.exit(f"error: {exe.name} failed for voice {voice}:\n{res.stderr}")
    return ref, test


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wavs", nargs="*", help="REF.wav TEST.wav (omit when using --bank)")
    p.add_argument("--bank", help="render both sides from this .syx instead")
    p.add_argument("--voices", default="0-31", help="voice range for --bank (default 0-31)")
    p.add_argument("--note", type=int, default=48, help="MIDI note (default 48 = C3)")
    p.add_argument("--vel", type=int, default=100)
    p.add_argument("--gate", type=float, default=2.0)
    p.add_argument("--tail", type=float, default=1.5)
    p.add_argument("--outdir", default="tools/fm_ref/out", help="where --bank renders go")
    p.add_argument("--json", help="write the full scorecard here")
    p.add_argument("--quiet", action="store_true", help="summary table only")
    args = p.parse_args()

    results = []
    if args.bank:
        bank = Path(args.bank)
        lo, _, hi = args.voices.partition("-")
        voices = range(int(lo), int(hi or lo) + 1)
        names = subprocess.run([str(DEXED_RENDER), "--syx", str(bank), "--list"],
                               capture_output=True, text=True).stdout.splitlines()
        for v in voices:
            ref, test = render_pair(bank, v, args.note, args.vel, args.gate,
                                    args.tail, Path(args.outdir))
            name = names[v][4:].strip() if v < len(names) else f"voice {v}"
            r = compare(ref, test, args.note, args.gate, label=f"{v:02d} {name}")
            results.append(r)
            if not args.quiet:
                print_report(r, verbose=False)
    else:
        if len(args.wavs) != 2:
            p.error("give REF.wav TEST.wav, or use --bank")
        r = compare(args.wavs[0], args.wavs[1], args.note, args.gate,
                    label=f"{Path(args.wavs[0]).name} vs {Path(args.wavs[1]).name}")
        results.append(r)
        print_report(r)

    if len(results) > 1:
        print("\n=== summary ===")
        print(f"{'patch':<16} {'harm MAE':>9} {'atkMAE':>7} {'env MAE':>8} {'cent x':>7} {'level':>8}")
        for r in results:
            print(f"{r['label']:<16} {r['harmonic_mae_db']:>9.1f} {r['attack_harmonic_mae_db']:>7.1f} "
                  f"{r['env_mae_db']:>8.1f} {r['centroid_ratio']:>7.2f} "
                  f"{fmt(r['level_gap_db'], '+.1f'):>8}")
        agg = lambda k: sum(r[k] for r in results) / len(results)
        print(f"{'MEAN':<16} {agg('harmonic_mae_db'):>9.1f} {agg('attack_harmonic_mae_db'):>7.1f} "
              f"{agg('env_mae_db'):>8.1f} {agg('centroid_ratio'):>7.2f}")

    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
