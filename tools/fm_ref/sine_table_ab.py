#!/usr/bin/env python3
"""sine_table_ab -- what does t00t's non-interpolated sine table cost at real
modulation indices? (fm2.md §3.2, the F0 check.)

fm.md §3.5 justified dropping interpolation with an estimated ~-72 dBc
truncation-spur floor. That estimate was reasoned about a bare sine. Under phase
modulation the truncation error is no longer a fixed low-level dither: the
lookup index moves through the table at a rate set by the *modulated* phase, so
the spur floor rises with modulation index. Since fixing fm2.md §1.1(a) is
specifically about letting real patches reach index 3-10, the estimate needs
checking in that range before F2 commits to a kernel shape.

Models t00t's exact integer arithmetic (uint32 phase accumulator, 4096-entry
int16 table, index = phase >> 20) for a 2-operator pair -- modulator through the
same table as the carrier, as in the real engine -- against a float64 reference
with identical increments, and reports SNR over a beta sweep.

Three variants, to separate the two independent error sources:
  trunc-16    4096-entry int16 table, no interpolation   (what op.h does today)
  interp-16   same table, linear interpolation           (costs ~45%/op, fm.md §3.5)
  trunc-24    4096-entry int32 table, no interpolation   (isolates phase
              truncation from 16-bit amplitude quantisation)

Run: tools/fm_ref/.venv/bin/python tools/fm_ref/sine_table_ab.py
"""

import numpy as np

SR = 44100
N = 1 << 17          # ~3 s, power of two for a clean FFT
TABLE_BITS = 12      # sine_tab.h's FM_TABLE_BITS
TABLE = 1 << TABLE_BITS
PHASE_SHIFT = 32 - TABLE_BITS


def make_table(amp_bits):
    """Quarter-wave-symmetric table, same generation as sine_tab.h."""
    i = np.arange(TABLE)
    full = (2 ** (amp_bits - 1) - 1) * np.sin(2 * np.pi * i / TABLE)
    return np.rint(full).astype(np.int64)


def render(f_c, f_m, beta, mode):
    """One 2-operator FM pair. Returns float64 audio in [-1, 1].

    `beta` is the modulation index in radians, applied as a full-circle phase
    offset: 2**32 phase units == 2*pi radians, matching the contract fm2.md §2
    proposes (unity operator output == one full cycle of deviation).
    """
    n = np.arange(N)
    inc_c = int(f_c / SR * 2 ** 32)
    inc_m = int(f_m / SR * 2 ** 32)
    ph_c = (n * inc_c) % 2 ** 32
    ph_m = (n * inc_m) % 2 ** 32

    if mode == "exact":
        mod = beta * np.sin(2 * np.pi * ph_m / 2 ** 32)
        return np.sin(2 * np.pi * ph_c / 2 ** 32 + mod)

    amp_bits = 24 if mode == "trunc-24" else 16
    tab = make_table(amp_bits)
    peak = float(2 ** (amp_bits - 1) - 1)

    def lookup(phase):
        idx = (phase >> PHASE_SHIFT).astype(np.int64)
        if mode == "interp-16":
            frac = ((phase >> (PHASE_SHIFT - 8)) & 0xFF).astype(np.float64) / 256.0
            y0 = tab[idx]
            y1 = tab[(idx + 1) & (TABLE - 1)]
            return (y0 + (y1 - y0) * frac) / peak
        return tab[idx] / peak

    # Modulator output scaled to `beta` radians of carrier phase deviation,
    # then converted to phase units the same way the kernel does (unsigned wrap).
    mod_units = (lookup(ph_m) * (beta / (2 * np.pi)) * 2 ** 32).astype(np.int64)
    return lookup(((ph_c + mod_units) % 2 ** 32).astype(np.uint64))


def snr_db(ref, test):
    """SNR after removing the best-fit scale, so a pure gain difference doesn't
    count as noise."""
    scale = float(np.dot(ref, test) / np.dot(test, test))
    err = ref - test * scale
    return 10 * np.log10(np.dot(ref, ref) / max(np.dot(err, err), 1e-300))


def spur_floor_db(sig, f_c, f_m, beta):
    """Worst non-harmonic spectral component, dB relative to the peak partial.

    FM sidebands land at f_c +/- k*f_m; anything else at a meaningful level is
    truncation error folded back into the audio band, which is what actually
    gets heard as grit.
    """
    win = np.hanning(len(sig))
    spec = np.abs(np.fft.rfft(sig * win))
    freqs = np.fft.rfftfreq(len(sig), 1 / SR)
    legit = np.zeros(len(freqs), dtype=bool)
    # Sidebands out to well past where a given beta carries real energy.
    for k in range(-int(beta * 3) - 12, int(beta * 3) + 13):
        f = f_c + k * f_m
        if 0 < f < SR / 2:
            legit |= np.abs(freqs - f) < 30.0
    peak = spec.max()
    rest = spec[~legit]
    return 20 * np.log10(max(rest.max(), 1e-30) / peak)


def main():
    f_c, f_m = 261.63, 261.63     # C4 carrier, 1:1 ratio -- the commonest DX7 pair
    modes = ["trunc-16", "interp-16", "trunc-24"]
    betas = [0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 12.0]

    print(f"2-op FM, f_c = f_m = {f_c} Hz (1:1), {TABLE}-entry table, "
          f"index = phase >> {PHASE_SHIFT}")
    print("\nSNR vs exact float sine (dB, higher is better):")
    print(f"  {'beta':>6} " + " ".join(f"{m:>11}" for m in modes))
    snrs = {}
    for beta in betas:
        ref = render(f_c, f_m, beta, "exact")
        row = []
        for m in modes:
            v = snr_db(ref, render(f_c, f_m, beta, m))
            snrs[(beta, m)] = v
            row.append(f"{v:>11.1f}")
        print(f"  {beta:>6.1f} " + " ".join(row))

    print("\nWorst non-harmonic spur (dBc, lower is better):")
    print(f"  {'beta':>6} " + " ".join(f"{m:>11}" for m in modes))
    for beta in betas:
        row = []
        for m in modes:
            row.append(f"{spur_floor_db(render(f_c, f_m, beta, m), f_c, f_m, beta):>11.1f}")
        print(f"  {beta:>6.1f} " + " ".join(row))

    print("\nInterpolation's benefit (interp-16 minus trunc-16 SNR, dB):")
    for beta in betas:
        print(f"  beta {beta:>5.1f}   {snrs[(beta, 'interp-16')] - snrs[(beta, 'trunc-16')]:>+6.1f} dB")


if __name__ == "__main__":
    main()
