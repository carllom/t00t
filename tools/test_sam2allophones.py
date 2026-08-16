#!/usr/bin/env python3
"""Test suite for sam2allophones.py (#71). Run: python3 tools/test_sam2allophones.py

Reference-data-gated checks look for locally-supplied S.A.M. reference
header files (e.g. `RenderTabs.h`/`SamTabs.h` from
https://github.com/s-macke/SAM or https://github.com/vidarh/SAM) in
`../sam/` (gitignored -- neither commonly-circulated reimplementation
carries a license, same reasoning as talkie2lattice.py's `../talkie/`). If
it's empty they're skipped with a clear message rather than failing, so the
synthetic-fixture and unit checks still run to completion anywhere.
"""

from __future__ import annotations

import glob
import os
import sys
import tempfile
import traceback
from typing import List

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sam2allophones as sa

HERE = os.path.dirname(os.path.abspath(__file__))
REFERENCE_DIR = os.path.join(HERE, "..", "sam")


def reference_files() -> List[str]:
    return sorted(glob.glob(os.path.join(REFERENCE_DIR, "*.h")))


# --- parse_c_array -----------------------------------------------------

def test_parse_c_array_hex_and_decimal() -> None:
    text = """
    // a leading comment, and a decoy array of the wrong type
    const int decoy[] = { 99, 99 };
    const unsigned char tab[] =
    {
        0x00 , 1 , 0x0F ,10,
        0xFF
    };
    """
    assert sa.parse_c_array(text, "tab") == [0, 1, 15, 10, 255]


def test_parse_c_array_missing_returns_empty() -> None:
    assert sa.parse_c_array("const unsigned char other[] = { 1, 2 };", "tab") == []


def test_parse_c_array_ignores_line_comments() -> None:
    text = """
    const unsigned char tab[] = {
        1, 2, // this comment's digits (3, 4) must not be parsed
        5
    };
    """
    assert sa.parse_c_array(sa._strip_line_comments(text), "tab") == [1, 2, 5]


# --- FORMANT_REFERENCE / SAM_ALLOPHONE_NAMES ----------------------------

def test_formant_reference_covers_every_allophone_name() -> None:
    assert set(sa.FORMANT_REFERENCE.keys()) == set(sa.SAM_ALLOPHONE_NAMES)
    assert len(sa.SAM_ALLOPHONE_NAMES) == sa.SAM_ALLOPHONE_COUNT
    # No duplicate names -- each index needs a distinct enum identifier in
    # the generated header.
    assert len(set(sa.SAM_ALLOPHONE_NAMES)) == sa.SAM_ALLOPHONE_COUNT


def test_bandwidth_for_f1_widens_with_f1() -> None:
    close = sa._bandwidth_for_f1(270)   # IY-like
    mid = sa._bandwidth_for_f1(490)     # ER-like
    open_ = sa._bandwidth_for_f1(730)   # AA-like
    assert close[0] < mid[0] < open_[0]


# --- convert_all: synthetic reference data ------------------------------

def _write_synthetic_reference(path: str, s_af: int = 0xF1, iy_amp: int = 0x0F) -> None:
    """A minimal synthetic reference file with all five required arrays at
    the real S.A.M. allophone count, matching the real files' own messy
    comma-per-line formatting -- exercises the parser against something
    closer to the real files' shape than a single tidy line would."""
    n = sa.SAM_ALLOPHONE_COUNT
    s_idx = sa.SAM_ALLOPHONE_NAMES.index("S")
    iy_idx = sa.SAM_ALLOPHONE_NAMES.index("IY")

    flags = [0] * n
    flags[s_idx] = s_af
    ampl1 = [0] * n
    ampl1[iy_idx] = iy_amp
    ampl2 = [0] * n
    ampl3 = [0] * n
    durations = [8] * n  # 8 * DURATION_SCALE_MS_PER_TICK(6) = 48ms for every entry

    def emit(name: str, values: List[int]) -> str:
        body = " , ".join(f"0x{v:02X}" for v in values)
        return f"const unsigned char {name}[] =\n{{\n    {body}\n}};\n\n"

    with open(path, "w") as f:
        f.write(emit("sampledConsonantFlags", flags))
        f.write(emit("amplitudeRescale", [0, 1, 2, 2, 2, 3, 3, 4, 4, 5, 6, 8, 9, 0xB, 0xD, 0xF]))
        f.write(emit("ampl1data", ampl1))
        f.write(emit("ampl2data", ampl2))
        f.write(emit("ampl3data", ampl3))
        f.write(emit("phonemeLengthTable", durations))


