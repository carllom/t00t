#!/usr/bin/env python3
"""make_fb_bank -- synthetic DX7 banks that sweep operator FEEDBACK depth.

A quantity the control-plane diff cannot reach as a table: the algorithm
routing table only records *which* operator has feedback, not how deep the
feedback runs at each of the 8 levels, which lives in the per-sample kernel
(`op_render_fb` against Dexed's `FmOpKernel::compute_fb`).

Each voice is algorithm 32 (all six operators are carriers) with only OP6
audible and OP6's feedback level set to the value under test. So the render is
*nothing but* the self-feedback waveform -- at level 0 a pure sine, and by
level 7 the near-sawtooth that DX7 feedback is famous for. Comparing that
against Dexed is an exact statement about the feedback loop and nothing else.

Both sides consume these the same way as a factory bank (dexed_render unpacks
the sysex itself, syx2patch.py converts it for t00t), so the real conversion
path is covered too, not just the kernel.

Run: tools/fm_ref/.venv/bin/python tools/fm_ref/make_fb_bank.py
"""

import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "banks"


def pack_voice(name, feedback, outlevel=99, coarse=1, algorithm=31, mod_outlevel=None):
    """One 128-byte packed DX7 voice with feedback on OP6.

    `algorithm` 31 (= "algorithm 32", all six carriers) makes OP6 a CARRIER, so
    the render is the feedback waveform itself. `algorithm` 4 (= "algorithm 5")
    makes OP6 modulate OP5 instead, with `mod_outlevel` setting the carrier's
    level -- the same feedback loop, but heard through a modulation index rather
    than directly. Both are needed: a feedback operator's own output can be
    right while what it does as a modulator is wrong, and vice versa.
    """
    v = bytearray(128)

    for j in range(6):
        b = j * 17
        # Instant attack, hold at full, release to silence -- the envelope must
        # contribute nothing, since feedback depth is gain-dependent and a
        # moving gain would smear the thing under test.
        v[b + 0:b + 4] = bytes([99, 99, 99, 99])       # R1-R4
        v[b + 4:b + 8] = bytes([99, 99, 99, 0])        # L1-L4
        v[b + 8] = 0                                    # break point
        v[b + 9] = 0                                    # left depth
        v[b + 10] = 0                                   # right depth
        v[b + 11] = 0                                   # curves
        v[b + 12] = 7 << 3                              # rate scaling 0, detune 7 (neutral)
        v[b + 13] = 0                                   # AMS 0, KVS 0
        if j == 0:
            v[b + 14] = outlevel                        # OP6: the feedback operator
        elif j == 1 and mod_outlevel is not None:
            v[b + 14] = mod_outlevel                    # OP5: the carrier OP6 modulates
        else:
            v[b + 14] = 0
        v[b + 15] = ((coarse & 0x1F) << 1) | 0          # ratio mode
        v[b + 16] = 0                                   # fine

    v[102:106] = bytes([99, 99, 99, 99])                # pitch EG rates
    v[106:110] = bytes([50, 50, 50, 50])                # pitch EG levels: 50 == no deviation
    v[110] = algorithm                                   # 31 = all carriers; 4 = OP6(fb) -> OP5 -> OUT
    v[111] = feedback & 0x07                             # feedback depth under test, osc key sync off
    v[112:116] = bytes([0, 0, 0, 0])                     # LFO speed/delay/PMD/AMD
    v[116] = 0                                           # LFO key sync / waveform / PMS
    v[117] = 24                                          # transpose: DX7 neutral
    v[118:128] = name.ljust(10)[:10].encode("ascii")
    return bytes(v)


def bank(voices):
    """32 packed voices -> a 4104-byte sysex bulk dump."""
    while len(voices) < 32:
        voices.append(pack_voice("UNUSED", 0))
    body = b"".join(voices[:32])
    checksum = (-sum(body)) & 0x7F
    return bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00]) + body + bytes([checksum, 0xF7])


def spec_depth():
    """The 8 feedback levels, then the same 8 again at a lower output level.

    The second half matters because feedback depth in both engines scales with
    the operator's own post-gain output: the loop is `scaled_fb = (y0 + y) >>
    (fb_shift + 1)` over the operator's own recent output samples, so halving
    the level halves the modulation index. A test that only ever ran at full
    level would pass on an implementation that had the gain coupling wrong.
    """
    vs, desc = [], []
    for fb in range(8):
        vs.append(pack_voice(f"FB{fb}", fb))
        desc.append(dict(feedback=fb, outlevel=99))
    for fb in range(8):
        vs.append(pack_voice(f"FB{fb}L80", fb, outlevel=80))
        desc.append(dict(feedback=fb, outlevel=80))
    return vs, desc


def spec_depth_mod():
    """The same 8 levels again, but with OP6 feeding OP5 instead of the output.

    A feedback operator's own output can be right while what it does as a
    modulator is wrong, and vice versa, so both modes need separate coverage.
    """
    vs, desc = [], []
    for fb in range(8):
        vs.append(pack_voice(f"MFB{fb}", fb, algorithm=4, mod_outlevel=99))
        desc.append(dict(feedback=fb, mode="modulator", outlevel=99))
    for fb in range(8):
        vs.append(pack_voice(f"MFB{fb}L80", fb, outlevel=80, algorithm=4, mod_outlevel=99))
        desc.append(dict(feedback=fb, mode="modulator", outlevel=80))
    return vs, desc


SPECS = {"fb_depth": spec_depth, "fb_depth_mod": spec_depth_mod}


def main(argv):
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in SPECS.items():
        voices, desc = fn()
        path = OUT / f"{name}.syx"
        path.write_bytes(bank(list(voices)))
        print(f"wrote {path} ({len(desc)} voices under test)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
