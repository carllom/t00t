#!/usr/bin/env python3
"""sam2allophones.py -- S.A.M. reference tables -> sam_allophones.h
(module_speech.md "SAM Tract").

    sam2allophones.py convert <in.h> [in2.h ...] <out.h>
        Parse one or more locally-supplied S.A.M. reference header(s) for
        four specific C arrays -- `sampledConsonantFlags`, `amplitudeRescale`,
        `ampl1data`, `ampl2data`, `ampl3data` -- searching across every given
        file for each by name (so it doesn't matter which file a given array
        lives in), and emit #71's SamAllophoneTarget table (sam.h) for all
        SAM_ALLOPHONE_COUNT allophones: <out.h>.

    sam2allophones.py dump <in.h> [in2.h ...]
        Parse and print a per-allophone summary (name, amp weights, af,
        source) without writing a header.

Source format: locally-supplied copies of the reference C headers that ship
with the commonly-circulated S.A.M. reimplementations (e.g.
https://github.com/s-macke/SAM, https://github.com/vidarh/SAM --
`RenderTabs.h`/`SamTabs.h`), each `unsigned char <name>[] = { ... };`.
Neither carries a license, so -- following the DX7 `.syx`/Talkie precedent
(tools/syx2patch.py, tools/talkie2lattice.py) -- only this converter is
committed; the reference headers are supplied locally and gitignored
(`../sam/`).

What's sourced from the reference data, and what isn't: `sampledConsonantFlags`
(which allophones are noise-driven, e.g. /s/, /sh/, /th/ -- a structural
classification, not audio data) and the three per-formant amplitude tables
(`ampl1data`/`ampl2data`/`ampl3data`, rescaled through the reference's own
`amplitudeRescale` curve) are genuine S.A.M.-specific numeric data, read
here. Formant *frequency* targets are not: S.A.M.'s own `freq1data`/
`freq2data`/`freq3data` are phase-increment values for a three-oscillator
wavetable technique (`sam.h`'s resonant-filter model doesn't use one), so
translating them into Hz would mean reverse-engineering a mismatched
representation with no way to verify the result. `FORMANT_REFERENCE` below
instead extends the same Peterson & Barney (1952) published acoustic data
`tools/speech_phonemes.csv` already uses for the formant tract, to the
allophones sam.h's SAM_TEST_ALLOPHONES fixture doesn't cover, plus this
project's own consonant/burst targets where a S.A.M. allophone corresponds
directly to one already in speech_phonemes.csv.

Allophone list: the 81-entry name/index scheme (`SAM_ALLOPHONE_NAMES` below)
is S.A.M.'s well-documented, widely-published allophone table (its own user
manual describes this list); the entries marked `**` in every published copy
of the underlying disassembly are unnamed contextual variants with no
distinct linguistic identity of their own -- mapped here to their nearest
named family (see `FORMANT_REFERENCE`'s comments) rather than fabricated.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Dict, List, Tuple

SAM_ALLOPHONE_COUNT = 80

# S.A.M.'s own allophone index/name table. Punctuation-pause slots (1-4) get
# identifier-safe names; every other name matches the symbol S.A.M.'s
# published documentation and every circulated disassembly comment use.
# Every circulated reference implementation's own per-allophone data arrays
# (sampledConsonantFlags, ampl1-3data, phonemeLengthTable) are 80 entries
# long, not 81 -- some published documentation lists a UL/UM/UN trio of
# syllabic consonants ending at index 80, but no reference data array this
# tool has found actually carries that 81st slot, so UN (folded into UM,
# both syllabic nasals) is dropped here to match the verified data shape
# rather than a transcribed comment.
SAM_ALLOPHONE_NAMES: List[str] = [
    "SIL", "PAUSE_PERIOD", "PAUSE_QUESTION", "PAUSE_COMMA", "PAUSE_DASH",
    "IY", "IH", "EH", "AE", "AA", "AH", "AO", "UH", "AX", "IX", "ER", "UX",
    "OH", "RX", "LX", "WX", "YX", "WH", "R", "L", "W", "Y", "M", "N", "NX",
    "DX", "Q", "S", "SH", "F", "TH", "HX", "XX", "Z", "ZH", "V", "DH", "CH",
    "VAR_43", "J", "VAR_45", "VAR_46", "VAR_47",
    "EY", "AY", "OY", "AW", "OW", "UW",
    "B", "VAR_55", "VAR_56", "D", "VAR_58", "VAR_59", "G", "VAR_61",
    "VAR_62", "GX", "VAR_64", "VAR_65", "P", "VAR_67", "VAR_68", "T",
    "VAR_70", "VAR_71", "K", "VAR_73", "VAR_74", "KX", "VAR_76", "VAR_77",
    "UL", "UM",
]
assert len(SAM_ALLOPHONE_NAMES) == SAM_ALLOPHONE_COUNT

# (F1, F2, F3, fric_F, fric_B) per allophone name, Hz -- see module docstring
# for sourcing. fric_F/fric_B only matter for allophones the reference data's
# own sampledConsonantFlags flags as noise-driven (af > 0); every other
# allophone's fric_F/fric_B is the same neutral default speech_phonemes.csv's
# own SIL/vowel rows already use, since it's never audible there.
_NEUTRAL_FRIC = (4000, 300)
_BURST_LABIAL = (800, 400)     # P/B family -- speech_phonemes.csv P_BR/B_BR
_BURST_ALVEOLAR = (5000, 600)  # T/D/DX family -- speech_phonemes.csv T_BR/D_BR
_BURST_VELAR = (2000, 500)     # K/G/GX/KX family -- speech_phonemes.csv K_BR/G_BR
_BURST_PALATAL = (2700, 400)   # CH/J/SH/ZH family -- speech_phonemes.csv CH_BR/SH

FORMANT_REFERENCE: Dict[str, Tuple[int, int, int, int, int]] = {
    "SIL": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "PAUSE_PERIOD": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "PAUSE_QUESTION": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "PAUSE_COMMA": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "PAUSE_DASH": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "IY": (270, 2290, 3010) + _NEUTRAL_FRIC,   # speech_phonemes.csv I
    "IH": (390, 1990, 2550) + _NEUTRAL_FRIC,
    "EH": (530, 1840, 2480) + _NEUTRAL_FRIC,   # speech_phonemes.csv E
    "AE": (660, 1720, 2410) + _NEUTRAL_FRIC,
    "AA": (730, 1090, 2440) + _NEUTRAL_FRIC,   # speech_phonemes.csv A
    "AH": (640, 1190, 2390) + _NEUTRAL_FRIC,
    "AO": (570, 840, 2410) + _NEUTRAL_FRIC,    # speech_phonemes.csv O
    "UH": (440, 1020, 2240) + _NEUTRAL_FRIC,
    "AX": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "IX": (390, 1990, 2550) + _NEUTRAL_FRIC,   # reduced /i/, ~ IH
    "ER": (490, 1350, 1690) + _NEUTRAL_FRIC,
    "UX": (300, 870, 2240) + _NEUTRAL_FRIC,    # ~ speech_phonemes.csv U
    "OH": (570, 840, 2410) + _NEUTRAL_FRIC,    # ~ AO
    "RX": (310, 1060, 1380) + _NEUTRAL_FRIC,   # ~ R
    "LX": (360, 1300, 2700) + _NEUTRAL_FRIC,   # ~ L
    "WX": (290, 610, 2150) + _NEUTRAL_FRIC,    # ~ W
    "YX": (260, 2070, 3020) + _NEUTRAL_FRIC,   # ~ Y
    "WH": (290, 610, 2150) + _NEUTRAL_FRIC,    # voiceless W, ~ W
    "R": (310, 1060, 1380) + _NEUTRAL_FRIC,
    "L": (360, 1300, 2700) + _NEUTRAL_FRIC,
    "W": (290, 610, 2150) + _NEUTRAL_FRIC,
    "Y": (260, 2070, 3020) + _NEUTRAL_FRIC,
    "M": (300, 1000, 2200) + _NEUTRAL_FRIC,
    "N": (300, 1600, 2600) + _NEUTRAL_FRIC,
    "NX": (300, 2200, 2800) + _NEUTRAL_FRIC,   # speech_phonemes.csv NG
    "DX": (500, 1700, 2500) + _BURST_ALVEOLAR,  # flap D
    "Q": (500, 1500, 2500) + _NEUTRAL_FRIC,     # glottal stop, ~ SIL
    "S": (500, 1500, 2500, 6000, 500),
    "SH": (500, 1500, 2500) + _BURST_PALATAL,
    "F": (500, 1500, 2500, 7500, 600),
    "TH": (500, 1500, 2500, 8000, 600),
    "HX": (500, 1500, 2500, 3000, 800),         # /H, aspiration, speech_phonemes.csv HH
    "XX": (500, 1500, 2500) + _NEUTRAL_FRIC,    # /X, unclear flap/liaison marker
    "Z": (500, 1500, 2500, 6000, 500),
    "ZH": (500, 1500, 2500) + _BURST_PALATAL,
    "V": (500, 1500, 2500, 7500, 600),
    "DH": (500, 1500, 2500, 8000, 600),
    "CH": (500, 1700, 2500) + _BURST_PALATAL,
    "VAR_43": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "J": (500, 1700, 2500) + _BURST_PALATAL,
    "VAR_45": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "VAR_46": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "VAR_47": (500, 1500, 2500) + _NEUTRAL_FRIC,
    "EY": (400, 2100, 2700) + _NEUTRAL_FRIC,
    "AY": (550, 1700, 2600) + _NEUTRAL_FRIC,
    "OY": (450, 1400, 2600) + _NEUTRAL_FRIC,
    "AW": (680, 1100, 2300) + _NEUTRAL_FRIC,
    "OW": (570, 840, 2410) + _NEUTRAL_FRIC,     # no direct speech_phonemes.csv entry, ~ AO
    "UW": (300, 870, 2240) + _NEUTRAL_FRIC,     # speech_phonemes.csv U
    "B": (500, 1000, 2500) + _BURST_LABIAL,
    "VAR_55": (500, 1000, 2500) + _BURST_LABIAL,
    "VAR_56": (500, 1000, 2500) + _BURST_LABIAL,
    "D": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "VAR_58": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "VAR_59": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "G": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_61": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_62": (500, 2200, 2500) + _BURST_VELAR,
    "GX": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_64": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_65": (500, 2200, 2500) + _BURST_VELAR,
    "P": (500, 1000, 2500) + _BURST_LABIAL,
    "VAR_67": (500, 1000, 2500) + _BURST_LABIAL,
    "VAR_68": (500, 1000, 2500) + _BURST_LABIAL,
    "T": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "VAR_70": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "VAR_71": (500, 1700, 2500) + _BURST_ALVEOLAR,
    "K": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_73": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_74": (500, 2200, 2500) + _BURST_VELAR,
    "KX": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_76": (500, 2200, 2500) + _BURST_VELAR,
    "VAR_77": (500, 2200, 2500) + _BURST_VELAR,
    "UL": (360, 1300, 2700) + _NEUTRAL_FRIC,    # syllabic L, ~ L
    "UM": (300, 1000, 2200) + _NEUTRAL_FRIC,    # syllabic M, ~ M
}
assert set(FORMANT_REFERENCE.keys()) == set(SAM_ALLOPHONE_NAMES)

REQUIRED_ARRAYS = ("sampledConsonantFlags", "amplitudeRescale", "ampl1data", "ampl2data", "ampl3data",
                    "phonemeLengthTable")

# phonemeLengthTable's raw units aren't documented as a fixed ms value in
# any reference source this tool has checked; #72 needs *a* duration to
# drive the SAM sequencer, so this is a simple linear scale landing typical
# entries in the same 20-200ms range this project's own phoneme table
# (speech_phonemes.csv) already uses, clamped so the two syllabic-consonant
# outliers (UL/UM, meant to be held rather than literally ~1200ms/2500ms)
# don't produce an implausibly long segment. DURATION_MIN_MS in particular
# is raised well above sam.h's own SAM_RAMP_COEFF settle time (~12-15ms) --
# a segment shorter than that never actually reaches its target before the
# next one starts pulling it elsewhere, heard on real hardware as mud
# rather than distinct sounds (#73 hardware feedback).
DURATION_SCALE_MS_PER_TICK = 7
DURATION_MIN_MS = 40
DURATION_MAX_MS = 200


class SamConverterError(Exception):
    pass


def _strip_line_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def parse_c_array(text: str, name: str) -> List[int]:
    """Finds `[const] [unsigned|signed] char <name>[...] = { ... };` (in that
    order, whitespace-insensitive, across line breaks) and returns its
    integer literals (hex `0x..` or decimal) in source order."""
    pattern = (
        r"(?:const\s+)?(?:unsigned\s+|signed\s+)?char\s+" + re.escape(name)
        + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};"
    )
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        return []
    body = m.group(1)
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|-?\d+", body)]


def _find_array(sources: Dict[str, str], name: str) -> Tuple[List[int], str]:
    for path, text in sources.items():
        values = parse_c_array(text, name)
        if values:
            return values, path
    raise SamConverterError(f"array `{name}` not found in any given file")


class AllophoneOut:
    def __init__(self, name: str, F: Tuple[int, int, int], amp: Tuple[float, float, float],
                 fric_F: int, fric_B: int, af: float, sampled: bool, duration_ms: int):
        self.name = name
        self.F = F
        self.amp = amp
        self.fric_F = fric_F
        self.fric_B = fric_B
        self.af = af
        self.sampled = sampled
        self.duration_ms = duration_ms


def convert_all(paths: List[str]) -> List[AllophoneOut]:
    sources: Dict[str, str] = {}
    for path in paths:
        with open(path, "r") as f:
            sources[path] = _strip_line_comments(f.read())

    arrays: Dict[str, List[int]] = {}
    for name in REQUIRED_ARRAYS:
        values, _ = _find_array(sources, name)
        arrays[name] = values

    rescale = arrays["amplitudeRescale"]
    if len(rescale) < 16:
        raise SamConverterError(f"amplitudeRescale has {len(rescale)} entries, need at least 16")

    for name in ("sampledConsonantFlags", "ampl1data", "ampl2data", "ampl3data", "phonemeLengthTable"):
        if len(arrays[name]) < SAM_ALLOPHONE_COUNT:
            raise SamConverterError(
                f"{name} has {len(arrays[name])} entries, need at least {SAM_ALLOPHONE_COUNT}"
            )

    rows: List[AllophoneOut] = []
    for idx, name in enumerate(SAM_ALLOPHONE_NAMES):
        F1, F2, F3, fric_F, fric_B = FORMANT_REFERENCE[name]
        sampled = arrays["sampledConsonantFlags"][idx] != 0
        if sampled:
            amp = (0.0, 0.0, 0.0)
            af = 1.0
        else:
            # Raw 4-bit amplitude (0-15) through the reference's own
            # rescale curve, normalized to this tract's 0..1 amp weight --
            # the same curve S.A.M.'s own render pass applies before using
            # these values, read here rather than re-derived.
            raw = (arrays["ampl1data"][idx], arrays["ampl2data"][idx], arrays["ampl3data"][idx])
            amp = tuple(rescale[v & 0x0F] / 15.0 for v in raw)
            af = 0.0
        raw_duration = arrays["phonemeLengthTable"][idx]
        duration_ms = max(DURATION_MIN_MS, min(DURATION_MAX_MS, raw_duration * DURATION_SCALE_MS_PER_TICK))
        rows.append(AllophoneOut(name, (F1, F2, F3), amp, fric_F, fric_B, af, sampled, duration_ms))
    return rows


def _bandwidth_for_f1(f1: int) -> Tuple[int, int, int]:
    """Formant bandwidth widens with F1 in real vocal-tract acoustics (an
    open vowel resonates less sharply than a close one) -- the same pattern
    sam.h's own SAM_TEST_ALLOPHONES fixture already uses per vowel (60-90-150
    for close /iy//uw/, 90-110-170 for open /aa/). Consonants land in the
    open-vowel range here too, matching speech_phonemes.csv's own uniform
    consonant bandwidths."""
    if f1 < 320:
        return (60, 85, 150)
    if f1 < 500:
        return (75, 95, 160)
    return (90, 110, 170)


def _format_row(r: AllophoneOut) -> str:
    Fs = ", ".join(f"{v}.0f" for v in r.F)
    Bs = ", ".join(f"{v}.0f" for v in _bandwidth_for_f1(r.F[0]))
    amps = ", ".join(f"{v:.4f}f" for v in r.amp)
    return (f"{{ {{ {Fs} }}, {{ {Bs} }}, {{ {amps} }}, {r.fric_F}.0f, {r.fric_B}.0f, "
            f"{r.af:.1f}f, {r.duration_ms} }}")


def render_header(rows: List[AllophoneOut], source_paths: List[str]) -> str:
    sources = ", ".join(os.path.basename(p) for p in source_paths)
    lines = [
        f"// GENERATED by tools/sam2allophones.py convert from {sources} -- do not hand-edit.",
        "// Every entry is sam.h's SamAllophoneTarget: F1-F3 from FORMANT_REFERENCE",
        "// (Peterson & Barney published data, not S.A.M.'s own oscillator encoding),",
        "// B unchanged from sam.h's own fixture bandwidths, amp/af from the reference",
        "// data's own per-allophone amplitude tables -- see sam2allophones.py's module",
        "// docstring for the full sourcing breakdown.",
        "#pragma once",
        "",
        "enum SamAllophoneId : uint8_t {",
    ]
    for r in rows:
        lines.append(f"    SAM_ID_{r.name},")
    lines.append("    SAM_ALLOPHONE_DATA_COUNT")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr SamAllophoneTarget SAM_ALLOPHONES[SAM_ALLOPHONE_DATA_COUNT] = {{")
    for r in rows:
        lines.append(f"    {_format_row(r)},  // {r.name}")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr const char *SAM_ALLOPHONE_DATA_NAMES[SAM_ALLOPHONE_DATA_COUNT] = {{")
    for r in rows:
        lines.append(f'    "{r.name}",')
    lines.append("};")
    lines.append("")
    sampled_count = sum(1 for r in rows if r.sampled)
    lines.append(f"// {len(rows)} allophones ({sampled_count} noise-driven, {len(rows) - sampled_count} formant-driven).")
    lines.append("")
    return "\n".join(lines)


def cmd_convert(args: argparse.Namespace) -> None:
    try:
        rows = convert_all(args.reference)
    except SamConverterError as exc:
        print(f"sam2allophones: {exc}", file=sys.stderr)
        sys.exit(1)

    with open(args.header, "w") as f:
        f.write(render_header(rows, args.reference))

    sampled_count = sum(1 for r in rows if r.sampled)
    print(f"Wrote {args.header} ({len(rows)} allophones, {sampled_count} noise-driven, "
          f"{len(rows) - sampled_count} formant-driven)")


def cmd_dump(args: argparse.Namespace) -> None:
    try:
        rows = convert_all(args.reference)
    except SamConverterError as exc:
        print(f"sam2allophones: {exc}", file=sys.stderr)
        sys.exit(1)

    for r in rows:
        kind = "noise " if r.sampled else "formant"
        amps = ", ".join(f"{v:.2f}" for v in r.amp)
        print(f"  {r.name:14s} {kind}  F={r.F}  amp=({amps})  af={r.af:.1f}  dur={r.duration_ms}ms")
    sampled_count = sum(1 for r in rows if r.sampled)
    print(f"{len(rows)} allophones ({sampled_count} noise-driven, {len(rows) - sampled_count} formant-driven)")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="sam2allophones")
    sub = parser.add_subparsers(dest="command", required=True)

    p_convert = sub.add_parser("convert", help="parse S.A.M. reference header(s) and emit sam_allophones.h")
    p_convert.add_argument("reference", nargs="+", help="input S.A.M. reference .h file(s)")
    p_convert.add_argument("header", help="output path for sam_allophones.h")
    p_convert.set_defaults(func=cmd_convert)

    p_dump = sub.add_parser("dump", help="parse and print a per-allophone summary, write nothing")
    p_dump.add_argument("reference", nargs="+", help="input S.A.M. reference .h file(s)")
    p_dump.set_defaults(func=cmd_dump)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