def test_convert_all_synthetic_classifies_sampled_vs_formant() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "synthetic.h")
        _write_synthetic_reference(path)
        rows = sa.convert_all([path])

        assert len(rows) == sa.SAM_ALLOPHONE_COUNT
        by_name = {r.name: r for r in rows}

        s_row = by_name["S"]
        assert s_row.sampled is True
        assert s_row.af == 1.0
        assert s_row.amp == (0.0, 0.0, 0.0)

        iy_row = by_name["IY"]
        assert iy_row.sampled is False
        assert iy_row.af == 0.0
        # raw ampl1data=0x0F (15) through the identity-ish rescale curve's
        # own [15]=0xF entry, normalized by 15 -> 1.0.
        assert abs(iy_row.amp[0] - 1.0) < 1e-6
        assert iy_row.amp[1] == 0.0 and iy_row.amp[2] == 0.0

        sil_row = by_name["SIL"]
        assert sil_row.sampled is False
        assert sil_row.amp == (0.0, 0.0, 0.0)

        # 8 ticks * DURATION_SCALE_MS_PER_TICK(6) = 48ms, within [MIN, MAX].
        assert s_row.duration_ms == 48


def test_duration_clamps_to_min_and_max() -> None:
    n = sa.SAM_ALLOPHONE_COUNT
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "durations.h")
        durations = [0] * n  # raw 0 -> below DURATION_MIN_MS
        durations[5] = 200   # raw 200 * 6 = 1200 -> above DURATION_MAX_MS
        with open(path, "w") as f:
            f.write(f"const unsigned char sampledConsonantFlags[] = {{ {', '.join('0' for _ in range(n))} }};\n")
            f.write("const unsigned char amplitudeRescale[] = { 0,1,2,2,2,3,3,4,4,5,6,8,9,0xB,0xD,0xF };\n")
            f.write(f"const unsigned char ampl1data[] = {{ {', '.join('0' for _ in range(n))} }};\n")
            f.write(f"const unsigned char ampl2data[] = {{ {', '.join('0' for _ in range(n))} }};\n")
            f.write(f"const unsigned char ampl3data[] = {{ {', '.join('0' for _ in range(n))} }};\n")
            f.write(f"const unsigned char phonemeLengthTable[] = {{ {', '.join(str(v) for v in durations)} }};\n")
        rows = sa.convert_all([path])
        assert rows[0].duration_ms == sa.DURATION_MIN_MS
        assert rows[5].duration_ms == sa.DURATION_MAX_MS


def test_convert_all_searches_across_multiple_files() -> None:
    """The real reference ships sampledConsonantFlags and the amplitude
    tables in different files (RenderTabs.h) from other S.A.M. data
    (SamTabs.h) -- convert_all() must find each named array regardless of
    which given file actually contains it."""
    with tempfile.TemporaryDirectory() as d:
        combined = os.path.join(d, "combined.h")
        _write_synthetic_reference(combined)
        with open(combined) as f:
            text = f.read()
        # Split roughly in half, on an array boundary, into two files.
        cut = text.index("const unsigned char ampl1data")
        path_a = os.path.join(d, "a.h")
        path_b = os.path.join(d, "b.h")
        with open(path_a, "w") as f:
            f.write(text[:cut])
        with open(path_b, "w") as f:
            f.write(text[cut:])

        rows = sa.convert_all([path_a, path_b])
        assert len(rows) == sa.SAM_ALLOPHONE_COUNT


