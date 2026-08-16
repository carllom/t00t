"""sam_reciter.py -- host-side English letter-to-sound + stress engine for
the SAM tract (#72). Independent from tools/nrl_rules.py (the formant
tract's own reciter): its own rule table, its own output vocabulary
(sam2allophones.py's SAM_ALLOPHONE_NAMES -- one allophone per stop, not the
formant tract's closure+burst pairs), and its own stress pass, none of it
imported from nrl_rules.py, per #69's "kept separate" decision. Reimplements
the same public NRL Report 7948 (Elovitz et al. 1976) letter-to-sound
technique nrl_rules.py's own header comment already cites as its origin --
that published algorithm, and its context-template notation, is not
S.A.M.-specific or drawn from either unlicensed open S.A.M. C conversion.

Two passes per word: word_to_allophones() (spelling -> SAM allophone
symbols, ordered context-sensitive rules, longest-pattern-first, plus a
short whole-word exception list) then assign_stress() (which allophone
carries S.A.M.'s characteristic pitch overshoot). Predicting English stress
correctly needs a pronouncing dictionary this project doesn't have; the
heuristic here -- a content word's first non-schwa vowel is primary-stressed,
a small set of known function words carry no stress at all -- is honest
about being approximate, in the same "curated set plus inline exception
overrides" spirit nrl_rules.py's own header comment describes: a
mis-stressed word gets a STRESS_OVERRIDES entry, not a bigger heuristic.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, List, Tuple

VOWELS = "AEIOUY"

# NRL context-template notation (Elovitz et al. 1976 sec. 3), reimplemented
# independently -- see module docstring.
_CONTEXT_RE = {
    "#": r"[AEIOUY]+",
    ":": r"[^AEIOUY ]*",
    "^": r"[^AEIOUY ]",
    "+": r"[EIY]",
    " ": r" ",
}


def _compile_ctx(template: str, *, anchor_end: bool) -> "re.Pattern[str]":
    parts = [_CONTEXT_RE.get(c, re.escape(c)) for c in template]
    body = "".join(parts)
    return re.compile(body + "$" if anchor_end else "^" + body)


@dataclass(frozen=True)
class Rule:
    letters: str
    left: str
    right: str
    allophones: Tuple[str, ...]

    def __post_init__(self):
        object.__setattr__(self, "_left_re", _compile_ctx(self.left, anchor_end=True))
        object.__setattr__(self, "_right_re", _compile_ctx(self.right, anchor_end=False))

    def matches(self, padded: str, pos: int) -> bool:
        end = pos + len(self.letters)
        if padded[pos:end] != self.letters:
            return False
        if not self._left_re.search(padded[:pos]):
            return False
        if not self._right_re.match(padded[end:]):
            return False
        return True


class ReciterError(Exception):
    """No rule matched -- only possible for a character outside A-Z, since
    every letter in RULES carries an unconditional default rule."""


# SAM's own single-allophone stops (no closure/burst split -- sam.h has no
# plosive-segment mechanism, unlike the formant tract's PhonemeDef flags).
RULES: Dict[str, List[Rule]] = {
    "A": [
        Rule("A", "", "^E", ("EY",)),      # make, cake (silent-e ahead)
        Rule("A", "", "I", ("EY",)),        # rain
        Rule("A", "", "Y", ("EY",)),        # day (rare -- AY digraph below usually wins)
        Rule("A", " ", " ", ("AX",)),       # standalone "a" (article) -- schwa
        Rule("A", "", "", ("AE",)),         # default short a: cat, had
    ],
    "E": [
        Rule("E", "^", " ", ("",)),         # silent trailing e after one consonant (magic e)
        Rule("E", "", "", ("EH",)),         # default short e: bed, pet
    ],
    "I": [
        Rule("I", "", "^E", ("AY",)),       # time, bike (silent-e ahead)
        Rule("I", "", "", ("IH",)),          # default short i: hid, sit
    ],
    "O": [
        Rule("O", "", "^E", ("OW",)),       # home, note
        Rule("O", "", " ", ("OW",)),         # word-final bare o: go, hello
        Rule("O", "", "", ("AA",)),           # default short o: hot, dog
    ],
    "U": [
        Rule("U", "", "^E", ("UW",)),       # cute, tube
        Rule("U", "", "", ("AH",)),          # default short u: cup, hut
    ],
    "B": [Rule("B", "", "", ("B",))],
    "C": [
        Rule("CK", "", "", ("K",)),
        Rule("CH", "", "", ("CH",)),
        Rule("C", "", "+", ("S",)),          # ce/ci/cy: cent, city, cycle
        Rule("C", "", "", ("K",)),            # default hard c: cat, cot
    ],
    "D": [Rule("D", "", "", ("D",))],
    "F": [Rule("F", "", "", ("F",))],
    "G": [
        Rule("GH", "#", "", ("",)),          # night, though -- silent after a vowel
        Rule("GH", " ", "", ("G",)),          # ghost -- word-initial gh is hard
        Rule("GN", "", " ", ("N",)),           # sign
        Rule("GN", " ", "", ("N",)),            # gnome
        Rule("G", "", "+", ("J",)),              # ge/gi/gy: gem, giant
        Rule("G", "", "", ("G",)),                # default hard g: go, dog
    ],
    "H": [Rule("H", "", "", ("HX",))],
    "J": [Rule("J", "", "", ("J",))],
    "K": [
        Rule("KN", " ", "", ("N",)),          # know, knee -- silent k word-initial
        Rule("K", "", "", ("K",)),
    ],
    "L": [Rule("L", "", "", ("L",))],
    "M": [
        Rule("MB", "", " ", ("M",)),          # comb, climb -- silent b word-final
        Rule("M", "", "", ("M",)),
    ],
    "N": [
        Rule("NG", "", "", ("NX",)),          # sing, finger
        Rule("N", "", "", ("N",)),
    ],
    "P": [
        Rule("PH", "", "", ("F",)),
        Rule("P", "", "", ("P",)),
    ],
    "Q": [Rule("QU", "", "", ("K", "W"))],
    "R": [Rule("R", "", "", ("R",))],
    "S": [
        Rule("SH", "", "", ("SH",)),
        Rule("S", "", "", ("S",)),
    ],
    "T": [
        Rule("TH", "", "", ("TH",)),
        Rule("T", "", "", ("T",)),
    ],
    "V": [Rule("V", "", "", ("V",))],
    "W": [
        Rule("WR", " ", "", ("R",)),          # write, wrong -- silent w word-initial
        Rule("WH", " ", "", ("WH",)),          # what, where
        Rule("W", "", "", ("W",)),
    ],
    "X": [
        Rule("X", " ", "", ("Z",)),            # rare word-initial x (xylophone)
        Rule("X", "", "", ("K", "S")),          # default: box, six
    ],
    "Y": [
        Rule("Y", " ", "#", ("Y",)),            # word-initial before a vowel: yes, you
        Rule("Y", "", "", ("IH",)),              # default: gym, and word-final after a consonant
    ],
    "Z": [Rule("Z", "", "", ("Z",))],
}

# Two-letter vowel teams and r-controlled vowels -- own unit each, not a
# single-letter fallback.
_DIGRAPHS: List[Rule] = [
    Rule("EE", "", "", ("IY",)),        # see, feed
    Rule("EA", "", "", ("IY",)),        # eat, read (default; exceptions overridden per word)
    Rule("AI", "", "", ("EY",)),        # rain, main
    Rule("AY", "", "", ("EY",)),        # day, play
    Rule("OA", "", "", ("OW",)),        # boat, road
    Rule("OW", "", "", ("OW",)),        # snow, grow -- letters "ow", default long-o
                                         # ("cow"-style /aw/ is the minority case here, overridden per word)
    Rule("OU", "", "", ("AW",)),        # out, house (default; many exceptions)
    Rule("OO", "", "", ("UW",)),        # moon, food
    Rule("IE", "", " ", ("AY",)),       # pie, tie -- word-final
    Rule("IE", "", "", ("IY",)),        # field, believe
    Rule("EI", "", "", ("EY",)),        # eight, vein
    Rule("EW", "", "", ("UW",)),        # new, grew
    Rule("UE", "", "", ("UW",)),        # blue, true
    Rule("AU", "", "", ("AO",)),        # caught, author
    Rule("AW", "", "", ("AO",)),        # saw, draw -- letters "aw"
    Rule("OI", "", "", ("OY",)),        # oil, coin
    Rule("OY", "", "", ("OY",)),        # boy, toy
    Rule("AR", "", "", ("AA", "R")),    # car, star
    Rule("ER", "", "", ("ER",)),        # her, fern
    Rule("IR", "", "", ("ER",)),        # bird, first
    Rule("UR", "", "", ("ER",)),        # burn, turn
    Rule("OR", "", "", ("AO", "R")),    # for, or
]


def _classify_group(groups: List[Rule]) -> List[Rule]:
    return sorted(groups, key=lambda r: -len(r.letters))


# Common function words -- unstressed in connected speech (see assign_stress()) and
# frequently irregular enough that a general spelling rule gets them wrong.
# (allophones, primary_stress_index or None)
WORD_EXCEPTIONS: Dict[str, Tuple[Tuple[str, ...], "int | None"]] = {
    "A": (("AX",), None),
    "I": (("AY",), 0),
    "THE": (("DH", "AX"), None),
    "OF": (("AH", "V"), None),
    "TO": (("T", "UW"), None),
    "ONE": (("W", "AH", "N"), 1),
    "TWO": (("T", "UW"), 1),
    "ARE": (("AA", "R"), None),
    "IS": (("IH", "Z"), None),
    "WAS": (("W", "AH", "Z"), None),
    "WERE": (("W", "ER"), None),
    "WHAT": (("W", "AH", "T"), 1),
    "WHO": (("HX", "UW"), 1),
    "WHERE": (("W", "EH", "R"), 1),
    "YOU": (("Y", "UW"), 1),
    "YOUR": (("Y", "AO", "R"), 1),
    "MY": (("M", "AY"), 1),
    "GOOD": (("G", "UH", "D"), 1),
    "NOW": (("N", "AW"), 1),
    "PLAYING": (("P", "L", "EY", "IH", "NX"), 2),
    "TRACK": (("T", "R", "AE", "K"), 2),
    "SAVED": (("S", "EY", "V", "D"), 1),
    "VOICE": (("V", "OY", "S"), 1),
    "TEST": (("T", "EH", "S", "T"), 1),
    "DRUM": (("D", "R", "AH", "M"), 2),
    "MACHINE": (("M", "AX", "SH", "IY", "N"), 3),  # French-derived spelling, stress on final syllable
    "MORNING": (("M", "AO", "R", "N", "IH", "NX"), 1),
    "WORLD": (("W", "ER", "L", "D"), 1),
    "HELLO": (("HX", "EH", "L", "OW"), 3),
}

_SCHWA_LIKE = {"AX", "IX"}


def word_to_allophones(word: str) -> List[str]:
    """SAM-flavored letter-to-sound for one word. Returns SAM allophone
    symbol strings (sam2allophones.py's SAM_ALLOPHONE_NAMES); raises
    ReciterError only for characters outside A-Z, since every letter in
    RULES carries an unconditional default rule."""
    upper = word.upper()
    if upper in WORD_EXCEPTIONS:
        return list(WORD_EXCEPTIONS[upper][0])
    if not upper.isalpha():
        raise ReciterError(
            f"'{word}': only letters A-Z are supported -- use a {{SYM SYM ...}} "
            f"override for anything else (digits, punctuation)"
        )

    padded = " " + upper + " "
    allophones: List[str] = []
    pos = 1
    end = len(padded) - 1
    while pos < end:
        ch = padded[pos]
        rule = None
        # Doubled consonant collapses to one sound: "hello"'s LL, "off"'s FF.
        if ch not in VOWELS and ch == padded[pos - 1]:
            pos += 1
            continue
        if ch == "I" and padded[pos:pos + 3] == "ING" and padded[pos + 3] == " ":
            rule = Rule("ING", "", "", ("IH", "NX"))
        if rule is None:
            for candidate in _classify_group(_DIGRAPHS):
                if candidate.letters[0] == ch and candidate.matches(padded, pos):
                    rule = candidate
                    break
        if rule is None:
            for candidate in RULES.get(ch, []):
                if candidate.matches(padded, pos):
                    rule = candidate
                    break
        if rule is None:
            raise ReciterError(f"'{word}': no rule matched '{ch}' at position {pos - 1}")
        if rule.allophones != ("",):
            allophones.extend(rule.allophones)
        pos += len(rule.letters)
    return allophones


def assign_stress(word: str, allophones: List[str]) -> List[bool]:
    """Which allophones (parallel to word_to_allophones()'s output) carry
    S.A.M.'s pitch overshoot. WORD_EXCEPTIONS carries an explicit index
    when one is known; otherwise every allophone of an all-function-word
    (WORD_EXCEPTIONS with a None index) is unstressed, and any other word's
    first non-schwa vowel-family allophone is stressed -- see module
    docstring for why this is a heuristic, not a lookup."""
    upper = word.upper()
    n = len(allophones)
    if upper in WORD_EXCEPTIONS:
        idx = WORD_EXCEPTIONS[upper][1]
        return [i == idx for i in range(n)] if idx is not None else [False] * n

    for i, a in enumerate(allophones):
        if a in VOWELS_ALLOPHONES and a not in _SCHWA_LIKE:
            return [j == i for j in range(n)]
    return [False] * n


# SAM allophone symbols this reciter's own rule table can emit that count as
# "a vowel" for assign_stress()'s purposes -- a fixed set (not derived from
# sam2allophones.py's full 80-entry list, most of which this reciter never
# emits) so a rule-table typo that emits an unexpected symbol can't silently
# make every word stressless.
VOWELS_ALLOPHONES = {
    "IY", "IH", "EH", "AE", "AA", "AH", "AO", "UH", "AX", "ER", "UW",
    "EY", "AY", "OY", "AW", "OW",
}


if __name__ == "__main__":
    import sys
    for w in sys.argv[1:] or ["hello", "world", "cat", "the", "good", "morning", "playing", "machine"]:
        try:
            al = word_to_allophones(w)
            st = assign_stress(w, al)
            rendered = " ".join((f"*{a}" if s else a) for a, s in zip(al, st))
            print(w, "->", rendered)
        except ReciterError as exc:
            print(w, "-> ERROR:", exc)
