#!/usr/bin/env python3
"""talkie2lattice.py -- TMS5220 "Talkie" vocab source(s) -> lattice_words.h
(module_speech.md "LPC Lattice Tract").

    talkie2lattice.py convert <in.ino> [in2.ino ...] <out.h>
        Parse one or more Talkie-format vocab source files (`.ino`/`.h`,
        `uint8_t sp<NAME>[] PROGMEM = {...};` declarations -- see below), decode
        every word's TMS5220 bitstream into #63's lattice.h frame format
        (LatticeFrame: k[10]/gain/pitch_hz, one per 25 ms native sub-frame),
        and write <out.h>: a device+host header (`enum LatticeWordId`,
        `LATTICE_WORD_COUNT`, one `const LatticeFrame[]` per word, `const
        LatticeWord LATTICE_WORDS[]`, `const char *LATTICE_WORD_NAMES[]`).
        Words from multiple files are laid out in the given file order.
        Never wired into CMakeLists.txt or committed -- the vendor-ROM-derived
        word/coefficient data this emits has no clear redistribution license,
        following the DX7 `.syx` bulk-dump precedent (tools/syx2patch.py):
        only this converter is committed, the source vocab files are supplied
        locally and gitignored (`talkie/`).

    talkie2lattice.py dump <in.ino> [in2.ino ...]
        Parse and print a per-word summary (name, frame count, source file)
        without writing a header.

Source format: Talkie (https://github.com/going-digital/Talkie, Peter Knight,
GPLv2) ships its word corpus as Arduino `.ino` files, one `uint8_t
sp<NAME>[] PROGMEM = { 0x.., 0x.., ... };` byte array per word -- some source
files ship every declaration already active, others (e.g. the TI-99/4A
vocabulary specifically) ship every declaration commented out with a leading
`//`, purely to fit Arduino flash budgets that don't apply to this host-side
converter. This parser accepts both forms identically, so the *entire*
corpus converts regardless of which form a given source file uses.

Bitstream format and decode tables: the TMS5220 LPC-10-style frame format
(4-bit energy, 1-bit repeat, 6-bit pitch period, then 5/5/4/4-bit K1-K4 and,
for voiced frames, 4/4/4/3/3/3-bit K5-K10, LSB-reversed-per-byte bit packing)
is the chip's own hardware behavior, not any one implementation's creative
work -- the same reasoning that already applies to the DX7 sysex byte layout
this project ported from Dexed (tools/syx2patch.py). The specific table
values (`TMS5220_TABLES` below) were cross-referenced against the Talkie
library's own decoder (`talkie.cpp`, GPLv2, Peter Knight) for correctness,
then re-expressed here as this project's own data, not copied from that
file. `ChipTables`/`decode_word()` below take those tables as a parameter
rather than hardcoding TMS5220 specifics inline, so a TMS5100 (Speak &
Spell) decode path -- different tables, same frame structure -- can be added
as a second `ChipTables` instance later without restructuring this pipeline.

Frame timing: the TMS5220's real cadence is one coefficient frame per 25 ms
(200 samples at its native 8 kHz rate) -- exactly lattice.h's own
SPEECH_LATTICE_FRAME_SAMPLES assumption, so no retiming happens here: one
decoded bitstream frame becomes one LatticeFrame, unchanged.

Gain calibration: the TMS5220's raw energy table (`TMS5220_TABLES.energy`,
0-255) is normalized to `LatticeFrame.gain`'s 0.0-1.0 range by dividing by
255 -- a different unit and a different absolute calibration than #63's own
hand-built LATTICE_TEST_WORD (whose gain values come from a Levinson-Durbin
analysis of unrelated synthetic data, then scaled again in render.h's
SPEECH_LATTICE_GAIN_BOOST). Real corpus playback may need its own gain
retuning once heard on hardware -- this converter's job is a faithful,
literal decode of the chip's own energy value, not a loudness match to
LATTICE_TEST_WORD's own bring-up-only calibration.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from typing import Dict, List, Tuple

# TMS5220 native rate (module_speech.md's SPEECH_LATTICE_RATE) and frame
# period -- both hardware facts, not tool-specific choices.
NATIVE_RATE_HZ = 8000
FRAME_PERIOD_MS = 25

LATTICE_ORDER = 10


class TalkieConverterError(Exception):
    """Raised for anything this converter cannot represent, or any vocab/
    bitstream data that fails validation -- callers print str(exc) and exit
    non-zero without writing a header. Same fail-loud, no-silent-
    approximation contract as syx2patch.py's Syx2PatchError."""


