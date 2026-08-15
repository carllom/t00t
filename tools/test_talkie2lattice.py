#!/usr/bin/env python3
"""Test suite for talkie2lattice.py (#64). Run: python3 tools/test_talkie2lattice.py

Corpus-dependent checks look for Talkie vocab `.ino`/`.h` files in `../talkie/`
(gitignored -- not committed, since the TMS5220 ROM-derived word data has no
clear redistribution license, same reasoning as syx2patch.py's `../syx/`).
Populate it yourself (e.g. from https://github.com/going-digital/Talkie's
`examples/` vocab sketches) to run those checks; if it's empty they're
skipped with a clear message rather than failing, so the synthetic-fixture
and unit checks still run to completion anywhere.
"""

from __future__ import annotations

import glob
import os
import sys
import tempfile
import traceback
from typing import List

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import talkie2lattice as tl

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_DIR = os.path.join(HERE, "..", "talkie")


def corpus_files() -> List[str]:
    return sorted(glob.glob(os.path.join(CORPUS_DIR, "*.ino")) + glob.glob(os.path.join(CORPUS_DIR, "*.h")))


# --- Bit primitive (synthetic, no corpus needed) ----------------------------

class BitWriter:
    """Test-only inverse of BitReader: builds a byte string that
    BitReader.get_bits() will read back exactly the fields written, in
    order. Bits are accumulated MSB-first in logical read order, then each
    8-bit chunk is bit-reversed to produce the physical stored byte -- the
    same reversal BitReader itself undoes on read, confirmed by
    test_bit_reader_writer_roundtrip() below."""

    def __init__(self) -> None:
        self._bits: List[int] = []

    def put_bits(self, value: int, n: int) -> None:
        for i in range(n - 1, -1, -1):
            self._bits.append((value >> i) & 1)

    def to_bytes(self) -> bytes:
        bits = list(self._bits)
        while len(bits) % 8 != 0:
            bits.append(0)
        out = bytearray()
        for i in range(0, len(bits), 8):
            logical_byte = 0
            for b in bits[i:i + 8]:
                logical_byte = (logical_byte << 1) | b
            out.append(tl.BitReader._reverse_byte(logical_byte))
        return bytes(out)


def test_reverse_byte_self_inverse() -> None:
    for v in [0x00, 0xFF, 0b10000000, 0b00000001, 0b10110010, 0x3C]:
        assert tl.BitReader._reverse_byte(tl.BitReader._reverse_byte(v)) == v
    assert tl.BitReader._reverse_byte(0b10000000) == 0b00000001
    assert tl.BitReader._reverse_byte(0b11110000) == 0b00001111
    assert tl.BitReader._reverse_byte(0b10110010) == 0b01001101


def test_bit_reader_writer_roundtrip() -> None:
    fields = [(0xF, 4), (1, 1), (0x2A, 6), (0x1F, 5), (0x0, 5), (0x9, 4), (0x1, 1), (0xFF, 8), (0x3, 2)]
    w = BitWriter()
    for value, bits in fields:
        w.put_bits(value, bits)
    data = w.to_bytes()
    r = tl.BitReader(data)
    for value, bits in fields:
        assert r.get_bits(bits) == value, (value, bits)


def test_get_bits_exhaustion_fails_loud() -> None:
    r = tl.BitReader(b"\x00")
    r.get_bits(4)  # fine: still 4 bits left in the one byte
    try:
        r.get_bits(6)  # needs a second byte that doesn't exist
        assert False, "expected exhaustion to raise"
    except tl.TalkieConverterError as exc:
        assert "exhaust" in str(exc).lower()


# --- Chip tables --------------------------------------------------------

def test_tms5220_table_shapes_and_stability() -> None:
    t = tl.TMS5220_TABLES
    assert len(t.energy) == 16
    assert len(t.period) == 64
    assert t.period[0] == 0  # index 0 is the unvoiced/period-less sentinel
    assert len(t.k_bits) == len(t.k_tables) == len(t.k_scale) == 10
    for i in range(10):
        assert len(t.k_tables[i]) == (1 << t.k_bits[i])
        for raw in t.k_tables[i]:
            k = raw / t.k_scale[i]
            assert -1.0 < k < 1.0, (i, raw, k)


# --- Frame decode (synthetic bitstreams) ---------------------------------

