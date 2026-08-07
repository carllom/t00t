"""XM note -> playback frequency -> Q8.24 phase increment (#14).

tracker.md build order step 3 asks for period -> increment tables
"precomputed" at convert time (one 96-entry table per sample, see
blob_format.SampleHeader.note_increments_offset) so the device never runs
period math — it indexes an array.

Both of XM's frequency modes are implemented from the format's public,
long-documented formulas/constants:

- **Linear** (`freq_table = 1`): a closed-form exponential, exact.
- **Amiga** (`freq_table = 0`): the classic 12-entry ProTracker period
  table (the same one reproduced in essentially every MOD/XM format
  writeup), octave-scaled by note and linearly interpolated for finetune.

Caveat (see also engine.md and the plan for #14): sanity-checked against
the well-known anchor (note C-4, finetune 0, relative_note 0 -> exactly
8363 Hz, the XM sample-rate reference), not verified bit-exact against a
real FT2/OpenMPT period table dump. That level of precision matters once
something actually plays these increments back (the mixer issue); a v1
loader only needs internally-consistent, correctly-ordered pitches.
"""

from __future__ import annotations

import math

from blob_format import NOTES_PER_TABLE

SAMPLE_RATE = 44100
XM_BASE_FREQ_HZ = 8363.0  # the reference rate XM samples are authored against

# --- Linear frequency mode --------------------------------------------------
# Period = 1920 - Note*16 - FineTune/8   (FineTune -128..127 == -1..+1 semitone,
#          1 semitone == 16 period units, hence /8 to scale a byte to that range)
# Frequency = 8363 * 2^((1152 - Period) / 192)
# Sanity: Note=48 (C-4), FineTune=0 -> Period=1152 -> Frequency=8363 Hz exactly.
_LINEAR_NOTE_SCALE = 16.0
_LINEAR_BASE_PERIOD = 10 * 12 * 16  # 1920
_LINEAR_FREQ_PERIOD = 6 * 12 * 16   # 1152
_LINEAR_OCTAVE_PERIODS = 12 * 16    # 192


def _linear_frequency(note: float, finetune: float) -> float:
    period = _LINEAR_BASE_PERIOD - note * _LINEAR_NOTE_SCALE - finetune / 8.0
    return XM_BASE_FREQ_HZ * (2.0 ** ((_LINEAR_FREQ_PERIOD - period) / _LINEAR_OCTAVE_PERIODS))


# --- Amiga (non-linear) frequency mode --------------------------------------
# The classic ProTracker period table for one octave (C..B), finetune 0.
# Anchored so that index 0 (C) of this table is XM note 36 (C-3); one octave
# up (XM note 48, C-4) halves it to 428 - the historical Amiga period whose
# NTSC-clock frequency (7159090.5 / (2*428) ~= 8363 Hz) is where XM's 8363 Hz
# reference comes from in the first place.
_AMIGA_OCTAVE_BASE_NOTE = 36  # XM note of this table's C entry
_AMIGA_PERIOD_TABLE = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453]
_AMIGA_NTSC_CLOCK = 7159090.5


def _amiga_period_at_note(note_int: int) -> float:
    rel = note_int - _AMIGA_OCTAVE_BASE_NOTE
    octave, idx = divmod(rel, 12)
    return _AMIGA_PERIOD_TABLE[idx] / (2.0 ** octave)


def _amiga_frequency(note: float, finetune: float) -> float:
    # finetune is treated as a fractional-note offset (see linear mode's
    # comment on the -128..127 == -1..+1 semitone convention) and resolved
    # by linearly interpolating between the two neighbouring integer-note
    # table periods.
    effective_note = note + finetune / 128.0
    n0 = math.floor(effective_note)
    frac = effective_note - n0
    p0 = _amiga_period_at_note(int(n0))
    p1 = _amiga_period_at_note(int(n0) + 1)
    period = p0 + (p1 - p0) * frac
    return _AMIGA_NTSC_CLOCK / (2.0 * period)


def note_frequency(note: float, finetune: float, linear: bool) -> float:
    """note: 0-based semitone index from C-0 (== sample.relative_note + played note).
    finetune: -128..127. linear: freq_table flag from the module header."""
    return _linear_frequency(note, finetune) if linear else _amiga_frequency(note, finetune)


def q8_24_increment(frequency_hz: float) -> int:
    """tracker.md 'Fixed-Point Formats': inc = f_note / f_mix, Q8.24."""
    inc = round(frequency_hz / SAMPLE_RATE * (1 << 24))
    return max(0, min(inc, 0xFFFFFFFF))


def note_increment_table(relative_note: int, finetune: int, linear: bool) -> list:
    """96 Q8.24 increments, one per XM note 1..96 (0-based index 0..95)."""
    table = []
    for played_note in range(NOTES_PER_TABLE):  # 0-based; XM note = played_note + 1
        note = relative_note + played_note
        freq = note_frequency(float(note), float(finetune), linear)
        table.append(q8_24_increment(freq))
    return table
