#!/usr/bin/env python3
"""Write the DX7 .syx import fixtures shared by the host converter's tests
and the editor's.

    dx7_vectors.py <out.txt>

Builds a synthetic 32-voice bulk dump and a single-voice dump (no Yamaha
data), converts both with tools/syx2patch.py, and writes each input and the
stored Patches the converter makes of it as hex lines, `name byte byte ...`,
in the format of the firmware's golden SysEx vectors:

    bank_syx        the 32-voice bulk dump (4104 bytes)
    bank_patch_NN   voice NN of it as a stored FM Patch
    voice_syx       a single-voice dump of bank voice VOICE_INDEX
    voice_patch     it as a stored FM Patch

test_syx2patch.py checks the file is what the converter makes today;
tools/editor/test/dx7.test.js checks the editor converts the same inputs to
the same bytes. Regenerate it after changing the converter.

The voices are unpacked DX7 voices (the 155-byte single-voice layout,
"VCED"), varied per voice and operator so a field read from the wrong place
or an operator taken in the wrong order changes the output. Voice i uses
algorithm i, so all 32 appear; a few carry the bytes real factory banks have
outside the front-panel range (EG bytes and fine frequency above 99).
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import syx2patch as sp

VOICE_INDEX = 7
VCED_SIZE = sp.VCED_SIZE

NAMES = ["INIT VOICE", "E.PIANO 1", " LEAD", "BRASS", "A", "STRINGS  2", "BASS-1~", "TUB BELLS",
         "", "ORGAN", "MARIMBA", "SYN-LEAD 3", "HARP", "FLUTE 1", "PIANO   4", "CLAV"]


def voice_vced(i: int) -> bytes:
    """Voice i of the fixture bank, unpacked: per operator (OP6 first) R1-4,
    L1-4, break point, left/right depth, left/right curve, rate scaling, AMS,
    velocity, level, mode, coarse, fine, detune; then pitch EG R1-4, L1-4,
    algorithm, feedback, oscillator sync, LFO speed, delay, PMD, AMD, sync,
    wave, PMS, transpose, and the 10-character name."""
    v = []
    for j in range(6):
        v += [(i * 7 + j * 13 + s * 5) % 100 for s in range(4)]
        v += [(i * 11 + j * 3 + s * 17) % 100 for s in range(4)]
        v += [(i + j * 9) % 100, (i * 3 + j) % 100, (i * 5 + j * 2) % 100, (i + j) % 4, (i + 2 * j) % 4,
              (i + j) % 8, (i * j) % 4, (i + 3 * j) % 8, (50 + i + j * 7) % 100, 1 if (i + j) % 7 == 0 else 0,
              (i + j * 5) % 32, (i * 3 + j * 11) % 100, (i + j) % 15]
    v += [(i * 9 + s * 21) % 100 for s in range(4)] + [(i * 13 + s * 7) % 100 for s in range(4)]
    v += [i, i % 8, i % 2, (i * 3) % 100, (i * 5) % 100, (i * 7) % 100, (i * 11) % 100, (i + 1) % 2,
          i % 6, i % 8, (i * 3) % 49]
    v += list((NAMES[i % len(NAMES)] + " " * 10)[:10].encode("ascii"))
    if i == 1:
        v[0] = 127        # OP6 R1 above the panel range
        v[21 + 4] = 120   # OP5 L1
    if i == 2:
        v[5 * 21 + 18] = 0    # OP1 coarse 0 ...
        v[5 * 21 + 19] = 100  # ... fine 100
    if i == 3:
        v[2 * 21 + 17] = 1    # OP4 fixed frequency, fine past 99
        v[2 * 21 + 18] = 3
        v[2 * 21 + 19] = 127
    assert len(v) == VCED_SIZE
    return bytes(v)


def pack_vced(v: bytes) -> bytes:
    """An unpacked voice in the 128-byte packed form of a bulk dump (the
    DX7 MIDI Data Format Sheet; Dexed's Cartridge::packProgram)."""
    b = bytearray(128)
    for j in range(6):
        o, p = j * 21, j * 17
        b[p:p + 11] = v[o:o + 11]
        b[p + 11] = v[o + 11] | (v[o + 12] << 2)
        b[p + 12] = v[o + 13] | (v[o + 20] << 3)
        b[p + 13] = v[o + 14] | (v[o + 15] << 2)
        b[p + 14] = v[o + 16]
        b[p + 15] = v[o + 17] | (v[o + 18] << 1)
        b[p + 16] = v[o + 19]
    b[102:110] = v[126:134]
    b[110] = v[134]
    b[111] = v[135] | (v[136] << 3)
    b[112:116] = v[137:141]
    b[116] = v[141] | (v[142] << 1) | (v[143] << 4)
    b[117] = v[144]
    b[118:128] = v[145:155]
    return bytes(b)


def wrap_bulk(voices: list) -> bytes:
    """32 packed voices as a 32-voice bulk dump."""
    assert len(voices) == 32
    payload = b"".join(voices)
    assert len(payload) == 4096
    checksum = (-sum(payload)) & 0x7F
    return b"\xF0\x43\x00\x09\x20\x00" + payload + bytes([checksum]) + b"\xF7"


def wrap_single(vced: bytes) -> bytes:
    """An unpacked voice as a single-voice dump."""
    assert len(vced) == VCED_SIZE
    checksum = (-sum(vced)) & 0x7F
    return b"\xF0\x43\x00\x00\x01\x1B" + vced + bytes([checksum]) + b"\xF7"


def fixture_bank() -> bytes:
    return wrap_bulk([pack_vced(voice_vced(i)) for i in range(32)])


def fixture_voice() -> bytes:
    return wrap_single(voice_vced(VOICE_INDEX))


def converted(syx: bytes) -> list:
    """The converter's stored Patches for a .syx file's bytes."""
    return [sp.stored_patch(v) for v in sp.read_syx_voices(syx, "<fixture>", [])]


def _line(name: str, data: bytes) -> str:
    return name + " " + " ".join(f"{x:02X}" for x in data)


def render() -> str:
    bank, voice = fixture_bank(), fixture_voice()
    lines = ["# DX7 .syx import fixtures -- written by tools/dx7_vectors.py from tools/syx2patch.py's",
             "# conversion. Synthetic voices, no Yamaha data. Do not hand-edit.",
             _line("bank_syx", bank)]
    lines += [_line(f"bank_patch_{n:02d}", p) for n, p in enumerate(converted(bank))]
    lines += [_line("voice_syx", voice), _line("voice_patch", converted(voice)[0])]
    return "\n".join(lines) + "\n"


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "w") as f:
        f.write(render())


if __name__ == "__main__":
    main()
