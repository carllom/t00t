#!/usr/bin/env python3
"""make_freq_bank -- synthetic DX7 banks that sweep operator frequency settings.

Built for F5's frequency conformance test (fm2.md §4). F1 could diff every other
control-plane quantity as a table, but not this one: Dexed's `Dx7Note::osc_freq()`
is a private method with no accessor, so the only way to compare coarse / fine /
detune / fixed-frequency handling is to *render* a single operator and measure
what came out.

Each voice here is one audible carrier on algorithm 32 (all six operators are
carriers) with the other five at output level 0, a flat instant-attack envelope,
no key scaling, no LFO and no pitch EG. Rendering one produces a bare sine whose
frequency is exactly the thing under test, so a spectral peak measurement is an
exact statement about the frequency pipeline and nothing else.

Both sides consume these the same way as a factory bank -- dexed_render unpacks
the sysex itself, and tools/syx2patch.py converts it for t00t -- so the test
covers the real conversion path (op_ratio / op_detune_cents / op_fixed_hz) and
not just the engine.

Run: tools/fm_ref/.venv/bin/python tools/fm_ref/make_freq_bank.py
"""

import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "banks"


def pack_voice(name, mode, coarse, fine, detune):
    """One 128-byte packed DX7 voice: a single sounding carrier."""
    v = bytearray(128)

    for j in range(6):
        b = j * 17
        # Instant attack, hold at full, release to silence.
        v[b + 0:b + 4] = bytes([99, 99, 99, 99])       # R1-R4
        v[b + 4:b + 8] = bytes([99, 99, 99, 0])        # L1-L4
        v[b + 8] = 0                                    # break point
        v[b + 9] = 0                                    # left depth
        v[b + 10] = 0                                   # right depth
        v[b + 11] = 0                                   # curves
        v[b + 12] = 7 << 3                              # rate scaling 0, detune 7 (neutral)
        v[b + 13] = 0                                   # AMS 0, KVS 0
        v[b + 14] = 99 if j == 0 else 0                 # only OP6 sounds
        v[b + 15] = (1 << 1) | 0                        # coarse 1, ratio mode
        v[b + 16] = 0                                   # fine

    # OP6 (j = 0) carries the settings under test.
    v[12] = ((detune & 0x0F) << 3) | 0
    v[15] = ((coarse & 0x1F) << 1) | (mode & 1)
    v[16] = fine & 0x7F

    v[102:106] = bytes([99, 99, 99, 99])                # pitch EG rates
    v[106:110] = bytes([50, 50, 50, 50])                # pitch EG levels: 50 == no deviation
    v[110] = 31                                          # algorithm 32 (all carriers)
    v[111] = 0                                           # feedback 0, osc key sync off
    v[112:116] = bytes([0, 0, 0, 0])                     # LFO speed/delay/PMD/AMD
    v[116] = 0                                           # LFO key sync / waveform / PMS
    v[117] = 24                                          # transpose: DX7 neutral
    v[118:128] = name.ljust(10)[:10].encode("ascii")
    return bytes(v)


def bank(voices):
    """32 packed voices -> a 4104-byte sysex bulk dump."""
    while len(voices) < 32:
        voices.append(pack_voice("UNUSED", 0, 1, 0, 7))
    body = b"".join(voices[:32])
    checksum = (-sum(body)) & 0x7F
    # F0 43 0n 09 20 00 -- Yamaha, sub-status/channel 0, format 9 (32 voices),
    # byte count 0x1000. Verified byte-for-byte against the factory ROM dumps.
    return bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00]) + body + bytes([checksum, 0xF7])


# Each spec returns (voices, description-per-voice) so the comparison script can
# label what it is looking at without re-deriving the sweep.
def spec_coarse():
    vs, desc = [], []
    for c in range(32):
        vs.append(pack_voice(f"C{c:02d}", 0, c, 0, 7))
        desc.append(dict(mode=0, coarse=c, fine=0, detune=7))
    return vs, desc


def spec_fine():
    vs, desc = [], []
    for i in range(32):
        f = min(99, i * 3)
        vs.append(pack_voice(f"F{f:02d}", 0, 1, f, 7))
        desc.append(dict(mode=0, coarse=1, fine=f, detune=7))
    return vs, desc


def spec_detune():
    # All 15 detune values at two coarse settings, so the note-dependent
    # detune curve is measured rather than assumed.
    vs, desc = [], []
    for c in (1, 2):
        for d in range(15):
            vs.append(pack_voice(f"D{c}_{d:02d}", 0, c, 0, d))
            desc.append(dict(mode=0, coarse=c, fine=0, detune=d))
    return vs, desc


def spec_fixed():
    vs, desc = [], []
    for c in range(4):
        for f in (0, 20, 40, 60, 80, 99):
            vs.append(pack_voice(f"X{c}_{f:02d}", 1, c, f, 7))
            desc.append(dict(mode=1, coarse=c, fine=f, detune=7))
    # Fixed-mode detune only ever sharpens on real hardware (Dexed:
    # `detune > 7 ? 13457 * (detune - 7) : 0`), so sweep across the midpoint.
    for d in (0, 4, 7, 9, 11, 14):
        vs.append(pack_voice(f"XD{d:02d}", 1, 2, 0, d))
        desc.append(dict(mode=1, coarse=2, fine=0, detune=d))
    return vs, desc


SPECS = {"coarse": spec_coarse, "fine": spec_fine,
         "detune": spec_detune, "fixed": spec_fixed}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in SPECS.items():
        voices, _ = fn()
        path = OUT / f"freq_{name}.syx"
        path.write_bytes(bank(list(voices)))
        print(f"  wrote {path.name} ({len(voices)} test voices)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