# ---------------------------------------------------------------------------
# Chip decode tables: TMS5220-specific, isolated behind ChipTables so a
# second chip (TMS5100) can be added as a second instance later.
# ---------------------------------------------------------------------------

@dataclass
class ChipTables:
    """Everything decode_word() needs to know about one chip's bitstream
    format: the per-field bit widths (energy is always 4 bits, repeat always
    1 -- both fixed by the TMS52xx family's frame header shape -- but the K
    stage widths and the lookup tables themselves are chip-specific) and the
    quantization tables index -> real value. `k_bits[i]`/`k_tables[i]` are
    parallel, index 0..9 = K1..K10. `k_scale[i]` converts a raw table entry
    into a `|k|<1` reflection coefficient (32768 for the two 16-bit stages,
    128 for the eight 8-bit stages -- the TMS5220's own Q15/Q7 fixed-point
    convention, not an arbitrary choice)."""
    energy_bits: int
    energy: List[int]
    period_bits: int
    period: List[int]
    k_bits: List[int]        # length 10
    k_tables: List[List[int]]  # length 10
    k_scale: List[float]     # length 10


# TMS5220 tables, cross-referenced against the Talkie library's talkie.cpp
# (see module docstring) and re-expressed here. Every entry, scaled by its
# k_scale below, lands strictly inside (-1, 1) -- checked again at import
# time by _validate_tables() rather than trusted on sight.
TMS5220_TABLES = ChipTables(
    energy_bits=4,
    energy=[0, 2, 3, 4, 5, 7, 10, 15, 20, 32, 41, 57, 81, 114, 161, 255],
    period_bits=6,
    period=[
        0, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
        31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 45, 47, 49,
        51, 53, 54, 57, 59, 61, 63, 66, 69, 71, 73, 77, 79, 81, 85, 87,
        92, 95, 99, 102, 106, 110, 115, 119, 123, 128, 133, 138, 143, 149, 154, 160,
    ],
    k_bits=[5, 5, 4, 4, 4, 4, 4, 3, 3, 3],
    k_tables=[
        # K1 (5 bits, 32 entries, 16-bit signed / 32768)
        [-32064, -31872, -31808, -31680, -31552, -31424, -31232, -30848,
         -30592, -30336, -30016, -29696, -29376, -28928, -28480, -27968,
         -26368, -24256, -21632, -18368, -14528, -10048, -5184, 0,
         5184, 10048, 14528, 18368, 21632, 24256, 26368, 27968],
        # K2 (5 bits, 32 entries, 16-bit signed / 32768)
        [-20992, -19328, -17536, -15552, -13440, -11200, -8768, -6272,
         -3712, -1088, 1536, 4160, 6720, 9216, 11584, 13824,
         15936, 17856, 19648, 21248, 22656, 24000, 25152, 26176,
         27072, 27840, 28544, 29120, 29632, 30080, 30464, 32384],
        # K3 (4 bits, 16 entries, 8-bit signed / 128)
        [-110, -97, -83, -70, -56, -43, -29, -16, -2, 11, 25, 38, 52, 65, 79, 92],
        # K4 (4 bits, 16 entries, 8-bit signed / 128)
        [-82, -68, -54, -40, -26, -12, 1, 15, 29, 43, 57, 71, 85, 99, 113, 126],
        # K5 (4 bits, 16 entries, 8-bit signed / 128)
        [-82, -70, -59, -47, -35, -24, -12, -1, 11, 23, 34, 46, 57, 69, 81, 92],
        # K6 (4 bits, 16 entries, 8-bit signed / 128)
        [-64, -53, -42, -31, -20, -9, 3, 14, 25, 36, 47, 58, 69, 80, 91, 102],
        # K7 (4 bits, 16 entries, 8-bit signed / 128)
        [-77, -65, -53, -41, -29, -17, -5, 7, 19, 31, 43, 55, 67, 79, 90, 102],
        # K8 (3 bits, 8 entries, 8-bit signed / 128)
        [-64, -40, -16, 7, 31, 55, 79, 102],
        # K9 (3 bits, 8 entries, 8-bit signed / 128)
        [-64, -44, -24, -4, 16, 37, 57, 77],
        # K10 (3 bits, 8 entries, 8-bit signed / 128)
        [-51, -33, -15, 4, 22, 32, 59, 77],
    ],
    k_scale=[32768.0, 32768.0, 128.0, 128.0, 128.0, 128.0, 128.0, 128.0, 128.0, 128.0],
)