def _write_normal_frame(w: BitWriter, tables: tl.ChipTables, energy_idx: int, period_idx: int,
                         k_indices) -> None:
    """k_indices: list of 10 table indices (K1..K10). Only K1-K4 are written
    if the period is 0 (unvoiced), matching the real chip's own frame shape."""
    w.put_bits(energy_idx, tables.energy_bits)
    w.put_bits(0, 1)  # repeat = 0
    w.put_bits(period_idx, tables.period_bits)
    n = 10 if tables.period[period_idx] else 4
    for i in range(n):
        w.put_bits(k_indices[i], tables.k_bits[i])


def _write_repeat_frame(w: BitWriter, tables: tl.ChipTables, energy_idx: int, period_idx: int) -> None:
    w.put_bits(energy_idx, tables.energy_bits)
    w.put_bits(1, 1)  # repeat = 1
    w.put_bits(period_idx, tables.period_bits)


def _write_rest_frame(w: BitWriter, tables: tl.ChipTables) -> None:
    w.put_bits(0, tables.energy_bits)


def _write_stop_frame(w: BitWriter, tables: tl.ChipTables) -> None:
    w.put_bits((1 << tables.energy_bits) - 1, tables.energy_bits)


def test_decode_stop_only_word() -> None:
    t = tl.TMS5220_TABLES
    w = BitWriter()
    _write_stop_frame(w, t)
    frames = tl.decode_word(w.to_bytes(), t)
    assert len(frames) == 1
    assert frames[0].gain == 0.0
    assert frames[0].pitch_hz == 0.0
    assert all(k == 0.0 for k in frames[0].k)


def test_decode_voiced_frame_reads_all_ten_k() -> None:
    t = tl.TMS5220_TABLES
    k_idx = [3, 7, 2, 9, 1, 5, 4, 0, 6, 7]
    energy_idx, period_idx = 9, 20  # period[20] = 41 != 0 -> voiced
    assert t.period[period_idx] != 0
    w = BitWriter()
    _write_normal_frame(w, t, energy_idx, period_idx, k_idx)
    _write_stop_frame(w, t)
    frames = tl.decode_word(w.to_bytes(), t)
    assert len(frames) == 2  # the voiced frame, then the stop frame
    f = frames[0]
    expected_k = [t.k_tables[i][k_idx[i]] / t.k_scale[i] for i in range(10)]
    for got, want in zip(f.k, expected_k):
        assert abs(got - want) < 1e-9
    assert abs(f.gain - t.energy[energy_idx] / 255.0) < 1e-9
    assert abs(f.pitch_hz - tl.NATIVE_RATE_HZ / t.period[period_idx]) < 1e-9


def test_decode_unvoiced_frame_reads_only_k1_to_k4() -> None:
    t = tl.TMS5220_TABLES
    # period index 0 -> unvoiced: only K1-K4 present in the bitstream.
    k_idx = [5, 5, 5, 5, 0, 0, 0, 0, 0, 0]  # K5-K10 indices irrelevant, not written
    energy_idx, period_idx = 4, 0
    assert t.period[period_idx] == 0

    # A second, voiced frame follows -- if the unvoiced frame's bit count
    # were wrong, this second frame's fields would be read misaligned and
    # fail to match what was written.
    k_idx2 = [1, 2, 3, 4, 5, 6, 7, 0, 1, 2]
    energy_idx2, period_idx2 = 12, 40
    assert t.period[period_idx2] != 0

    w = BitWriter()
    _write_normal_frame(w, t, energy_idx, period_idx, k_idx)
    _write_normal_frame(w, t, energy_idx2, period_idx2, k_idx2)
    _write_stop_frame(w, t)
    frames = tl.decode_word(w.to_bytes(), t)
    assert len(frames) == 3

    f0 = frames[0]
    assert f0.pitch_hz == 0.0
    for i in range(4):
        assert abs(f0.k[i] - t.k_tables[i][k_idx[i]] / t.k_scale[i]) < 1e-9

    f1 = frames[1]
    expected_k2 = [t.k_tables[i][k_idx2[i]] / t.k_scale[i] for i in range(10)]
    for got, want in zip(f1.k, expected_k2):
        assert abs(got - want) < 1e-9
    assert abs(f1.pitch_hz - tl.NATIVE_RATE_HZ / t.period[period_idx2]) < 1e-9


def test_decode_rest_frame_holds_previous_k_and_pitch() -> None:
    t = tl.TMS5220_TABLES
    k_idx = [2, 4, 6, 8, 1, 3, 5, 7, 0, 2]
    energy_idx, period_idx = 8, 30
    w = BitWriter()
    _write_normal_frame(w, t, energy_idx, period_idx, k_idx)
    _write_rest_frame(w, t)
    _write_stop_frame(w, t)
    frames = tl.decode_word(w.to_bytes(), t)
    assert len(frames) == 3
    voiced, rest = frames[0], frames[1]
    assert rest.gain == 0.0
    assert rest.pitch_hz == voiced.pitch_hz
    assert rest.k == voiced.k


