#!/usr/bin/env python3
"""samgen.py -- SAM tract phrase-bank generator (#72), the same role
tools/speechgen.py's `gen-phrases` plays for the formant tract: reads a
plain-text phrase list, runs each word through sam_reciter.py (this
module's own from-scratch reciter, not nrl_rules.py), and writes a compiled
header.

    samgen.py gen-phrases <phrases.txt> <sam_phrases.h>

Phrase list format: one phrase per line, `NAME: word word ...`, same
`word{SYM SYM ...}` override escape hatch tools/speechgen.py's own phrase
list uses -- a bare override symbol carries no stress; prefix it with `*`
(`*IY`) to mark it stressed. See tools/sam_phrases.txt.

sam_phrases.h emits SamUtterance rows as plain integer allophone indices
(sam2allophones.py's SAM_ALLOPHONE_NAMES ordering), not symbolic
`SAM_ID_*` names -- unlike phrases.h's `PH_*` references (phonemes.h is
always present), sam_allophones.h is optional and gitignored, so
sam_phrases.h must not have a hard compile-time dependency on it existing.
Pitch-contour targets (semitones, `sam.h`'s per-segment ramp) come from
sam_reciter.py's stress assignment, plus one phrase-level rule this module
owns: the last non-silent segment before a phrase's trailing pause gets a
small negative offset (terminal declination) unless it's already stressed,
approximating S.A.M.'s falling end-of-utterance intonation.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from typing import Dict, List, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sam2allophones as sa
from sam_reciter import ReciterError, assign_stress, word_to_allophones

# Whole semitones only -- sam_phrases.h's pitch array is int8_t, plain
# semitones, no fractional encoding. 3/-2 (this constant's original value)
# read as noticeably melodic/"singing" on real hardware, more than natural
# speech stress; halved to a subtler overshoot (#73 hardware feedback).
STRESSED_SEMITONES = 2      # pitch overshoot on a stressed allophone
DECLINATION_SEMITONES = -1  # phrase-final falling intonation


class SamGenError(Exception):
    pass


_TOKEN_RE = re.compile(r"\S*\{[^}]*\}|\S+")


def _validate_ident(name: str, kind: str, line_num: int) -> None:
    if not name or not (name[0].isalpha() or name[0] == "_"):
        raise SamGenError(f"line {line_num}: {kind} '{name}' must start with a letter or underscore")
    if any(not (c.isalnum() or c == "_") for c in name):
        raise SamGenError(f"line {line_num}: {kind} '{name}' must be letters/digits/underscore only")


@dataclass
class PhraseEntry:
    name: str
    text: str
    allophones: List[str]
    pitch: List[int]
    release_index: int


_SIL = "SIL"


def _parse_override(token: str) -> Tuple[List[str], List[bool]]:
    """`{IY *EH S}` -> (["IY", "EH", "S"], [False, True, False])."""
    syms, stressed = [], []
    for raw in token[1:-1].split():
        s = raw.startswith("*")
        syms.append(raw[1:] if s else raw)
        stressed.append(s)
    return syms, stressed


def load_phrase_list(path: str) -> List[PhraseEntry]:
    valid = set(sa.SAM_ALLOPHONE_NAMES)

    def check_symbols(symbols: List[str], context: str, line_num: int) -> None:
        for sym in symbols:
            if sym not in valid:
                raise SamGenError(
                    f"line {line_num}: {context}: unknown SAM allophone '{sym}' -- "
                    f"not one of the {len(valid)} names in sam2allophones.py's "
                    f"SAM_ALLOPHONE_NAMES (typo, or they've drifted apart)"
                )

    entries: List[PhraseEntry] = []
    seen_names: Dict[str, int] = {}

    with open(path) as f:
        for line_num, raw_line in enumerate(f, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                raise SamGenError(f"line {line_num}: missing ':' -- expected 'NAME: word word ...'")
            name, raw_text = line.split(":", 1)
            name, raw_text = name.strip(), raw_text.strip()
            _validate_ident(name, "phrase name", line_num)
            if name in seen_names:
                raise SamGenError(f"line {line_num}: duplicate phrase name '{name}' (first seen at line {seen_names[name]})")
            seen_names[name] = line_num

            tokens = _TOKEN_RE.findall(raw_text)
            if not tokens:
                raise SamGenError(f"line {line_num}: phrase '{name}' has no words")

            allophones: List[str] = []
            pitch: List[int] = []
            display_words: List[str] = []
            for word_idx, token in enumerate(tokens):
                if word_idx > 0:
                    allophones.append(_SIL)
                    pitch.append(0)
                brace = token.find("{")
                if brace >= 0:
                    word_part, override_part = token[:brace], token[brace:]
                    if not override_part.endswith("}"):
                        raise SamGenError(f"line {line_num}: phrase '{name}': unterminated {{}} override in '{token}'")
                    syms, stressed = _parse_override(override_part)
                    if not syms:
                        raise SamGenError(f"line {line_num}: phrase '{name}': empty {{}} override in '{token}'")
                    check_symbols(syms, f"phrase '{name}' override '{token}'", line_num)
                    allophones.extend(syms)
                    pitch.extend(STRESSED_SEMITONES if s else 0 for s in stressed)
                    display_words.append(word_part if word_part else "/" + " ".join(syms) + "/")
                else:
                    try:
                        word_al = word_to_allophones(token)
                        word_stress = assign_stress(token, word_al)
                    except ReciterError as exc:
                        raise SamGenError(
                            f"line {line_num}: phrase '{name}', word '{token}': {exc} -- "
                            f"use a word{{SYM SYM ...}} override instead"
                        )
                    check_symbols(word_al, f"phrase '{name}' word '{token}'", line_num)
                    allophones.extend(word_al)
                    pitch.extend(STRESSED_SEMITONES if s else 0 for s in word_stress)
                    display_words.append(token)

            # Terminal declination: the last non-silent, non-stressed segment
            # falls slightly ahead of the trailing pause -- see module docstring.
            for i in range(len(allophones) - 1, -1, -1):
                if allophones[i] != _SIL:
                    if pitch[i] == 0:
                        pitch[i] = DECLINATION_SEMITONES
                    break

            text = " ".join(display_words)
            allophones.append(_SIL)
            pitch.append(0)

            entries.append(PhraseEntry(name=name, text=text, allophones=allophones, pitch=pitch,
                                        release_index=len(allophones) - 1))

    if not entries:
        raise SamGenError(f"{path}: no phrases found")
    return entries


def render_header(entries: List[PhraseEntry], txt_path: str) -> str:
    name_to_idx = {n: i for i, n in enumerate(sa.SAM_ALLOPHONE_NAMES)}
    lines = [
        f"// GENERATED by tools/samgen.py gen-phrases from {os.path.basename(txt_path)} -- do not hand-edit.",
        "// Allophone indices are plain integers (sam2allophones.py's SAM_ALLOPHONE_NAMES",
        "// ordering), not SAM_ID_* names -- sam_allophones.h is optional and gitignored,",
        "// so this header must not depend on it existing. Meaningful once a generated",
        "// allophone table is present locally; wraps into sam.h's small fixture otherwise",
        "// (safe, not intelligible -- see module_speech.md's SAM Tract section).",
        "#pragma once",
        "",
        '#include "sam.h"',
        "",
    ]
    for e in entries:
        idx_list = ", ".join(f"{name_to_idx[a]}" for a in e.allophones)
        lines.append(f"static constexpr uint8_t SAM_PHRASE_{e.name}_ALLOPHONES[] = {{ {idx_list} }};  // \"{e.text}\"")
        pitch_list = ", ".join(str(p) for p in e.pitch)
        lines.append(f"static constexpr int8_t SAM_PHRASE_{e.name}_PITCH[] = {{ {pitch_list} }};")
    lines.append("")
    lines.append("enum SamPhraseId : uint8_t {")
    for e in entries:
        lines.append(f"    SAM_PHRASE_{e.name},")
    lines.append("    SAM_PHRASE_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr SamUtterance SAM_PHRASES[SAM_PHRASE_COUNT] = {")
    for e in entries:
        lines.append(
            f"    {{ SAM_PHRASE_{e.name}_ALLOPHONES, SAM_PHRASE_{e.name}_PITCH, "
            f"{len(e.allophones)}, {e.release_index} }},  // \"{e.text}\""
        )
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr const char *SAM_PHRASE_TEXT[SAM_PHRASE_COUNT] = {")
    for e in entries:
        lines.append(f'    "{e.text}",')
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def cmd_gen_phrases(args: argparse.Namespace) -> None:
    try:
        entries = load_phrase_list(args.phrases)
    except SamGenError as exc:
        print(f"samgen: {exc}", file=sys.stderr)
        sys.exit(1)

    with open(args.header, "w") as f:
        f.write(render_header(entries, args.phrases))

    total = sum(len(e.allophones) for e in entries)
    print(f"Wrote {args.header} ({len(entries)} phrases, {total} allophones total)")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="samgen")
    sub = parser.add_subparsers(dest="command", required=True)

    p_gen = sub.add_parser("gen-phrases", help="parse a SAM phrase list and emit sam_phrases.h")
    p_gen.add_argument("phrases", help="input phrase list (see tools/sam_phrases.txt)")
    p_gen.add_argument("header", help="output path for sam_phrases.h")
    p_gen.set_defaults(func=cmd_gen_phrases)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