def _validate_tables(tables: ChipTables) -> None:
    if len(tables.energy) != (1 << tables.energy_bits):
        raise TalkieConverterError(
            f"energy table has {len(tables.energy)} entries, expected {1 << tables.energy_bits} "
            f"for {tables.energy_bits}-bit indexing")
    if len(tables.period) != (1 << tables.period_bits):
        raise TalkieConverterError(
            f"period table has {len(tables.period)} entries, expected {1 << tables.period_bits} "
            f"for {tables.period_bits}-bit indexing")
    if not (len(tables.k_bits) == len(tables.k_tables) == len(tables.k_scale) == LATTICE_ORDER):
        raise TalkieConverterError(
            f"k_bits/k_tables/k_scale must each have {LATTICE_ORDER} entries (one per lattice stage)")
    for i in range(LATTICE_ORDER):
        expected = 1 << tables.k_bits[i]
        if len(tables.k_tables[i]) != expected:
            raise TalkieConverterError(
                f"K{i + 1} table has {len(tables.k_tables[i])} entries, expected {expected} "
                f"for {tables.k_bits[i]}-bit indexing")
        for raw in tables.k_tables[i]:
            k = raw / tables.k_scale[i]
            if not (-1.0 < k < 1.0):
                raise TalkieConverterError(
                    f"K{i + 1} table entry {raw} -> k={k:.4f} is outside (-1, 1) -- "
                    f"a bad table would silently produce an unstable lattice filter")


_validate_tables(TMS5220_TABLES)


# ---------------------------------------------------------------------------
# Bitstream reader: LSB-reversed-per-byte, sliding 1-8-bit window. Re-expresses
# the TMS5220's own bit-packing convention (see module docstring) -- the ROMs
# feeding this chip were serial, not byte-wide, so each byte's bit order is
# reversed before the sliding window reads across it.
# ---------------------------------------------------------------------------

class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.byte_idx = 0
        self.bit_idx = 0  # 0-7: bits already consumed from the current byte

    @staticmethod
    def _reverse_byte(a: int) -> int:
        a &= 0xFF
        a = ((a >> 4) | (a << 4)) & 0xFF
        a = ((a & 0xCC) >> 2) | ((a & 0x33) << 2)
        a = ((a & 0xAA) >> 1) | ((a & 0x55) << 1)
        return a & 0xFF

    def get_bits(self, n: int) -> int:
        if self.byte_idx >= len(self.data):
            raise TalkieConverterError("bitstream exhausted while reading a frame field")
        window = self._reverse_byte(self.data[self.byte_idx]) << 8
        if self.bit_idx + n > 8:
            if self.byte_idx + 1 >= len(self.data):
                raise TalkieConverterError("bitstream exhausted mid-field (needs a second byte)")
            window |= self._reverse_byte(self.data[self.byte_idx + 1])
        window = (window << self.bit_idx) & 0xFFFF
        value = window >> (16 - n)
        self.bit_idx += n
        if self.bit_idx >= 8:
            self.bit_idx -= 8
            self.byte_idx += 1
        return value


# ---------------------------------------------------------------------------
# Frame decode: bitstream -> a list of #63's LatticeFrame values.
# ---------------------------------------------------------------------------

@dataclass
class LatticeFrameOut:
    k: List[float]    # length 10, |k[i]| < 1
    gain: float        # 0.0-1.0, this chip's energy value normalized
    pitch_hz: float     # 0 = unvoiced/silent


ENERGY_REST = 0  # a short silence: energy 0, everything else holds