def test_decode_repeat_frame_reuses_k_with_new_energy_and_pitch() -> None:
    t = tl.TMS5220_TABLES
    k_idx = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
    energy_idx, period_idx = 5, 15
    w = BitWriter()
    _write_normal_frame(w, t, energy_idx, period_idx, k_idx)
    repeat_energy_idx, repeat_period_idx = 11, 50
    _write_repeat_frame(w, t, repeat_energy_idx, repeat_period_idx)
    _write_stop_frame(w, t)
    frames = tl.decode_word(w.to_bytes(), t)
    assert len(frames) == 3
    normal, repeat = frames[0], frames[1]
    assert repeat.k == normal.k  # coefficients held, not re-read
    assert abs(repeat.gain - t.energy[repeat_energy_idx] / 255.0) < 1e-9
    assert abs(repeat.pitch_hz - tl.NATIVE_RATE_HZ / t.period[repeat_period_idx]) < 1e-9
    assert repeat.gain != normal.gain
    assert repeat.pitch_hz != normal.pitch_hz


# --- Vocab source parsing (synthetic fixtures) -------------------------

_FIXTURE_SOURCE = """\
// Talkie library
#include "talkie.h"

uint8_t spHELLO[] PROGMEM = {0x0F,0x24,0xAB};
//uint8_t spCOMMENTED[]      PROGMEM ={0x01,0x02,0x03};
uint8_t spMIXED_RADIX[] PROGMEM = {0x0A, 10, 0xff};
"""


def test_parse_vocab_file_commented_and_plain_declarations() -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".ino", delete=False) as f:
        f.write(_FIXTURE_SOURCE)
        path = f.name
    try:
        entries = tl.parse_vocab_file(path)
        names = {name: data for name, data in entries}
        assert set(names) == {"HELLO", "COMMENTED", "MIXED_RADIX"}
        assert names["HELLO"] == bytes([0x0F, 0x24, 0xAB])
        assert names["COMMENTED"] == bytes([0x01, 0x02, 0x03])
        assert names["MIXED_RADIX"] == bytes([0x0A, 10, 0xFF])  # 10 decimal == 0x0A
    finally:
        os.unlink(path)


def test_parse_vocab_file_rejects_out_of_range_byte() -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".ino", delete=False) as f:
        f.write("uint8_t spBAD[] PROGMEM = {0x01, 300, 0x02};\n")
        path = f.name
    try:
        try:
            tl.parse_vocab_file(path)
            assert False, "expected an out-of-range byte value to raise"
        except tl.TalkieConverterError as exc:
            assert "out-of-range" in str(exc).lower()
    finally:
        os.unlink(path)


def test_convert_all_dedupes_identically_named_words() -> None:
    src = "uint8_t spTEST[] PROGMEM = {0x0F};\n"  # a single stop-only frame
    paths = []
    try:
        for _ in range(2):
            with tempfile.NamedTemporaryFile("w", suffix=".ino", delete=False) as f:
                f.write(src)
                paths.append(f.name)
        words, warnings, skipped = tl._convert_all(paths)
        assert skipped == 0
        idents = [w.ident for w in words]
        assert idents == ["TEST", "TEST_2"]
        assert all(w.name == "TEST" for w in words)
    finally:
        for p in paths:
            os.unlink(p)


