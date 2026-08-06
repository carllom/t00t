"""Canonical t00t tracker blob format — the single source of truth (#14).

Every multi-byte struct the converter emits is declared here once, as a
list of `Field`s in on-disk order. `blob_writer.py` uses `StructDef.pack()`
to build the blob; `test_xm2t00t.py` uses `StructDef.unpack()` to read it
back for self-consistency checks; `xm2t00t.py gen-header` walks the same
definitions to emit the C++ mirror (`blob_format.h`) — so the Python and
C++ views of the layout cannot drift out of sync with each other.

All multi-byte header/table fields are `uint32_t` (or `int32_t`) — no
`#pragma pack` needed on the device side, every field lands on a natural
4-byte boundary. Only the flat per-event array (`Event`, six `uint8_t`s)
and the two per-instrument/per-sample byte tables intentionally stay
byte-sized, since those repeat thousands of times and the size adds up.

All offsets recorded in the blob are byte offsets from the start of the
blob (offset 0 == the first byte of `SongHeader`), never pointers — the
blob is meant to be `memcpy`'d into SRAM and read in place.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any, Dict, Tuple

MAGIC = b"T00T"
VERSION = 1

# tracker.md "Fixed-Point Formats": Q18.14 position caps a sample at this
# many frames (256 KB at 8-bit); tracker.md "Memory Strategy": realistically
# 350-400 KB of SRAM is available for sample data after code/stacks/DMA/mixer
# scratch. Both are enforced by blob_writer.py, not the device.
MAX_SAMPLE_FRAMES = 262_144
DEFAULT_SRAM_BUDGET_BYTES = 380 * 1024

# One increment table entry per playable XM note (file note values 1..96,
# stored 0-based here: index 0 == XM note 1 == C-0).
NOTES_PER_TABLE = 96
# Per-instrument note -> in-instrument sample index map (XM note values 1..96).
SAMPLE_MAP_SIZE = 96


@dataclass(frozen=True)
class Field:
    name: str
    ctype: str  # one of _SCALAR_FMT's keys, or 'char' for a raw byte/string blob
    count: int = 1
    comment: str = ""


_SCALAR_FMT = {
    "uint8_t": "B",
    "int8_t": "b",
    "uint16_t": "H",
    "int16_t": "h",
    "uint32_t": "I",
    "int32_t": "i",
}
_CTYPE_SIZE = {"uint8_t": 1, "int8_t": 1, "uint16_t": 2, "int16_t": 2,
               "uint32_t": 4, "int32_t": 4, "char": 1}


def _field_fmt(f: Field) -> str:
    if f.ctype == "char":
        return f"<{f.count}s"
    if f.count == 1:
        return f"<{_SCALAR_FMT[f.ctype]}"
    return f"<{f.count}{_SCALAR_FMT[f.ctype]}"


def _field_size(f: Field) -> int:
    return f.count * _CTYPE_SIZE[f.ctype]


@dataclass(frozen=True)
class StructDef:
    name: str
    fields: Tuple[Field, ...]
    doc: str = ""

    @property
    def size(self) -> int:
        return sum(_field_size(f) for f in self.fields)

    def offset_of(self, field_name: str) -> int:
        off = 0
        for f in self.fields:
            if f.name == field_name:
                return off
            off += _field_size(f)
        raise KeyError(field_name)

    def pack(self, **values: Any) -> bytes:
        out = bytearray()
        for f in self.fields:
            v = values[f.name]
            if f.ctype == "char":
                b = v if isinstance(v, (bytes, bytearray)) else str(v).encode("latin-1", "replace")
                out += struct.pack(_field_fmt(f), bytes(b[: f.count]))
            elif f.count == 1:
                out += struct.pack(_field_fmt(f), v)
            else:
                seq = list(v)
                if len(seq) != f.count:
                    raise ValueError(f"{self.name}.{f.name}: expected {f.count} values, got {len(seq)}")
                out += struct.pack(_field_fmt(f), *seq)
        assert len(out) == self.size, f"{self.name}: packed {len(out)} bytes, expected {self.size}"
        return bytes(out)

    def unpack(self, data: bytes, offset: int = 0) -> Dict[str, Any]:
        out: Dict[str, Any] = {}
        pos = offset
        for f in self.fields:
            sz = _field_size(f)
            chunk = data[pos:pos + sz]
            if f.ctype == "char":
                out[f.name] = chunk.split(b"\x00", 1)[0]
            elif f.count == 1:
                out[f.name] = struct.unpack(_field_fmt(f), chunk)[0]
            else:
                out[f.name] = list(struct.unpack(_field_fmt(f), chunk))
            pos += sz
        return out


# --- Struct definitions (on-disk order == field declaration order) --------

SONG_HEADER = StructDef("SongHeader", (
    Field("magic", "char", 4, '"T00T"'),
    Field("version", "uint32_t", comment="format version, currently 1"),
    Field("num_channels", "uint32_t"),
    Field("freq_table", "uint32_t", comment="0 = Amiga (period table), 1 = Linear"),
    Field("num_orders", "uint32_t"),
    Field("num_patterns", "uint32_t"),
    Field("num_instruments", "uint32_t"),
    Field("num_samples", "uint32_t"),
    Field("default_tempo", "uint32_t", comment="ticks per row"),
    Field("default_bpm", "uint32_t"),
    Field("restart_order", "uint32_t"),
    Field("order_table_offset", "uint32_t", comment="-> uint8_t[num_orders]"),
    Field("pattern_table_offset", "uint32_t", comment="-> PatternHeader[num_patterns]"),
    Field("instrument_table_offset", "uint32_t", comment="-> InstrumentHeader[num_instruments]"),
    Field("sample_table_offset", "uint32_t", comment="-> SampleHeader[num_samples]"),
    Field("sample_data_offset", "uint32_t", comment="-> start of concatenated PCM + guard bytes"),
    Field("sample_data_bytes", "uint32_t", comment="total PCM+guard bytes; the SRAM residency size"),
    Field("total_size", "uint32_t", comment="whole blob size, for a bounds-checked copy"),
    Field("name", "char", 32),
), doc="Fixed at blob offset 0.")

PATTERN_HEADER = StructDef("PatternHeader", (
    Field("num_rows", "uint32_t"),
    Field("event_offset", "uint32_t", comment="-> Event[num_rows * num_channels]"),
))

EVENT = StructDef("Event", (
    Field("note", "uint8_t", comment="0 = none, 1..96 = C-0..B-7, 97 = key off"),
    Field("instrument", "uint8_t", comment="0 = none, else 1-based instrument index"),
    Field("vol_effect", "uint8_t", comment="VolEffect enum"),
    Field("vol_param", "uint8_t"),
    Field("effect", "uint8_t", comment="Effect enum"),
    Field("effect_param", "uint8_t", comment="raw param byte (both nibbles, or low nibble for decomposed Exy/Xxy)"),
), doc="6 bytes, flat array of num_rows * num_channels, row-major.")

ENVELOPE_POINT = StructDef("EnvelopePoint", (
    Field("tick", "uint16_t"),
    Field("value", "uint16_t", comment="0..64"),
))

INSTRUMENT_HEADER = StructDef("InstrumentHeader", (
    Field("sample_map_offset", "uint32_t", comment="-> uint8_t[96], note -> in-instrument sample index, 0xFF = none"),
    Field("num_samples", "uint32_t"),
    Field("sample_index_offset", "uint32_t", comment="-> uint32_t[num_samples], global sample indices"),
    Field("vol_env_offset", "uint32_t", comment="-> EnvelopePoint[vol_env_count], 0 if unused"),
    Field("vol_env_count", "uint32_t"),
    Field("vol_env_sustain", "uint32_t"),
    Field("vol_env_loop_start", "uint32_t"),
    Field("vol_env_loop_end", "uint32_t"),
    Field("vol_env_flags", "uint32_t", comment="bit0 enabled, bit1 sustain, bit2 loop"),
    Field("pan_env_offset", "uint32_t"),
    Field("pan_env_count", "uint32_t"),
    Field("pan_env_sustain", "uint32_t"),
    Field("pan_env_loop_start", "uint32_t"),
    Field("pan_env_loop_end", "uint32_t"),
    Field("pan_env_flags", "uint32_t"),
    Field("vibrato_type", "uint32_t"),
    Field("vibrato_sweep", "uint32_t"),
    Field("vibrato_depth", "uint32_t"),
    Field("vibrato_rate", "uint32_t"),
    Field("volume_fadeout", "uint32_t"),
    Field("name", "char", 24),
))

SAMPLE_HEADER = StructDef("SampleHeader", (
    Field("length", "uint32_t", comment="frames, not counting the guard byte"),
    Field("loop_start", "uint32_t"),
    Field("loop_end", "uint32_t"),
    Field("loop_type", "uint32_t", comment="0 = none, 1 = forward, 2 = ping-pong"),
    Field("finetune", "int32_t"),
    Field("relative_note", "int32_t"),
    Field("default_volume", "uint32_t", comment="0..64"),
    Field("default_panning", "int32_t", comment="0..255, XM convention"),
    Field("data_offset", "uint32_t", comment="-> int8_t[length + 1] (PCM + guard) in the sample data region"),
    Field("note_increments_offset", "uint32_t", comment="-> uint32_t[96] Q8.24 phase increments, one per note"),
    Field("name", "char", 24),
))

ALL_STRUCTS = (SONG_HEADER, PATTERN_HEADER, EVENT, ENVELOPE_POINT, INSTRUMENT_HEADER, SAMPLE_HEADER)


def _cpp_field_line(f: Field) -> str:
    array = f"[{f.count}]" if f.count > 1 else ""
    line = f"    {f.ctype} {f.name}{array};"
    if f.comment:
        line += f"  // {f.comment}"
    return line


def generate_cpp_header() -> str:
    """Mirror of this file's struct/enum layout, consumed by src/engines/tracker/
    player.h (#17) and, once #18 wires the device side up, the real on-device
    player too. Regenerate via `xm2t00t.py gen-header` whenever a struct here
    changes."""
    # Local import: effects.py has no dependency on this module, so this
    # stays a one-way edge (blob_format -> effects), not a cycle.
    from effects import Effect, VolEffect

    lines = [
        "// GENERATED by tools/xm2t00t/blob_format.py:generate_cpp_header() — do not hand-edit.",
        "// Mirrors blob_format.py, the format's source of truth. Regenerate with:",
        "//   python3 tools/xm2t00t/xm2t00t.py gen-header src/engines/tracker/blob_format.h",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        f"static constexpr uint32_t T00T_BLOB_MAGIC_LEN = {len(MAGIC)};",
        f"static constexpr char T00T_BLOB_MAGIC[{len(MAGIC) + 1}] = \"{MAGIC.decode()}\";",
        f"static constexpr uint32_t T00T_BLOB_VERSION = {VERSION};",
        f"static constexpr uint32_t T00T_MAX_SAMPLE_FRAMES = {MAX_SAMPLE_FRAMES};  // Q18.14 cap",
        f"static constexpr uint32_t T00T_NOTES_PER_TABLE = {NOTES_PER_TABLE};",
        f"static constexpr uint32_t T00T_SAMPLE_MAP_SIZE = {SAMPLE_MAP_SIZE};",
        "",
    ]

    for py_enum, cpp_name in ((Effect, "Effect"), (VolEffect, "VolEffect")):
        lines.append(f"enum class {cpp_name} : uint8_t {{")
        for member in py_enum:
            lines.append(f"    {member.name} = {member.value},")
        lines.append("};")
        lines.append("")

    for sd in ALL_STRUCTS:
        lines.append(f"// {sd.doc}" if sd.doc else f"// {sd.name}")
        lines.append(f"struct {sd.name} {{")
        for f in sd.fields:
            lines.append(_cpp_field_line(f))
        lines.append("};")
        lines.append(f"static_assert(sizeof({sd.name}) == {sd.size}, \"{sd.name} size drifted from blob_format.py\");")
        lines.append("")

    return "\n".join(lines) + "\n"