def decode_word(data: bytes, tables: ChipTables, word_name: str = "") -> List[LatticeFrameOut]:
    """Decodes one word's raw bitstream into a list of LatticeFrameOut, one
    per 25 ms native sub-frame, faithfully reproducing the chip's own three
    frame kinds:

      - REST (energy index 0): a short silence -- only the 4-bit energy
        field is read; K/pitch hold whatever the previous frame left them at
        (silent either way, since gain 0 zeroes the excitation regardless of
        what K says).
      - STOP (energy index `2**energy_bits - 1`): ends the word. Emitted as
        one final all-zero, silent frame (matching the real chip, which
        still spends 25 ms on this frame before falling silent) rather than
        being dropped -- a decoded word's last frame is always genuine
        silence, not an abrupt cut.
      - Normal frame: energy + repeat bit + pitch period, then (unless
        `repeat` is set, which reuses the previous frame's K unchanged)
        K1-K4, plus K5-K10 for voiced frames only (pitch period != 0) --
        exactly mirroring how much data the real chip's decoder reads for
        each case.
    """
    stop_index = (1 << tables.energy_bits) - 1
    reader = BitReader(data)
    frames: List[LatticeFrameOut] = []

    cur_k = [0.0] * LATTICE_ORDER
    cur_pitch_hz = 0.0

    while True:
        energy_idx = reader.get_bits(tables.energy_bits)

        if energy_idx == ENERGY_REST:
            frames.append(LatticeFrameOut(k=list(cur_k), gain=0.0, pitch_hz=cur_pitch_hz))
            continue

        if energy_idx == stop_index:
            frames.append(LatticeFrameOut(k=[0.0] * LATTICE_ORDER, gain=0.0, pitch_hz=0.0))
            break

        gain = tables.energy[energy_idx] / 255.0
        repeat = reader.get_bits(1)
        period_idx = reader.get_bits(tables.period_bits)
        period = tables.period[period_idx]
        cur_pitch_hz = (NATIVE_RATE_HZ / period) if period else 0.0

        if not repeat:
            for i in range(4):  # K1-K4: every frame, voiced or not
                bits = tables.k_bits[i]
                raw = tables.k_tables[i][reader.get_bits(bits)]
                cur_k[i] = raw / tables.k_scale[i]
            if period:  # voiced: K5-K10
                for i in range(4, LATTICE_ORDER):
                    bits = tables.k_bits[i]
                    raw = tables.k_tables[i][reader.get_bits(bits)]
                    cur_k[i] = raw / tables.k_scale[i]
            # Unvoiced normal frames leave K5-K10 at whatever they last were
            # (irrelevant to the actual sound -- an unvoiced source excites
            # the same order-10 filter, but real word data's unvoiced
            # segments are typically fricatives/stops where the high-order
            # coefficients matter less) -- matches the real chip exactly,
            # which never reads or resets them in this case either.

        frames.append(LatticeFrameOut(k=list(cur_k), gain=gain, pitch_hz=cur_pitch_hz))

    return frames


# ---------------------------------------------------------------------------
# Vocab source parsing: `uint8_t sp<NAME>[] PROGMEM = {...};`, commented or not.
# ---------------------------------------------------------------------------

_VOCAB_ENTRY_RE = re.compile(
    r"^\s*(?://)?\s*(?:const\s+)?u?int8_t\s+sp([A-Za-z0-9_]+)\s*\[\]\s*PROGMEM\s*=\s*\{([^}]*)\}\s*;",
    re.MULTILINE,
)


def parse_vocab_file(path: str) -> List[Tuple[str, bytes]]:
    """Extracts every `sp<NAME>[]` word declaration from a Talkie-format
    vocab source file, whether or not it's commented out (see module
    docstring's "Source format" -- some files ship everything commented
    purely for Arduino flash budget, which does not apply here). Byte list
    entries may be `0x..` hex or plain decimal, matching what real vocab
    files use interchangeably."""
    with open(path, "r", encoding="ascii", errors="replace") as f:
        text = f.read()
    out: List[Tuple[str, bytes]] = []
    for m in _VOCAB_ENTRY_RE.finditer(text):
        name = m.group(1)
        raw_list = m.group(2)
        try:
            values = [int(tok.strip(), 0) for tok in raw_list.split(",") if tok.strip()]
        except ValueError as exc:
            raise TalkieConverterError(f"{path}: word {name!r} has a non-numeric byte list entry: {exc}")
        for v in values:
            if not (0 <= v <= 255):
                raise TalkieConverterError(f"{path}: word {name!r} has an out-of-range byte value {v}")
        out.append((name, bytes(values)))
    return out


# ---------------------------------------------------------------------------
# Word conversion + identifier handling.
# ---------------------------------------------------------------------------

@dataclass
class WordOut:
    name: str        # original word name (e.g. "HELLO")
    ident: str        # unique C identifier suffix (name, or name_2/_3/... on collision)
    frames: List[LatticeFrameOut]
    source: str        # basename of the file this word came from


def _make_unique(base: str, used: Dict[str, int]) -> str:
    if base not in used:
        used[base] = 1
        return base
    used[base] += 1
    return f"{base}_{used[base]}"


def _convert_all(paths: List[str], tables: ChipTables = TMS5220_TABLES) -> Tuple[List[WordOut], List[str], int]:
    if not paths:
        raise TalkieConverterError("no input files given")

    warnings: List[str] = []
    used_idents: Dict[str, int] = {}
    words: List[WordOut] = []
    skipped = 0
    for path in paths:
        entries = parse_vocab_file(path)
        if not entries:
            warnings.append(f"{os.path.basename(path)}: no `sp<NAME>[] PROGMEM` declarations found")
        label = os.path.basename(path)
        for name, data in entries:
            if len(data) == 0:
                warnings.append(f"{label}: word {name!r} has an empty byte array -- skipped")
                skipped += 1
                continue
            try:
                frames = decode_word(data, tables, name)
            except TalkieConverterError as exc:
                warnings.append(f"{label}: word {name!r} failed to decode ({exc}) -- skipped")
                skipped += 1
                continue
            ident = _make_unique(name.upper(), used_idents)
            words.append(WordOut(name=name, ident=ident, frames=frames, source=label))
    return words, warnings, skipped