def test_convert_all_records_decode_failure_as_skip_not_abort() -> None:
    # A "normal" frame header claiming a 6-bit period field that the 1-byte
    # buffer can't actually supply -- the whole file must still convert its
    # other, valid word rather than aborting on this one's bad data.
    src = (
        "uint8_t spGOOD[] PROGMEM = {0x0F};\n"          # valid: stop-only
        "uint8_t spTRUNCATED[] PROGMEM = {0x05};\n"      # energy=0 needs no more bits...
    )
    # Force an actual truncation: energy index 5 (not rest/stop) demands a
    # repeat bit + 6-bit period + K1-4 that a single byte cannot hold.
    with tempfile.NamedTemporaryFile("w", suffix=".ino", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        words, warnings, skipped = tl._convert_all([path])
        names = {w.name for w in words}
        assert "GOOD" in names
        assert "TRUNCATED" not in names
        assert skipped == 1
        assert any("TRUNCATED" in w for w in warnings)
    finally:
        os.unlink(path)


def test_render_header_field_count_matches_lattice_frame() -> None:
    """LatticeFrame (lattice.h) has exactly 3 fields: k[10], gain, pitch_hz.
    A frame emitted with one too few/many would silently mis-initialise
    the C++ aggregate -- this counts the emitted braces structurally."""
    t = tl.TMS5220_TABLES
    w = BitWriter()
    _write_stop_frame(w, t)
    words = [tl.WordOut(name="X", ident="X", frames=tl.decode_word(w.to_bytes(), t), source="<test>")]
    text = tl.render_header(words, ["<test>"])
    frame_line = next(l.strip() for l in text.splitlines() if l.strip().startswith("{ {"))
    # "{ { k0f, k1f, ... k9f }, gainf, pitchf },"
    assert frame_line.count("f,") + frame_line.count("f }") >= 12  # 10 k's + gain + pitch_hz, loosely
    inner = frame_line[frame_line.index("{", frame_line.index("{") + 1) + 1 : frame_line.rindex("}")]
    # inner now holds "k0f, k1f, ..., k9f }, gainf, pitchf"
    k_part, rest = inner.split("}", 1)
    k_count = len([v for v in k_part.split(",") if v.strip()])
    assert k_count == 10, k_count
    rest_count = len([v for v in rest.split(",") if v.strip()])
    assert rest_count == 2, rest_count  # gain, pitch_hz


# --- Corpus-dependent checks (real Talkie vocab files, gitignored) -----

def test_corpus_file(path: str) -> None:
    words, warnings, skipped = tl._convert_all([path])
    assert len(words) > 0
    for w in words:
        assert len(w.frames) > 0
        for fr in w.frames:
            assert len(fr.k) == 10
            assert all(-1.0 < k < 1.0 for k in fr.k)
            assert 0.0 <= fr.gain <= 1.0
            assert fr.pitch_hz == 0.0 or fr.pitch_hz > 0.0


def corpus_paths() -> List[str]:
    return corpus_files()


def main() -> None:
    passed = 0
    failures = []

    def run(name, fn, *args):
        nonlocal passed
        try:
            fn(*args)
            print(f"PASS  {name}")
            passed += 1
        except AssertionError as exc:
            print(f"FAIL  {name}: {exc}")
            failures.append(name)
        except Exception:
            print(f"ERROR {name}")
            traceback.print_exc()
            failures.append(name)

    run("reverse_byte is self-inverse (and matches hand-checked cases)", test_reverse_byte_self_inverse)
    run("BitReader/BitWriter round-trip a mixed field sequence", test_bit_reader_writer_roundtrip)
    run("BitReader.get_bits fails loud on exhaustion", test_get_bits_exhaustion_fails_loud)
    run("TMS5220_TABLES: shapes correct, every |k|<1", test_tms5220_table_shapes_and_stability)
    run("decode_word: stop-only word", test_decode_stop_only_word)
    run("decode_word: voiced frame reads all 10 K", test_decode_voiced_frame_reads_all_ten_k)
    run("decode_word: unvoiced frame reads only K1-K4 (bit alignment)", test_decode_unvoiced_frame_reads_only_k1_to_k4)
    run("decode_word: rest frame holds previous K/pitch, gain=0", test_decode_rest_frame_holds_previous_k_and_pitch)
    run("decode_word: repeat frame reuses K with new energy/pitch", test_decode_repeat_frame_reuses_k_with_new_energy_and_pitch)
    run("parse_vocab_file: commented and plain declarations, mixed radix", test_parse_vocab_file_commented_and_plain_declarations)
    run("parse_vocab_file: out-of-range byte value fails loud", test_parse_vocab_file_rejects_out_of_range_byte)
    run("_convert_all: identically-named words across files get unique idents", test_convert_all_dedupes_identically_named_words)
    run("_convert_all: one word's decode failure is a recorded skip, not an abort", test_convert_all_records_decode_failure_as_skip_not_abort)
    run("render_header: every LatticeFrame field reaches the output", test_render_header_field_count_matches_lattice_frame)

    files = corpus_paths()
    if not files:
        print(f"(corpus dir {CORPUS_DIR} has no .ino/.h files - skipping corpus-dependent "
              "checks; populate it with Talkie vocab source files, e.g. from "
              "https://github.com/going-digital/Talkie/tree/master/Talkie/examples, "
              "to run them)")
    else:
        for path in files:
            run(f"{os.path.basename(path)}: full file conversion", test_corpus_file, path)

    print()
    print(f"{passed} passed, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
