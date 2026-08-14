"""nrl_rules.py -- host-side English letter-to-sound engine (#35, module_speech.md
"Host Tooling" / Phase 3's "NRL letter-to-sound rules engine (~400 rules,
6-8 KB)" -- kept host-only exactly as that line recommends: the device never
links this file or any rule table, it only ever sees the phoneme bytes
speechgen.py's gen-phrases baked into phrases.h).

Same shape as the original NRL Report 7948 (Elovitz et al. 1976) algorithm:
an ordered list of context-sensitive rules per starting letter, tried
longest-pattern-first, each rule constraining the text immediately to the
left and right of the letters it matches. The original table has ~400
entries built up over years of chasing English spelling's exceptions; this
one is a few dozen, covering common digraphs/vowel-teams/suffixes plus a
short whole-word exception list for irregular high-frequency words (THE, OF,
ONE, ...). speechgen.py's phrase-list override syntax ({SYM SYM ...}) is the
intended escape hatch for whatever this rule set gets wrong, per #35's own
framing ("that is what the override is for, rather than expanding the rule
set indefinitely") -- so this file grows only when a real phrase needs it,
not preemptively.

Output phonemes are symbol strings matching tools/speech_phonemes.csv's
`symbol` column exactly (e.g. "K_CL", "K_BR", "AE") -- speechgen.py validates
every symbol this module emits against the loaded CSV before writing
phrases.h, so a typo here fails the build instead of silently emitting an
out-of-range phoneme index.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, List, Tuple

VOWELS = "AEIOUY"

# Context template characters, same vocabulary as the original NRL notation
# (Elovitz et al. 1976 sec. 3): each maps to a regex fragment. ' ' is a
# literal word boundary because words are padded with one boundary space on
# each side before matching (see word_to_phonemes()) -- so "left ends in a
# boundary" and "right starts with a boundary" are both just `' '` in the
# padded string, no special-casing needed at the real start/end of the word.
_CONTEXT_RE = {
    "#": r"[AEIOUY]+",            # one or more vowels
    ":": r"[^AEIOUY ]*",          # zero or more consonants
    "^": r"[^AEIOUY ]",           # exactly one consonant
    ".": r"[BDVGJLMNR]",          # one voiced consonant
    "+": r"[EIY]",                # one front vowel
    "%": r"(?:ER|E|ES|ED|ING|ELY)",  # a common suffix
    " ": r" ",                    # word boundary
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
    phonemes: Tuple[str, ...]

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


class LetterToSoundError(Exception):
    """No rule (not even a letter's unconditional default) matched -- only
    possible for a character outside A-Z, since every letter has a
    context-free fallback rule at the end of its group (see RULES)."""


# Rule groups, keyed by the first character of `letters`. Within a group,
# order matters -- earlier entries are tried first, so longer/more specific
# patterns are listed before a letter's own bare default. This is the same
# ordering discipline the original algorithm uses (its rule file is one
# alphabetized block per letter, longest-context-first); it is not enforced
# by this module, only by the discipline of writing new rules above the
# default when adding them.
#
# Silent-e ("magic e": CVCe -> long vowel, e itself silent) is handled as two
# halves that fall out of ordinary context matching, same as the original
# algorithm: the vowel rule's *right* context spans across the following
# consonant to the trailing E ("^E$" = one consonant, then E, then word
# end), and the E rule's own *left* context ("^", one consonant, at word
# end) is what makes that E silent (empty phonemes) rather than falling
# through to E's bare default. Long O has no dedicated phoneme in this
# engine's set (phonemes.h has no OW row -- utterance.h's HELLO hit the same
# gap) so O's silent-e rule reuses O itself, the same substitution
# utterance.h already established.
RULES: Dict[str, List[Rule]] = {
    "A": [
        Rule("A", "", "^E", ("EY",)),           # make, cake, name (silent-e ahead)
        Rule("A", "", "I", ("EY",)),             # rain -- rare to hit (AI below usually wins)
        Rule("A", "", "Y", ("EY",)),             # -- rare, AY digraph below usually wins
        Rule("A", " ", " ", ("AX",)),            # standalone "a" (article) -- schwa
        Rule("A", "", "", ("AE",)),              # default short a: cat, had
    ],
    "E": [
        Rule("E", "^", " ", ("",)),              # silent trailing e after one consonant (magic e)
        Rule("E", "", "", ("E",)),                # default short e: bed, pet
    ],
    "I": [
        Rule("I", "", "^E", ("AY",)),            # time, bike (silent-e ahead)
        Rule("I", "", "", ("IH",)),               # default short i: hid, sit
    ],
    "O": [
        Rule("O", "", "^E", ("O",)),              # home, note -- O stands in for missing long-O
        Rule("O", "", " ", ("O",)),                # word-final bare o: go, hello -- same long-O stand-in
        Rule("O", "", "", ("A",)),                  # default short o: hot, dog (same vowel as "hod")
    ],
    "U": [
        Rule("U", "", "^E", ("U",)),              # cute, tube (silent-e ahead)
        Rule("U", "", "", ("AH",)),                # default short u: cup, hut
    ],
    "B": [Rule("B", "", "", ("B_CL", "B_BR"))],
    "C": [
        Rule("CK", "", "", ("K_CL", "K_BR")),
        Rule("CH", "", "", ("CH_CL", "CH_BR")),
        Rule("C", "", "+", ("S",)),                # ce/ci/cy: cent, city, cycle
        Rule("C", "", "", ("K_CL", "K_BR")),        # default hard c: cat, cot
    ],
    "D": [Rule("D", "", "", ("D_CL", "D_BR"))],
    "F": [
        Rule("F", "", "", ("F",)),
    ],
    "G": [
        Rule("GH", "#", "", ("",)),                # night, though -- silent after a vowel
        Rule("GH", " ", "", ("G_CL", "G_BR")),      # ghost -- word-initial gh is hard
        Rule("GN", "", " ", ("N",)),                 # sign -- silent g before word-final n
        Rule("GN", " ", "", ("N",)),                  # gnome -- silent g word-initial before n
        Rule("G", "", "+", ("JH_CL", "JH_BR")),        # ge/gi/gy: gem, giant (imperfect -- "get"/"give" are exceptions)
        Rule("G", "", "", ("G_CL", "G_BR")),            # default hard g: go, dog
    ],
    "H": [
        Rule("H", "", "", ("HH",)),
    ],
    "J": [Rule("J", "", "", ("JH_CL", "JH_BR"))],
    "K": [
        Rule("KN", " ", "", ("N",)),                # know, knee -- silent k word-initial
        Rule("K", "", "", ("K_CL", "K_BR")),
    ],
    "L": [Rule("L", "", "", ("L",))],
    "M": [
        Rule("MB", "", " ", ("M",)),                # comb, climb -- silent b word-final
        Rule("M", "", "", ("M",)),
    ],
    "N": [
        Rule("NG", "", "", ("NG",)),                 # sing, finger (approximation -- true /Ng/ vs /N/+/g/ split is skipped)
        Rule("N", "", "", ("N",)),
    ],
    "P": [
        Rule("PH", "", "", ("F",)),
        Rule("P", "", "", ("P_CL", "P_BR")),
    ],
    "Q": [Rule("QU", "", "", ("K_CL", "K_BR", "W"))],
    "R": [Rule("R", "", "", ("R",))],
    "S": [
        Rule("SION", "", "", ("SH", "AX", "N")),      # vision, decision (imperfect -- true ZH ignored)
        Rule("SH", "", "", ("SH",)),
        Rule("S", "", "", ("S",)),
    ],
    "T": [
        Rule("TION", "", "", ("SH", "AX", "N")),      # nation, station
        Rule("TH", "", "", ("TH",)),
        Rule("T", "", "", ("T_CL", "T_BR")),
    ],
    "V": [Rule("V", "", "", ("V",))],
    "W": [
        Rule("WOR", "", "", ("W", "ER")),             # work, word, world, worm -- w-rounding shift,
                                                        # reliably /w-er/ not the usual "or" digraph /O R/
        Rule("WR", " ", "", ("R",)),                 # write, wrong -- silent w word-initial
        Rule("WH", " ", "", ("W",)),                  # what, where (who is a whole-word exception)
        Rule("W", "", "", ("W",)),
    ],
    "X": [
        Rule("X", " ", "", ("Z",)),                    # rare word-initial x (xylophone)
        Rule("X", "", "", ("K_CL", "K_BR", "S")),        # default: box, six
    ],
    "Y": [
        Rule("Y", " ", "#", ("Y",)),                     # word-initial before a vowel: yes, you
        Rule("Y", "", "", ("IH",)),                       # default: gym, and word-final after a consonant
    ],
    "Z": [Rule("Z", "", "", ("Z",))],
}

# Two-letter vowel teams and r-controlled vowels. Kept as their own groups
# (not folded into the single-letter tables above) because they are
# genuinely two-character patterns with no useful single-letter fallback of
# their own -- e.g. "EA" isn't "E's rule with an A somewhere nearby", it is
# its own unit. Letter-pattern names below are spelling, not phonemes; do
# not confuse e.g. the letters "AY" (-> phoneme EY, as in "day") with the
# phoneme symbol "AY" (long i, as in "my") -- they are unrelated tokens that
# happen to share four characters.
_DIGRAPHS: List[Rule] = [
    Rule("EE", "", "", ("I",)),        # see, feed
    Rule("EA", "", "", ("I",)),        # eat, read (default; "bread" etc. are exceptions)
    Rule("AI", "", "", ("EY",)),       # rain, main
    Rule("AY", "", "", ("EY",)),       # day, play
    Rule("OA", "", "", ("O",)),        # boat, road
    Rule("OW", "", "", ("AW",)),       # cow, how (default; "snow" etc. are exceptions)
    Rule("OU", "", "", ("AW",)),       # out, house (default; many exceptions)
    Rule("OO", "", "", ("U",)),        # moon, food (default; short-oo words are whole-word exceptions)
    Rule("IE", "", " ", ("AY",)),      # pie, tie -- word-final
    Rule("IE", "", "", ("I",)),        # field, believe -- elsewhere
    Rule("EI", "", "", ("EY",)),       # eight, vein
    Rule("EW", "", "", ("U",)),        # new, grew
    Rule("UE", "", "", ("U",)),        # blue, true
    Rule("AU", "", "", ("O",)),        # caught, author
    Rule("AW", "", "", ("O",)),        # saw, draw -- letters "aw", not phoneme AW (see header note)
    Rule("OI", "", "", ("OY",)),       # oil, coin
    Rule("OY", "", "", ("OY",)),       # boy, toy
    Rule("AR", "", "", ("A", "R")),    # car, star
    Rule("ER", "", "", ("ER",)),       # her, fern
    Rule("IR", "", "", ("ER",)),       # bird, first
    Rule("UR", "", "", ("ER",)),       # burn, turn
    Rule("OR", "", "", ("O", "R")),    # for, or
]

# -ED's classic three-way voicing split (Elovitz-style layered rules: most
# specific left-context first). Handled inline in word_to_phonemes() rather
# than as ordinary Rule entries -- the split depends on the single letter
# immediately before "ed", which the '.'/'^' context templates can express
# as a *class* but not as "look up which class this specific letter is in
# to pick a different output", so a plain Python branch is clearer than
# stretching the template language to cover it.
_ED_VOICELESS_BEFORE = set("PKFSXH")  # walked, laughed, fixed, wished-ish approx


def _classify_group(groups: List[Rule]) -> List[Rule]:
    return sorted(groups, key=lambda r: -len(r.letters))


# Common irregular / high-frequency words a general spelling rule would get
# wrong (function words especially -- "the"/"of"/"one" are exactly the words
# a phrase bank leans on most and rules handle worst). Checked before the
# rule engine runs at all; this is the practical version of #35's own
# guidance ("the rules get wrong ... that is what the override is for") pre
# -applied to the small set of words common enough to be worth it here,
# instead of leaving every phrase author to discover and override them by
# ear one at a time.
WORD_EXCEPTIONS: Dict[str, Tuple[str, ...]] = {
    "A": ("AX",),
    "I": ("AY",),
    "THE": ("DH", "AX"),
    "OF": ("AH", "V"),
    "TO": ("T_CL", "T_BR", "U"),
    "ONE": ("W", "AH", "N"),
    "TWO": ("T_CL", "T_BR", "U"),
    "ARE": ("A", "R"),
    "IS": ("IH", "Z"),
    "WAS": ("W", "AH", "Z"),
    "WERE": ("W", "ER"),
    "DOES": ("D_CL", "D_BR", "AH", "Z"),
    "SAID": ("S", "E", "D_CL", "D_BR"),
    "WHAT": ("W", "AH", "T_CL", "T_BR"),
    "WHO": ("HH", "U"),
    "WHERE": ("W", "E", "R"),
    "YOU": ("Y", "U"),
    "YOUR": ("Y", "O", "R"),
    "THEY": ("DH", "EY"),
    "THERE": ("DH", "E", "R"),
    "THEIR": ("DH", "E", "R"),
    "THIS": ("DH", "IH", "S"),
    "THAT": ("DH", "AE", "T_CL", "T_BR"),
    "THESE": ("DH", "I", "Z"),
    "THOSE": ("DH", "O", "Z"),
    "THEN": ("DH", "E", "N"),
    "THAN": ("DH", "AE", "N"),
    "WITH": ("W", "IH", "DH"),
    "GOOD": ("G_CL", "G_BR", "UH", "D_CL", "D_BR"),
    "BOOK": ("B_CL", "B_BR", "UH", "K_CL", "K_BR"),
    "LOOK": ("L", "UH", "K_CL", "K_BR"),
    "FOOT": ("F", "UH", "T_CL", "T_BR"),
    "WOOD": ("W", "UH", "D_CL", "D_BR"),
    "MY": ("M", "AY"),
    "BY": ("B_CL", "B_BR", "AY"),
}


def word_to_phonemes(word: str) -> List[str]:
    """NRL-style letter-to-sound for one word. Returns phoneme symbol strings
    (tools/speech_phonemes.csv `symbol` column values); raises
    LetterToSoundError only for characters outside A-Z, since every letter
    in RULES carries an unconditional default rule."""
    upper = word.upper()
    if upper in WORD_EXCEPTIONS:
        return list(WORD_EXCEPTIONS[upper])
    if not upper.isalpha():
        raise LetterToSoundError(
            f"'{word}': only letters A-Z are supported by the rule engine -- "
            f"use a {{SYM SYM ...}} override for anything else (digits, punctuation)"
        )

    padded = " " + upper + " "
    phonemes: List[str] = []
    pos = 1
    end = len(padded) - 1
    while pos < end:
        ch = padded[pos]
        rule = None
        # Doubled consonant collapses to one sound (letter, not phoneme, has
        # already fired for the first occurrence): "hello"'s LL, "pattern"'s
        # TT, "off"'s FF are each one consonant sound in speech regardless of
        # spelling. Vowels are excluded -- doubled vowels ("EE", "OO") are
        # their own digraphs handled above, not a repeat of the single-letter
        # rule.
        if ch not in VOWELS and ch == padded[pos - 1]:
            pos += 1
            continue
        if ch == "E" and padded[pos:pos + 2] == "ED" and pos > 1:
            # -ED suffix: only at the tail of the word, checked ahead of the
            # ordinary E-group rules so it doesn't fall into the silent-e
            # rule (which would wrongly treat the D as trailing context it
            # doesn't understand) or the plain default-E rule.
            if padded[pos + 2] == " ":
                left_char = padded[pos - 1]
                if left_char in "TD":
                    rule = Rule("ED", "", "", ("AX", "D_CL", "D_BR"))
                elif left_char in _ED_VOICELESS_BEFORE:
                    rule = Rule("ED", "", "", ("T_CL", "T_BR"))
                else:
                    rule = Rule("ED", "", "", ("D_CL", "D_BR"))
        if rule is None and ch == "I" and padded[pos:pos + 3] == "ING" and padded[pos + 3] == " ":
            rule = Rule("ING", "", "", ("IH", "NG"))
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
            raise LetterToSoundError(f"'{word}': no rule matched '{ch}' at position {pos - 1}")
        if rule.phonemes != ("",):
            phonemes.extend(rule.phonemes)
        pos += len(rule.letters)
    return phonemes


if __name__ == "__main__":
    # Quick manual smoke test: `python3 tools/nrl_rules.py word [word ...]`
    import sys
    for w in sys.argv[1:] or ["hello", "world", "cat", "the", "good", "morning"]:
        try:
            print(w, "->", " ".join(word_to_phonemes(w)))
        except LetterToSoundError as exc:
            print(w, "-> ERROR:", exc)
