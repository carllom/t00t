#!/usr/bin/env python3
"""Test suite for sam_reciter.py (#72). Run: python3 tools/test_sam_reciter.py

A curated set of known words and their expected allophone sequences, the
same testing posture tools/nrl_rules.py's own header comment describes for
the formant tract's reciter -- not exhaustive dictionary coverage, an
inline WORD_EXCEPTIONS/STRESS_OVERRIDES entry for anything this gets wrong.
"""

from __future__ import annotations

import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sam2allophones as sa
from sam_reciter import ReciterError, assign_stress, word_to_allophones

_VALID = set(sa.SAM_ALLOPHONE_NAMES)


def _check(word: str, expected: tuple, expected_stress_idx=None) -> None:
    al = word_to_allophones(word)
    assert tuple(al) == expected, f"'{word}': got {al}, expected {list(expected)}"
    for a in al:
        assert a in _VALID, f"'{word}': emitted '{a}', not a SAM_ALLOPHONE_NAMES entry"
    stress = assign_stress(word, al)
    assert len(stress) == len(al)
    if expected_stress_idx is not None:
        assert stress[expected_stress_idx] is True, f"'{word}': expected stress at index {expected_stress_idx}, got {stress}"
        assert sum(stress) == 1, f"'{word}': expected exactly one stressed allophone, got {stress}"


def test_simple_consonant_vowel_words() -> None:
    _check("cat", ("K", "AE", "T"), 1)
    _check("dog", ("D", "AA", "G"), 1)
    _check("bed", ("B", "EH", "D"), 1)


def test_silent_e_lengthens_vowel() -> None:
    _check("make", ("M", "EY", "K"), 1)
    _check("time", ("T", "AY", "M"), 1)
    _check("home", ("HX", "OW", "M"), 1)
    _check("cute", ("K", "UW", "T"), 1)


def test_digraphs() -> None:
    _check("see", ("S", "IY"), 1)
    _check("rain", ("R", "EY", "N"), 1)
    _check("boat", ("B", "OW", "T"), 1)
    _check("moon", ("M", "UW", "N"), 1)
    _check("boy", ("B", "OY"), 1)


def test_r_controlled_vowels() -> None:
    _check("car", ("K", "AA", "R"), 1)
    _check("her", ("HX", "ER"), 1)
    _check("bird", ("B", "ER", "D"), 1)
    _check("for", ("F", "AO", "R"), 1)


def test_ing_suffix() -> None:
    al = word_to_allophones("sing")
    assert tuple(al) == ("S", "IH", "NX")
    al2 = word_to_allophones("playing")
    assert tuple(al2[-2:]) == ("IH", "NX")


def test_doubled_consonant_collapses() -> None:
    _check("off", ("AA", "F"), 0)
    al = word_to_allophones("hello")
    assert al.count("L") == 1  # LL collapses to one L


def test_word_exceptions() -> None:
    _check("the", ("DH", "AX"))
    _check("of", ("AH", "V"))
    _check("you", ("Y", "UW"), 1)
    _check("hello", ("HX", "EH", "L", "OW"), 3)
    _check("machine", ("M", "AX", "SH", "IY", "N"), 3)


def test_function_words_unstressed() -> None:
    al = word_to_allophones("the")
    stress = assign_stress("the", al)
    assert not any(stress), f"'the': expected no stressed allophone, got {stress}"


def test_non_alpha_raises() -> None:
    try:
        word_to_allophones("hello!")
        assert False, "expected ReciterError"
    except ReciterError:
        pass


def test_every_demo_phrase_word_is_reciter_covered() -> None:
    """Every word tools/sam_phrases.txt's non-override phrases use must
    recite without raising -- a regression lock so an edit to either file
    can't silently break the demo phrase bank's generation."""
    words = ["hello", "world", "good", "morning", "now", "playing",
             "track", "saved", "voice", "test", "the", "drum"]
    for w in words:
        al = word_to_allophones(w)
        assert al, f"'{w}': reciter produced no allophones"
        for a in al:
            assert a in _VALID, f"'{w}': emitted '{a}', not a valid SAM allophone"


def main() -> None:
    passed = 0
    failures = []

    def run(name, fn):
        nonlocal passed
        try:
            fn()
            print(f"PASS  {name}")
            passed += 1
        except AssertionError as exc:
            print(f"FAIL  {name}: {exc}")
            failures.append(name)
        except Exception:
            print(f"ERROR {name}")
            traceback.print_exc()
            failures.append(name)

    run("simple consonant-vowel-consonant words, stress on the vowel", test_simple_consonant_vowel_words)
    run("silent-e lengthens the preceding vowel", test_silent_e_lengthens_vowel)
    run("vowel-team digraphs (ee/ai/oa/oo/oy)", test_digraphs)
    run("r-controlled vowels (ar/er/ir/or)", test_r_controlled_vowels)
    run("-ing suffix and plain NG", test_ing_suffix)
    run("doubled consonant collapses to one sound", test_doubled_consonant_collapses)
    run("whole-word exceptions match their curated pronunciation", test_word_exceptions)
    run("function words (e.g. \"the\") carry no stress", test_function_words_unstressed)
    run("non-alphabetic input raises ReciterError", test_non_alpha_raises)
    run("every tools/sam_phrases.txt demo word recites cleanly", test_every_demo_phrase_word_is_reciter_covered)

    print()
    print(f"{passed} passed, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
