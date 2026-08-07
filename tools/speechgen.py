#!/usr/bin/env python3
"""speechgen.py -- phoneme CSV -> phonemes.h + host verification data (#32).

    speechgen.py gen <phonemes.csv> <phonemes.h> [--meta-out <phoneme_meta.h>]
        Parse and validate the phoneme CSV, then write:
          - <phonemes.h>: the device+host header (enum Phoneme, PHONEME_COUNT,
            const PhonemeDef PHONEME_TARGETS[], const char* PHONEME_LABELS[]).
            This is what ships in flash.
          - <phoneme_meta.h> (default: alongside phonemes.h's basename in
            tools/host_render/): PHONEME_CLASS[], host-only classification
            used by the verification harness (tools/host_render/
            render_speech.cpp) to know which rows are vowels/fricatives/
            nasals without hardcoding a phoneme list there. Never linked
            into the device build.

    speechgen.py gen-vowel-ref <vowel_reference.csv> <vowel_reference.h>
        Parse the independent published-vowel-space reference table and
        write it as a small host-only header (tools/host_render/
        vowel_reference.h) the harness compares its Goertzel-measured F1/F2
        against -- see vowel_reference.csv's header comment for why this is
        a separate file from phonemes.csv rather than reusing its targets.

Same host-side-authoring-and-validation split as tools/xm2t00t (keeping the
letter-to-sound rules engine host-side, per speech.md "Host Tooling", is the
same decision for the same reason): the device ships a table, all the
authoring smarts stay on the host where a CSV row typo fails a Python script
instead of turning into a pole outside the unit circle at runtime.

CSV schema (see tools/speech_phonemes.csv for the full ~48-row set):
    symbol   -- becomes enum value PH_<symbol>; must be a valid C identifier
                suffix (letters/digits/underscore, not digit-first), unique
    label    -- short display string (device-shipped, PHONEME_LABELS[]),
                e.g. "/p-cl/"
    F1..F5   -- cascade formant frequencies, Hz (uint16_t range)
    B1..B5   -- cascade formant bandwidths, Hz, quantized to Hz/4 (uint8_t:
                0-1020 Hz representable)
    fric_F, fric_B, nasal_F, nasal_B -- parallel-branch targets, same units
                as F/B above (#29's fricative/nasal resonators; not part of
                speech.md's original PhonemeDef sketch -- see phoneme_def.h)
    av, af, an -- excitation mix, 0.0-1.0 (quantized to a 0-255 byte)
    duration_ms -- 0-255, unread by anything yet (P3 sequencer)
    flags    -- '|'-separated names from PLOSIVE, STOP_CLOSURE,
                TRANSITION_FAST, SUSTAINABLE (speech.md / #30), or empty
    notes    -- free text, not emitted, for the human maintainer's citation
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List

SPEECH_FORMANTS = 5

FLAG_BITS = {
    "PLOSIVE": 0x01,
    "STOP_CLOSURE": 0x02,
    "TRANSITION_FAST": 0x04,
    "SUSTAINABLE": 0x08,
}


class PhonemeCsvError(Exception):
    """Raised for a CSV row that fails validation -- callers print str(exc)
    and exit non-zero without writing a header. No silent truncation: every
    path that would otherwise let a value wrap a uint8_t/uint16_t raises
    here instead."""


@dataclass
class PhonemeRow:
    symbol: str
    label: str
    F: List[float]
    B: List[float]
    fric_F: float
    fric_B: float
    nasal_F: float
    nasal_B: float
    av: float
    af: float
    an: float
    duration_ms: int
    flags: int
    # Host-verification-only classification, derived from the symbol/flags
    # below (never emitted into phonemes.h) -- see classify_row().
    pclass: str = field(default="OTHER")


# Classes the host harness (render_speech.cpp) groups phonemes into so its
# per-class acoustic checks (vowel F1/F2, fricative distinguishability,
# nasal pole, mixed excitation) scale to however many rows the CSV has,
# instead of a fixed name list. Order matters only for the generated enum's
# readability; PHONEME_CLASS_NAMES below must stay in this same order since
# render_speech.cpp indexes PHONEME_CLASS[] as a raw int into a matching enum.
CLASSES = ["VOWEL", "NASAL", "FRICATIVE_VL", "FRICATIVE_VD",
           "STOP_CLOSURE", "STOP_BURST", "OTHER"]


def classify_row(r: "PhonemeRow") -> str:
    # Derived purely from the CSV's own DSP fields, not from the symbol --
    # a name-based lookup here would be exactly the kind of hardcoded list
    # that drifts as rows are added, which is what this generator exists to
    # avoid downstream (render_speech.cpp). VOWEL also catches the
    # approximants (L/R/W/Y): av>0, af=0, an=0 is a purely-voiced cascade
    # with no parallel branch active either way, so the same F1/F2-vs-target
    # check method applies to both -- they only differ in whether an
    # independent published reference exists for them (vowel_reference.csv
    # has none for L/R/W/Y, so that half of the check just skips).
    if r.flags & FLAG_BITS["STOP_CLOSURE"]:
        return "STOP_CLOSURE"
    if r.flags & FLAG_BITS["PLOSIVE"]:
        return "STOP_BURST"
    if r.an > 0.0:
        return "NASAL"
    if r.af > 0.0 and r.av == 0.0:
        return "FRICATIVE_VL"
    if r.af > 0.0 and r.av > 0.0:
        return "FRICATIVE_VD"
    if r.av > 0.0 and r.af == 0.0 and r.an == 0.0:
        return "VOWEL"
    return "OTHER"


def _sanitize_ident(name: str) -> str:
    out = "".join(c if (c.isalnum() or c == "_") else "_" for c in name)
    if not out or out[0].isdigit():
        out = "_" + out
    return out


def _round_hz4(hz: float, field_name: str, row_num: int) -> int:
    # Hz -> Hz/4 byte. Explicit round-half-away-from-zero (not Python's
    # round-half-to-even) so the mapping is obvious from the CSV value
    # alone; the +-2 Hz this can introduce doesn't move a resonator's
    # center frequency (only its bandwidth), so it's inaudible and doesn't
    # affect any F1/F2 acceptance check.
    q = int((hz / 4.0) + 0.5)
    if hz < 0 or q > 255:
        raise PhonemeCsvError(
            f"row {row_num}: {field_name}={hz} Hz is out of range -- max representable "
            f"is 1020 Hz (255 * 4) before it wraps a uint8_t and becomes a pole outside "
            f"the unit circle at runtime. Fix the CSV value; this is not silently truncated."
        )
    return q


def _check_u16(hz: float, field_name: str, row_num: int) -> int:
    if hz < 0 or hz > 65535:
        raise PhonemeCsvError(f"row {row_num}: {field_name}={hz} Hz is out of uint16_t range")
    return int(round(hz))


def _check_byte01(v: float, field_name: str, row_num: int) -> int:
    if v < 0.0 or v > 1.0:
        raise PhonemeCsvError(
            f"row {row_num}: {field_name}={v} is out of range -- must be 0.0-1.0 "
            f"(scaled to a 0-255 byte); {v*255:.1f} would wrap a uint8_t."
        )
    return int((v * 255.0) + 0.5)


def parse_flags(text: str, row_num: int) -> int:
    text = (text or "").strip()
    if not text:
        return 0
    bits = 0
    for name in text.split("|"):
        name = name.strip()
        if not name:
            continue
        if name not in FLAG_BITS:
            raise PhonemeCsvError(
                f"row {row_num}: unknown flag '{name}' -- must be one of "
                f"{', '.join(FLAG_BITS)} (typo would otherwise silently drop the bit)"
            )
        bits |= FLAG_BITS[name]
    return bits


def load_csv(path: str) -> List[PhonemeRow]:
    rows: List[PhonemeRow] = []
    seen_symbols: Dict[str, int] = {}
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        required = ["symbol", "label", "F1", "F2", "F3", "F4", "F5",
                    "B1", "B2", "B3", "B4", "B5", "fric_F", "fric_B",
                    "nasal_F", "nasal_B", "av", "af", "an", "duration_ms", "flags"]
        missing = [c for c in required if c not in (reader.fieldnames or [])]
        if missing:
            raise PhonemeCsvError(f"{path}: missing column(s): {', '.join(missing)}")

        for row_num, row in enumerate(reader, start=2):  # header is row 1
            symbol = row["symbol"].strip()
            if not symbol:
                continue  # blank line
            if not (symbol[:1].isalpha() or symbol[:1] == "_"):
                raise PhonemeCsvError(f"row {row_num}: symbol '{symbol}' must start with a letter or underscore")
            if any(not (c.isalnum() or c == "_") for c in symbol):
                raise PhonemeCsvError(f"row {row_num}: symbol '{symbol}' must be letters/digits/underscore only")
            if symbol in seen_symbols:
                raise PhonemeCsvError(
                    f"row {row_num}: duplicate symbol '{symbol}' (first seen at row {seen_symbols[symbol]})"
                )
            seen_symbols[symbol] = row_num

            F = [_check_u16(float(row[f"F{i+1}"]), f"F{i+1}", row_num) for i in range(SPEECH_FORMANTS)]
            B_hz = [float(row[f"B{i+1}"]) for i in range(SPEECH_FORMANTS)]
            B = [_round_hz4(b, f"B{i+1}", row_num) for i, b in enumerate(B_hz)]

            fric_F = _check_u16(float(row["fric_F"]), "fric_F", row_num)
            fric_B = _round_hz4(float(row["fric_B"]), "fric_B", row_num)
            nasal_F = _check_u16(float(row["nasal_F"]), "nasal_F", row_num)
            nasal_B = _round_hz4(float(row["nasal_B"]), "nasal_B", row_num)

            av_f, af_f, an_f = float(row["av"]), float(row["af"]), float(row["an"])
            av = _check_byte01(av_f, "av", row_num)
            af = _check_byte01(af_f, "af", row_num)
            an = _check_byte01(an_f, "an", row_num)

            duration = int(float(row["duration_ms"]))
            if duration < 0 or duration > 255:
                raise PhonemeCsvError(f"row {row_num}: duration_ms={duration} is out of uint8_t range")

            flags = parse_flags(row["flags"], row_num)

            pr = PhonemeRow(
                symbol=symbol, label=row["label"].strip(),
                F=F, B=B, fric_F=fric_F, fric_B=fric_B, nasal_F=nasal_F, nasal_B=nasal_B,
                av=av_f, af=af_f, an=an_f, duration_ms=duration, flags=flags,
            )
            pr.pclass = classify_row(pr)
            # Store quantized bytes (not the raw floats) for emission -- av/af/an
            # above were kept as floats only for classify_row()'s > 0.0 tests.
            pr._av_byte, pr._af_byte, pr._an_byte = av, af, an  # type: ignore[attr-defined]
            rows.append(pr)

    if not rows:
        raise PhonemeCsvError(f"{path}: no phoneme rows found")
    return rows


def render_phonemes_header(rows: List[PhonemeRow], csv_path: str) -> str:
    lines = [
        f"// GENERATED by tools/speechgen.py gen from {os.path.basename(csv_path)} -- do not hand-edit.",
        "#pragma once",
        "",
        '#include "phoneme_def.h"',
        "",
        "enum Phoneme : uint8_t {",
    ]
    for r in rows:
        lines.append(f"    PH_{r.symbol},")
    lines.append("    PHONEME_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr PhonemeDef PHONEME_TARGETS[PHONEME_COUNT] = {")
    for r in rows:
        f_list = ", ".join(str(v) for v in r.F)
        b_list = ", ".join(str(v) for v in r.B)
        lines.append(
            f"    {{ {{ {f_list} }}, {r.fric_F}, {r.nasal_F}, "
            f"{{ {b_list} }}, {r.fric_B}, {r.nasal_B}, "
            f"{r._av_byte}, {r._af_byte}, {r._an_byte}, {r.duration_ms}, {r.flags} }},  "
            f"// {r.symbol} {r.label}"
        )
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr const char *PHONEME_LABELS[PHONEME_COUNT] = {")
    for r in rows:
        lines.append(f'    "{r.label}",')
    lines.append("};")
    lines.append("")
    # Bare CSV symbol ("I", "SH", "P_CL", ...), distinct from PHONEME_LABELS'
    # display string ("/i/") -- this is what the host harness matches
    # against vowel_reference.h's independent reference table by (#32).
    lines.append("inline constexpr const char *PHONEME_SYMBOLS[PHONEME_COUNT] = {")
    for r in rows:
        lines.append(f'    "{r.symbol}",')
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def render_meta_header(rows: List[PhonemeRow]) -> str:
    lines = [
        "// GENERATED by tools/speechgen.py gen -- do not hand-edit.",
        "// Host-only phoneme classification for tools/host_render/render_speech.cpp's",
        "// per-class acoustic checks. Parallel to phonemes.h's PHONEME_TARGETS[] (same",
        "// order, same PHONEME_COUNT) but never linked into the device build -- the",
        "// class a phoneme belongs to isn't runtime data, it's a fact about the table",
        "// this generator already knows and shouldn't make the harness re-derive.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "enum PhonemeClass : uint8_t {",
    ]
    for c in CLASSES:
        lines.append(f"    PCLASS_{c},")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr uint8_t PHONEME_CLASS[] = {")
    for r in rows:
        lines.append(f"    PCLASS_{r.pclass},  // {r.symbol}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def cmd_gen(args: argparse.Namespace) -> None:
    try:
        rows = load_csv(args.csv)
    except PhonemeCsvError as exc:
        print(f"speechgen: {args.csv}: {exc}", file=sys.stderr)
        sys.exit(1)

    with open(args.header, "w") as f:
        f.write(render_phonemes_header(rows, args.csv))
    table_bytes = len(rows) * 26  # sizeof(PhonemeDef), see phoneme_def.h's comment
    print(f"Wrote {args.header} ({len(rows)} phonemes, PHONEME_TARGETS = {table_bytes} bytes in flash)")

    meta_path = args.meta_out
    if meta_path is None:
        meta_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "host_render", "phoneme_meta.h")
    with open(meta_path, "w") as f:
        f.write(render_meta_header(rows))
    print(f"Wrote {meta_path} (host-only, not linked into the device build)")

    by_class: Dict[str, int] = {}
    for r in rows:
        by_class[r.pclass] = by_class.get(r.pclass, 0) + 1
    print("Class breakdown: " + ", ".join(f"{k}={v}" for k, v in by_class.items()))


def load_vowel_ref_csv(path: str) -> List[dict]:
    out = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row_num, row in enumerate(reader, start=2):
            symbol = row["symbol"].strip()
            if not symbol:
                continue
            out.append({
                "symbol": symbol,
                "F1": _check_u16(float(row["F1"]), "F1", row_num),
                "F2": _check_u16(float(row["F2"]), "F2", row_num),
                "F3": _check_u16(float(row["F3"]), "F3", row_num),
            })
    if not out:
        raise PhonemeCsvError(f"{path}: no reference rows found")
    return out


def render_vowel_ref_header(entries: List[dict]) -> str:
    lines = [
        "// GENERATED by tools/speechgen.py gen-vowel-ref from speech_vowel_reference.csv",
        "// -- do not hand-edit.",
        "//",
        "// Independent published vowel-space reference (#32's real acceptance target:",
        "// catch coefficient/data-entry errors the ear forgives). Deliberately a",
        "// separate file from speech_phonemes.csv's own F/B targets -- comparing a",
        "// measured formant against the same number that was typed in to author it",
        "// proves nothing. This exists to survive *future* edits to",
        "// speech_phonemes.csv: if a vowel row drifts from the published literature",
        "// without this file being updated too, the harness's comparison against it",
        "// (not against speech_phonemes.csv) is what catches that.",
        "#pragma once",
        "",
        "struct VowelRef { const char *symbol; float F1, F2, F3; };",
        "",
        "inline constexpr VowelRef VOWEL_REFERENCE[] = {",
    ]
    for e in entries:
        lines.append(f'    {{ "{e["symbol"]}", {e["F1"]}.0f, {e["F2"]}.0f, {e["F3"]}.0f }},')
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def cmd_gen_vowel_ref(args: argparse.Namespace) -> None:
    try:
        entries = load_vowel_ref_csv(args.csv)
    except PhonemeCsvError as exc:
        print(f"speechgen: {args.csv}: {exc}", file=sys.stderr)
        sys.exit(1)
    with open(args.header, "w") as f:
        f.write(render_vowel_ref_header(entries))
    print(f"Wrote {args.header} ({len(entries)} reference vowels)")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="speechgen")
    sub = parser.add_subparsers(dest="command", required=True)

    p_gen = sub.add_parser("gen", help="parse a phoneme CSV and emit phonemes.h + phoneme_meta.h")
    p_gen.add_argument("csv")
    p_gen.add_argument("header", help="output path for phonemes.h")
    p_gen.add_argument("--meta-out", default=None,
                        help="output path for the host-only classification header "
                             "(default: tools/host_render/phoneme_meta.h)")
    p_gen.set_defaults(func=cmd_gen)

    p_ref = sub.add_parser("gen-vowel-ref", help="parse the vowel reference CSV and emit vowel_reference.h")
    p_ref.add_argument("csv")
    p_ref.add_argument("header")
    p_ref.set_defaults(func=cmd_gen_vowel_ref)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