# ---------------------------------------------------------------------------
# lattice_words.h emission.
# ---------------------------------------------------------------------------

def _format_frame(f: LatticeFrameOut) -> str:
    ks = ", ".join(f"{v:.6f}f" for v in f.k)
    return f"{{ {{ {ks} }}, {f.gain:.6f}f, {f.pitch_hz:.4f}f }}"


def render_header(words: List[WordOut], source_paths: List[str]) -> str:
    sources = ", ".join(os.path.basename(p) for p in source_paths)
    total_frames = sum(len(w.frames) for w in words)
    lines = [
        f"// GENERATED by tools/talkie2lattice.py convert from {sources} -- do not hand-edit.",
        "// Every word is a TMS5220 LPC bitstream decoded into lattice.h's LatticeFrame",
        "// format (reflection coefficients k[10], gain, pitch_hz), one entry per 25 ms",
        "// native sub-frame -- see talkie2lattice.py's module docstring for the decode",
        "// and gain-calibration notes.",
        "#pragma once",
        "",
        '#include "lattice.h"',
        "",
        "enum LatticeWordId : uint16_t {",
    ]
    for w in words:
        lines.append(f"    LATTICE_WORD_{w.ident},")
    lines.append("    LATTICE_WORD_COUNT")
    lines.append("};")
    lines.append("")
    for w in words:
        lines.append(f"inline constexpr LatticeFrame LATTICE_WORD_FRAMES_{w.ident}[] = {{")
        for fr in w.frames:
            lines.append(f"    {_format_frame(fr)},")
        lines.append("};")
    lines.append("")
    lines.append("inline constexpr LatticeWord LATTICE_WORDS[LATTICE_WORD_COUNT] = {")
    for w in words:
        lines.append(
            f"    {{ LATTICE_WORD_FRAMES_{w.ident}, "
            f"sizeof(LATTICE_WORD_FRAMES_{w.ident}) / sizeof(LatticeFrame) }},"
        )
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr const char *LATTICE_WORD_NAMES[LATTICE_WORD_COUNT] = {")
    for w in words:
        lines.append(f'    "{w.name}",')
    lines.append("};")
    lines.append("")
    lines.append(f"// {len(words)} words, {total_frames} frames total ({total_frames * 25} ms of audio at 25 ms/frame).")
    lines.append("")
    return "\n".join(lines)


def cmd_convert(args: argparse.Namespace) -> None:
    try:
        words, warnings, skipped = _convert_all(args.vocab)
    except TalkieConverterError as exc:
        print(f"talkie2lattice: {exc}", file=sys.stderr)
        sys.exit(1)

    if not words:
        print("talkie2lattice: no words survived conversion -- nothing to write", file=sys.stderr)
        sys.exit(1)

    with open(args.header, "w") as f:
        f.write(render_header(words, args.vocab))

    total_frames = sum(len(w.frames) for w in words)
    print(f"Wrote {args.header} ({len(words)}/{len(words) + skipped} words converted, "
          f"{skipped} skipped, {total_frames} frames total)")
    for w in warnings:
        print(f"  note: {w}")


def cmd_dump(args: argparse.Namespace) -> None:
    try:
        words, warnings, skipped = _convert_all(args.vocab)
    except TalkieConverterError as exc:
        print(f"talkie2lattice: {exc}", file=sys.stderr)
        sys.exit(1)

    for w in words:
        print(f"  {w.ident:24s} {len(w.frames):4d} frames  ({w.source}, \"{w.name}\")")
    print(f"{len(words)} converted, {skipped} skipped")
    for warning in warnings:
        print(f"  note: {warning}")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="talkie2lattice")
    sub = parser.add_subparsers(dest="command", required=True)

    p_convert = sub.add_parser("convert", help="parse Talkie vocab source file(s) and emit lattice_words.h")
    p_convert.add_argument("vocab", nargs="+", help="input Talkie vocab .ino/.h file(s)")
    p_convert.add_argument("header", help="output path for lattice_words.h")
    p_convert.set_defaults(func=cmd_convert)

    p_dump = sub.add_parser("dump", help="parse and print a per-word summary, write nothing")
    p_dump.add_argument("vocab", nargs="+", help="input Talkie vocab .ino/.h file(s)")
    p_dump.set_defaults(func=cmd_dump)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
