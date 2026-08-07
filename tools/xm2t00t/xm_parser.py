""".xm file reader (#14): turns raw XM bytes into plain dataclasses.

Implements the standard, widely-documented FastTracker 2 XM layout:
module header -> pattern headers (bitmask-compressed event data) ->
instrument headers (envelopes, vibrato, per-sample headers, delta-encoded
PCM). Every "skip to the next thing" uses a size the file itself declares
(`header_size`, `InstrumentHeaderSize`, `PackedDataSize`, ...) rather than a
hardcoded offset, so minor padding differences between tracker versions
don't break the parse — the same approach real players use.

Sample PCM is kept as raw bytes throughout (never reinterpreted as signed
ints): XM's delta encoding is two's-complement wraparound addition mod 256
(or mod 65536 for 16-bit), which is bit-for-bit identical whether you treat
the bytes as signed or unsigned. See `delta_decode_8`.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Tuple


def _u8(data: bytes, pos: int) -> int:
    return data[pos]


def _s8(data: bytes, pos: int) -> int:
    v = data[pos]
    return v - 256 if v >= 128 else v


def _u16(data: bytes, pos: int) -> int:
    return struct.unpack_from("<H", data, pos)[0]


def _u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


def _str(data: bytes, pos: int, length: int) -> str:
    raw = data[pos:pos + length]
    return raw.split(b"\x00", 1)[0].rstrip().decode("latin-1", "replace")


# --- Sample PCM decode -------------------------------------------------

def delta_decode_8(raw: bytes) -> bytearray:
    """8-bit XM sample delta decode: running sum mod 256. Unsigned
    arithmetic mod 256 reproduces the correct two's-complement signed
    result bit-for-bit, so no explicit sign handling is needed."""
    out = bytearray(len(raw))
    old = 0
    for i, b in enumerate(raw):
        old = (old + b) & 0xFF
        out[i] = old
    return out


def delta_decode_16_to_8(raw: bytes) -> bytearray:
    """16-bit XM sample delta decode (running sum mod 65536 over LE int16
    words), then truncated to 8-bit by taking each sample's high byte —
    (#14 v1 always stores 8-bit PCM in the blob)."""
    n = len(raw) // 2
    out = bytearray(n)
    old = 0
    for i in range(n):
        u = struct.unpack_from("<H", raw, i * 2)[0]
        old = (old + u) & 0xFFFF
        out[i] = (old >> 8) & 0xFF
    return out


# --- Dataclasses ---------------------------------------------------------

@dataclass
class XmSample:
    name: str
    data: bytes            # decoded, 8-bit, `length` bytes (no guard yet)
    length: int             # frames
    loop_start: int
    loop_end: int
    loop_type: int           # 0 none, 1 forward, 2 ping-pong
    volume: int               # 0..64
    finetune: int              # -128..127
    panning: int                 # 0..255
    relative_note: int             # signed
    raw: bytes = b""                # pre-decode bytes as stored in the file
                                     # (delta-encoded, possibly 16-bit); kept
                                     # only for test_xm2t00t.py's independent
                                     # decode-roundtrip check, not used by
                                     # the converter itself.
    is_16bit: bool = False


@dataclass
class XmEnvelope:
    points: List[Tuple[int, int]] = field(default_factory=list)  # (tick, value)
    sustain: int = 0
    loop_start: int = 0
    loop_end: int = 0
    enabled: bool = False
    sustain_enabled: bool = False
    loop_enabled: bool = False


@dataclass
class XmInstrument:
    name: str
    sample_map: List[int]      # 96 entries, in-instrument sample index (0-based)
    samples: List[XmSample]
    vol_env: XmEnvelope
    pan_env: XmEnvelope
    vibrato_type: int
    vibrato_sweep: int
    vibrato_depth: int
    vibrato_rate: int
    volume_fadeout: int


@dataclass
class XmEvent:
    note: int          # 0 = none, 1..96, 97 = key off
    instrument: int
    volume: int          # raw volume-column byte
    effect: int            # raw XM effect type (0-35)
    param: int


@dataclass
class XmPattern:
    num_rows: int
    events: List[XmEvent]   # len == num_rows * num_channels, row-major


@dataclass
class XmSong:
    name: str
    tracker_name: str
    version: int
    num_channels: int
    restart_position: int
    linear_freq: bool
    default_tempo: int
    default_bpm: int
    order_table: List[int]
    patterns: List[XmPattern]
    instruments: List[XmInstrument]


# --- Pattern event unpacking (bitmask-byte compression) ------------------

def _unpack_pattern_events(data: bytes, num_rows: int, num_channels: int) -> List[XmEvent]:
    events: List[XmEvent] = []
    pos = 0
    n = len(data)
    for _row in range(num_rows):
        for _ch in range(num_channels):
            note = instrument = volume = effect = param = 0
            if pos < n:
                b0 = data[pos]
                if b0 & 0x80:
                    pos += 1
                    if b0 & 0x01:
                        note = data[pos]; pos += 1
                    if b0 & 0x02:
                        instrument = data[pos]; pos += 1
                    if b0 & 0x04:
                        volume = data[pos]; pos += 1
                    if b0 & 0x08:
                        effect = data[pos]; pos += 1
                    if b0 & 0x10:
                        param = data[pos]; pos += 1
                else:
                    note = b0; pos += 1
                    instrument = data[pos]; pos += 1
                    volume = data[pos]; pos += 1
                    effect = data[pos]; pos += 1
                    param = data[pos]; pos += 1
            events.append(XmEvent(note, instrument, volume, effect, param))
    return events


def _parse_pattern(data: bytes, pos: int, num_channels: int) -> Tuple[XmPattern, int]:
    header_start = pos
    header_len = _u32(data, pos)
    # packing_type = _u8(data, pos + 4)  # always 0 in practice, unused
    num_rows = _u16(data, pos + 5)
    packed_size = _u16(data, pos + 7)
    packed_start = header_start + header_len
    packed = data[packed_start:packed_start + packed_size]
    events = _unpack_pattern_events(packed, num_rows, num_channels)
    return XmPattern(num_rows, events), packed_start + packed_size


# --- Instrument / sample parsing -----------------------------------------

def _parse_envelope(points_raw: List[Tuple[int, int]], count: int, sustain: int,
                     loop_start: int, loop_end: int, type_byte: int) -> XmEnvelope:
    return XmEnvelope(
        points=points_raw[:count],
        sustain=sustain, loop_start=loop_start, loop_end=loop_end,
        enabled=bool(type_byte & 0x01),
        sustain_enabled=bool(type_byte & 0x02),
        loop_enabled=bool(type_byte & 0x04),
    )


def _parse_sample_header(data: bytes, pos: int) -> dict:
    byte_length = _u32(data, pos)
    byte_loop_start = _u32(data, pos + 4)
    byte_loop_length = _u32(data, pos + 8)
    volume = _u8(data, pos + 12)
    finetune = _s8(data, pos + 13)
    type_byte = _u8(data, pos + 14)
    panning = _u8(data, pos + 15)
    relative_note = _s8(data, pos + 16)
    name = _str(data, pos + 18, 22)

    is_16bit = bool(type_byte & 0x10)
    shift = 1 if is_16bit else 0
    return {
        "name": name,
        "byte_length": byte_length,
        "frame_length": byte_length >> shift,
        "frame_loop_start": byte_loop_start >> shift,
        "frame_loop_length": byte_loop_length >> shift,
        "loop_type": type_byte & 0x03,
        "is_16bit": is_16bit,
        "volume": volume,
        "finetune": finetune,
        "panning": panning,
        "relative_note": relative_note,
    }


def _parse_instrument(data: bytes, pos: int) -> Tuple[XmInstrument, int]:
    header_start = pos
    header_size = _u32(data, pos)
    name = _str(data, pos + 4, 22)
    num_samples = _u16(data, pos + 27)

    sample_map = [0] * 96
    vol_env = XmEnvelope()
    pan_env = XmEnvelope()
    vibrato_type = vibrato_sweep = vibrato_depth = vibrato_rate = 0
    volume_fadeout = 0
    samples: List[XmSample] = []

    if num_samples > 0:
        p = pos + 29
        sample_header_size = _u32(data, p); p += 4
        sample_map = list(data[p:p + 96]); p += 96

        vol_points = []
        for _ in range(12):
            x = _u16(data, p); y = _u16(data, p + 2)
            vol_points.append((x, y)); p += 4
        pan_points = []
        for _ in range(12):
            x = _u16(data, p); y = _u16(data, p + 2)
            pan_points.append((x, y)); p += 4

        num_vol_points = _u8(data, p); p += 1
        num_pan_points = _u8(data, p); p += 1
        vol_sustain = _u8(data, p); p += 1
        vol_loop_start = _u8(data, p); p += 1
        vol_loop_end = _u8(data, p); p += 1
        pan_sustain = _u8(data, p); p += 1
        pan_loop_start = _u8(data, p); p += 1
        pan_loop_end = _u8(data, p); p += 1
        vol_type = _u8(data, p); p += 1
        pan_type = _u8(data, p); p += 1
        vibrato_type = _u8(data, p); p += 1
        vibrato_sweep = _u8(data, p); p += 1
        vibrato_depth = _u8(data, p); p += 1
        vibrato_rate = _u8(data, p); p += 1
        volume_fadeout = _u16(data, p); p += 2

        vol_env = _parse_envelope(vol_points, num_vol_points, vol_sustain,
                                   vol_loop_start, vol_loop_end, vol_type)
        pan_env = _parse_envelope(pan_points, num_pan_points, pan_sustain,
                                   pan_loop_start, pan_loop_end, pan_type)

        # Sample headers start where the *declared* instrument header ends
        # (robust to trailing padding/reserved bytes we didn't enumerate).
        sp = header_start + header_size
        metas = []
        stride = sample_header_size if sample_header_size else 40
        for _ in range(num_samples):
            metas.append(_parse_sample_header(data, sp))
            sp += stride

        data_pos = sp
        for meta in metas:
            raw = data[data_pos:data_pos + meta["byte_length"]]
            data_pos += meta["byte_length"]
            decoded = (delta_decode_16_to_8(raw) if meta["is_16bit"]
                       else delta_decode_8(raw))
            loop_start = meta["frame_loop_start"]
            loop_len = meta["frame_loop_length"]
            samples.append(XmSample(
                name=meta["name"],
                data=bytes(decoded[:meta["frame_length"]]),
                length=meta["frame_length"],
                loop_start=loop_start,
                loop_end=loop_start + loop_len,
                loop_type=meta["loop_type"],
                volume=meta["volume"],
                finetune=meta["finetune"],
                panning=meta["panning"],
                relative_note=meta["relative_note"],
                raw=bytes(raw),
                is_16bit=meta["is_16bit"],
            ))
        next_pos = data_pos
    else:
        next_pos = header_start + header_size

    inst = XmInstrument(
        name=name, sample_map=sample_map, samples=samples,
        vol_env=vol_env, pan_env=pan_env,
        vibrato_type=vibrato_type, vibrato_sweep=vibrato_sweep,
        vibrato_depth=vibrato_depth, vibrato_rate=vibrato_rate,
        volume_fadeout=volume_fadeout,
    )
    return inst, next_pos


# --- Top level -------------------------------------------------------------

XM_MAGIC = b"Extended Module: "


def parse_xm(data: bytes) -> XmSong:
    if data[0:17] != XM_MAGIC:
        raise ValueError("not an XM file (bad magic)")
    if data[37] != 0x1A:
        raise ValueError("not an XM file (missing 0x1A marker)")

    name = _str(data, 17, 20)
    tracker_name = _str(data, 38, 20)
    version = _u16(data, 58)
    header_size = _u32(data, 60)

    song_length = _u16(data, 64)
    restart_position = _u16(data, 66)
    num_channels = _u16(data, 68)
    num_patterns = _u16(data, 70)
    num_instruments = _u16(data, 72)
    flags = _u16(data, 74)
    default_tempo = _u16(data, 76)
    default_bpm = _u16(data, 78)
    order_table = list(data[80:80 + 256])[:song_length]

    pos = 60 + header_size
    patterns = []
    for _ in range(num_patterns):
        pat, pos = _parse_pattern(data, pos, num_channels)
        patterns.append(pat)

    instruments = []
    for _ in range(num_instruments):
        inst, pos = _parse_instrument(data, pos)
        instruments.append(inst)

    return XmSong(
        name=name, tracker_name=tracker_name, version=version,
        num_channels=num_channels, restart_position=restart_position,
        linear_freq=bool(flags & 0x01),
        default_tempo=default_tempo, default_bpm=default_bpm,
        order_table=order_table, patterns=patterns, instruments=instruments,
    )


def parse_xm_file(path: str) -> XmSong:
    with open(path, "rb") as f:
        return parse_xm(f.read())
