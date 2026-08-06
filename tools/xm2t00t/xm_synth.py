"""Synthetic, byte-valid .xm builder (#17): produces small regression-corpus
modules *from checked-in code* rather than checked-in binary files.

`test_xm2t00t.py`'s corpus (`../../xm/`) is real, third-party Mod Archive
material and deliberately gitignored -- not something this repo can commit.
`tools/host_render/diff_xm.py`'s reference-diff harness needs at least one
module it can assert a tight tolerance against (a real module will legitimately
diverge the moment it uses anything beyond notes -- envelopes, effects -- none
of which src/engines/tracker/player.h implements yet). A module built here,
by contrast, is authored to stay entirely within player.h's notes-only scope,
so openmpt123 and the t00t player should agree closely: this is the "expected
result" half of issue #17's "module + expected result" acceptance criterion.

Implements just enough of the standard FastTracker 2 XM binary layout (the
inverse of xm_parser.py's read side) to produce a file real players accept:
module header, one pattern (uncompressed 5-byte cells -- no need to implement
the bitmask compression, only unpacking it), one instrument per voice with a
single 8-bit sample, delta-encoded PCM (the inverse of xm_parser.delta_decode_8).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

XM_MAGIC = b"Extended Module: "


def encode_delta_8(data: bytes) -> bytes:
    """Inverse of xm_parser.delta_decode_8: running difference mod 256, so
    decoding this back with the running sum reproduces `data` exactly."""
    out = bytearray(len(data))
    prev = 0
    for i, v in enumerate(data):
        out[i] = (v - prev) & 0xFF
        prev = v
    return bytes(out)


@dataclass
class SynthEvent:
    note: int = 0
    instrument: int = 0
    volume: int = 0   # raw XM volume-column byte (0 = none)
    effect: int = 0
    param: int = 0


@dataclass
class SynthSample:
    data: bytes             # 8-bit signed PCM, as unsigned bytes (two's complement)
    loop_start: int = 0
    loop_end: int = 0       # 0 = no loop (loop_type forced to 0)
    volume: int = 64
    panning: int = 128
    finetune: int = 0
    relative_note: int = 0
    name: str = ""

    @property
    def loop_type(self) -> int:
        return 1 if self.loop_end > self.loop_start else 0


@dataclass
class SynthInstrument:
    sample: SynthSample
    name: str = ""


@dataclass
class SynthSong:
    num_channels: int
    rows: List[List[SynthEvent]]        # len(rows) == num_rows, each row len == num_channels
    instruments: List[SynthInstrument]
    speed: int = 6                      # ticks per row
    bpm: int = 125
    linear_freq: bool = True
    name: str = "t00t synth fixture"


def _pad(s: str, n: int, fill: bytes = b" ") -> bytes:
    b = s.encode("latin-1", "replace")[:n]
    return b + fill * (n - len(b))


def _sample_header(s: SynthSample) -> bytes:
    type_byte = s.loop_type & 0x03  # bit4 (16-bit) stays 0 -- v1 is 8-bit only
    return struct.pack(
        "<IIIBbBBb",
        len(s.data), s.loop_start, max(0, s.loop_end - s.loop_start),
        s.volume & 0xFF, s.finetune, type_byte, s.panning & 0xFF, s.relative_note,
    ) + b"\x00" + _pad(s.name, 22)  # +1 reserved byte per the standard 40-byte layout


def _instrument_bytes(inst: SynthInstrument) -> bytes:
    ext = bytearray()
    ext += struct.pack("<I", 40)          # sample_header_size
    ext += bytes([0]) * 96                # sample_map: every note -> local sample 0
    ext += bytes(12 * 4)                  # vol envelope points (unused, count=0)
    ext += bytes(12 * 4)                  # pan envelope points (unused, count=0)
    ext += bytes([0, 0])                  # num_vol_points, num_pan_points
    ext += bytes([0, 0, 0])               # vol sustain/loop_start/loop_end
    ext += bytes([0, 0, 0])               # pan sustain/loop_start/loop_end
    ext += bytes([0, 0])                  # vol_type, pan_type (envelopes off)
    ext += bytes([0, 0, 0, 0])            # vibrato type/sweep/depth/rate
    ext += struct.pack("<H", 0)           # volume_fadeout
    ext += bytes(22)                      # reserved -- real FT2 files pad the extended
    # instrument header to 234 bytes here (giving the classic header_size=263 total);
    # xm_parser.py itself doesn't care (it trusts the declared header_size to skip
    # forward), but a size short of that apparently makes at least one real-world
    # player (libopenmpt/openmpt123, empirically -- see #17) treat the instrument as
    # a legacy/degenerate one with no attached samples, silently. Match the real
    # constant instead of the format's documented minimum.
    assert len(ext) == 4 + 96 + 48 + 48 + 16 + 22 == 234

    header_size = 29 + len(ext)
    head = struct.pack("<I", header_size) + _pad(inst.name, 22) + b"\x00" + struct.pack("<H", 1)
    assert len(head) == 29
    sample_hdr = _sample_header(inst.sample)
    pcm = encode_delta_8(inst.sample.data)
    return bytes(head) + bytes(ext) + sample_hdr + pcm


def _pattern_bytes(rows: List[List[SynthEvent]], num_channels: int) -> bytes:
    cells = bytearray()
    for row in rows:
        assert len(row) == num_channels
        for ev in row:
            cells += bytes([ev.note & 0xFF, ev.instrument & 0xFF, ev.volume & 0xFF,
                             ev.effect & 0xFF, ev.param & 0xFF])
    header = struct.pack("<IBHH", 9, 0, len(rows), len(cells))
    assert len(header) == 9
    return header + bytes(cells)


def build_xm(song: SynthSong) -> bytes:
    out = bytearray()
    out += XM_MAGIC
    out += _pad(song.name, 20)
    out += b"\x1a"
    out += _pad("t00t xm_synth", 20)
    out += struct.pack("<H", 0x0104)  # version

    order_table = bytes([0]) + bytes(255)  # single pattern, order 0
    body = struct.pack(
        "<HHHHHHHH",
        1,                                  # song_length (orders used)
        0,                                  # restart_position
        song.num_channels,
        1,                                  # num_patterns
        len(song.instruments),
        1 if song.linear_freq else 0,       # flags
        song.speed,
        song.bpm,
    ) + order_table
    # header_size counts itself (xm_parser.py: `pos = 60 + header_size` lands on the
    # first pattern, and 60 is where the header_size field itself starts) -- +4 for
    # that field, not just the body that follows it.
    out += struct.pack("<I", len(body) + 4)
    out += body

    out += _pattern_bytes(song.rows, song.num_channels)
    for inst in song.instruments:
        out += _instrument_bytes(inst)

    return bytes(out)


# --- Concrete fixtures -----------------------------------------------------

def _pluck_sample() -> bytes:
    """A short, one-shot, harmonically simple 8-bit waveform (a few cycles of
    a fattened sine) -- enough spectral content for a pitch/loop/retrigger
    bug to actually show up in a WAV diff, not silence or a DC value that
    would hide one. XM sample data is *signed* 8-bit, centered at 0 (mixer.h
    reads it via `const int8_t*`) -- no +128 bias, unlike unsigned PCM/WAV
    convention."""
    import math
    n = 64
    return bytes(int(100 * math.sin(2 * math.pi * i / 16.0)) & 0xFF for i in range(n))


def _pad_sample() -> bytes:
    """A short, cleanly-looping 8-bit waveform for the sustain/loop-wrap
    coverage (retrig_and_keyoff's ch1 sustains this across several rows)."""
    n = 24
    return bytes((((i * 255) // n) - 128) & 0xFF for i in range(n))  # one saw cycle


def notes_basic() -> bytes:
    """ch0: four distinct ascending notes, one per row, default sample
    volume. ch1: a single looped note held under all four rows (loop-wrap
    coverage, per-note-only, no retrigger). Exercises: multi-note sequencing,
    correct per-note increment lookup, and a sustained loop.

    Both samples use dead-center panning (128): pan.h deliberately uses an
    equal-power law (see its own header comment), which real FT2/libopenmpt
    does not necessarily match -- a permanent, by-design divergence source
    that belongs to the FT2 quirk tail (#22), not to this fixture's asserted
    tolerance. Center pan sidesteps it so this corpus tests what player.h
    actually claims to get right: notes, pitch, volume, triggers, loops."""
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=48, panning=128, name="pad")
    instruments = [SynthInstrument(pluck, "pluck"), SynthInstrument(pad, "pad")]

    def ev(note=0, inst=0, vol=0):
        return SynthEvent(note=note, instrument=inst, volume=vol)

    notes = [49, 53, 56, 61]  # C-4, E-4, G-4, C-5 (relative_note=0, so table index == note-1)
    rows = []
    for i, n in enumerate(notes):
        rows.append([ev(n, 1), ev(60, 2) if i == 0 else ev()])
    return build_xm(SynthSong(num_channels=2, rows=rows, instruments=instruments, speed=6, bpm=125))


def retrig_and_keyoff() -> bytes:
    """ch0: trigger, retrigger the same note (generation-counter coverage),
    then a key-off row. ch1: SET_VOLUME then SET_PANNING vol-column commands
    on separate triggers (both are direct sets, not per-tick effects, so
    in scope for the notes-only player -- see player.h's header comment)."""
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    instruments = [SynthInstrument(pluck, "pluck")]

    SET_VOLUME = 0x10 + 40   # vol-column encoding: 0x10..0x50 -> volume 0..64
    SET_PANNING = 0xC0 + 8   # vol-column encoding: 0xC0..0xCF -> pan 0..15

    def ev(note=0, inst=0, vol=0):
        return SynthEvent(note=note, instrument=inst, volume=vol)

    rows = [
        [ev(45, 1), ev(45, 1, SET_VOLUME)],
        [ev(),      ev()],
        [ev(45, 1), ev(45, 1, SET_PANNING)],
        [ev(97),    ev()],
    ]
    return build_xm(SynthSong(num_channels=2, rows=rows, instruments=instruments, speed=4, bpm=140))


FIXTURES = {
    "notes_basic": notes_basic,
    "retrig_and_keyoff": retrig_and_keyoff,
}