def test_convert_all_missing_array_raises_clear_error() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "incomplete.h")
        with open(path, "w") as f:
            f.write("const unsigned char amplitudeRescale[] = { 0,1,2,2,2,3,3,4,4,5,6,8,9,0xB,0xD,0xF };\n")
        try:
            sa.convert_all([path])
            assert False, "expected SamConverterError"
        except sa.SamConverterError as exc:
            assert "sampledConsonantFlags" in str(exc)


def test_convert_all_short_array_raises_clear_error() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "short.h")
        with open(path, "w") as f:
            f.write("const unsigned char sampledConsonantFlags[] = { 0, 0, 0 };\n")
            f.write("const unsigned char amplitudeRescale[] = { 0,1,2,2,2,3,3,4,4,5,6,8,9,0xB,0xD,0xF };\n")
            f.write("const unsigned char ampl1data[] = { 0, 0, 0 };\n")
            f.write("const unsigned char ampl2data[] = { 0, 0, 0 };\n")
            f.write("const unsigned char ampl3data[] = { 0, 0, 0 };\n")
            f.write("const unsigned char phonemeLengthTable[] = { 0, 0, 0 };\n")
        try:
            sa.convert_all([path])
            assert False, "expected SamConverterError"
        except sa.SamConverterError as exc:
            assert "sampledConsonantFlags" in str(exc)


# --- render_header -------------------------------------------------------

def test_render_header_shape() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "synthetic.h")
        _write_synthetic_reference(path)
        rows = sa.convert_all([path])
        header = sa.render_header(rows, [path])

        assert "#pragma once" in header
        assert "enum SamAllophoneId : uint8_t {" in header
        assert f"SAM_ALLOPHONES[SAM_ALLOPHONE_DATA_COUNT]" in header
        assert "SAM_ALLOPHONE_DATA_NAMES[SAM_ALLOPHONE_DATA_COUNT]" in header
        assert header.count("SAM_ID_") == sa.SAM_ALLOPHONE_COUNT  # one enumerator per allophone
        for name in sa.SAM_ALLOPHONE_NAMES:
            assert f'"{name}"' in header


# --- reference-data-gated ------------------------------------------------

def test_reference_file(paths: List[str]) -> None:
    rows = sa.convert_all(paths)
    assert len(rows) == sa.SAM_ALLOPHONE_COUNT
    for r in rows:
        for v in r.amp:
            assert 0.0 <= v <= 1.0
        assert r.af in (0.0, 1.0)


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

    run("parse_c_array: hex and decimal literals, mixed whitespace", test_parse_c_array_hex_and_decimal)
    run("parse_c_array: missing array name returns empty list", test_parse_c_array_missing_returns_empty)
    run("parse_c_array: line comments' digits are not parsed", test_parse_c_array_ignores_line_comments)
    run("FORMANT_REFERENCE covers every SAM_ALLOPHONE_NAMES entry, no duplicates", test_formant_reference_covers_every_allophone_name)
    run("_bandwidth_for_f1: bandwidth widens with F1", test_bandwidth_for_f1_widens_with_f1)
    run("convert_all: synthetic data classifies sampled vs. formant allophones", test_convert_all_synthetic_classifies_sampled_vs_formant)
    run("convert_all: duration_ms clamps to [MIN, MAX]", test_duration_clamps_to_min_and_max)
    run("convert_all: finds each array regardless of which given file has it", test_convert_all_searches_across_multiple_files)
    run("convert_all: missing required array fails loud, names the array", test_convert_all_missing_array_raises_clear_error)
    run("convert_all: under-length required array fails loud", test_convert_all_short_array_raises_clear_error)
    run("render_header: every SamAllophoneTarget field reaches the output", test_render_header_shape)

    files = reference_files()
    if not files:
        print(f"(reference dir {REFERENCE_DIR} has no .h files - skipping reference-dependent "
              "checks; populate it with locally-supplied S.A.M. reference headers, e.g. "
              "RenderTabs.h/SamTabs.h from https://github.com/s-macke/SAM, to run them)")
    else:
        run(f"{', '.join(os.path.basename(p) for p in files)}: full reference conversion", test_reference_file, files)

    print()
    print(f"{passed} passed, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
