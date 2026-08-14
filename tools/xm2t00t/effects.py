"""XM effect normalization (#14): map raw XM effect/volume bytes onto a
named `Effect`/`VolEffect` enum instead of leaving the device to decode
XM's letter-and-nibble scheme at runtime. Also serves as the "internal
enum" module_tracker.md's multi-format section describes as the seam a MOD/S3M
loader would target instead of raw XM bytes.

XM already stores the pattern effect column as a small integer (0-9 map to
'0'-'9', 10-35 map to 'A'-'Z') rather than an ASCII letter, so no text
parsing is needed here — just a lookup table from that integer to a name,
with the 'E' (extended) and 'X' (extra fine porta) commands decomposed by
their parameter's high nibble into distinct enum values, since a device
consuming this blob should never need to switch on a raw nibble.
"""

from __future__ import annotations

import enum
from typing import Tuple


class Effect(enum.IntEnum):
    NONE = 0
    ARPEGGIO = 1
    PORTA_UP = 2
    PORTA_DOWN = 3
    TONE_PORTA = 4
    VIBRATO = 5
    TONE_PORTA_VOLSLIDE = 6
    VIBRATO_VOLSLIDE = 7
    TREMOLO = 8
    SET_PANNING = 9
    SAMPLE_OFFSET = 10
    VOLUME_SLIDE = 11
    POSITION_JUMP = 12
    SET_VOLUME = 13
    PATTERN_BREAK = 14
    SET_SPEED = 15
    SET_BPM = 16
    SET_GLOBAL_VOLUME = 17
    GLOBAL_VOLUME_SLIDE = 18
    KEY_OFF = 19
    SET_ENVELOPE_POSITION = 20
    PANNING_SLIDE = 21
    RETRIG_NOTE = 22
    TREMOR = 23
    EXTRA_FINE_PORTA_UP = 24
    EXTRA_FINE_PORTA_DOWN = 25
    # Decomposed extended (Exy) sub-commands, keyed by param high nibble.
    FINE_PORTA_UP = 26
    FINE_PORTA_DOWN = 27
    SET_GLISSANDO_CONTROL = 28
    SET_VIBRATO_CONTROL = 29
    SET_FINETUNE = 30
    PATTERN_LOOP = 31
    SET_TREMOLO_CONTROL = 32
    FINE_VOLSLIDE_UP = 33
    FINE_VOLSLIDE_DOWN = 34
    NOTE_CUT = 35
    NOTE_DELAY = 36
    PATTERN_DELAY = 37
    # Anything the file's effect/param byte doesn't map to a known meaning
    # (unused letters, unused E-sub-commands) — preserved, not dropped, so a
    # malformed or exotic module fails loud in the mixer rather than being
    # silently misinterpreted by the converter.
    UNKNOWN = 255


class VolEffect(enum.IntEnum):
    NONE = 0
    SET_VOLUME = 1        # param 0..64
    VOLSLIDE_DOWN = 2      # param 0..15
    VOLSLIDE_UP = 3
    FINE_VOLSLIDE_DOWN = 4
    FINE_VOLSLIDE_UP = 5
    SET_VIBRATO_SPEED = 6
    VIBRATO = 7
    SET_PANNING = 8        # param 0..15
    PANSLIDE_LEFT = 9
    PANSLIDE_RIGHT = 10
    TONE_PORTA = 11        # param 0..15


# XM effect-type byte (0-35, i.e. '0'-'9' then 'A'-'Z') -> Effect, for the
# codes that don't need param-nibble decomposition. 'E' (14) and 'X' (33)
# are handled separately by decode_effect() below. Letters with no standard
# XM meaning (I,J,M,N,O,Q,S,U,V,W,Y,Z) map to UNKNOWN.
_SIMPLE_EFFECT = {
    0: Effect.ARPEGGIO,
    1: Effect.PORTA_UP,
    2: Effect.PORTA_DOWN,
    3: Effect.TONE_PORTA,
    4: Effect.VIBRATO,
    5: Effect.TONE_PORTA_VOLSLIDE,
    6: Effect.VIBRATO_VOLSLIDE,
    7: Effect.TREMOLO,
    8: Effect.SET_PANNING,
    9: Effect.SAMPLE_OFFSET,
    10: Effect.VOLUME_SLIDE,        # A
    11: Effect.POSITION_JUMP,       # B
    12: Effect.SET_VOLUME,          # C
    13: Effect.PATTERN_BREAK,       # D
    # 14 'E' -> extended, decomposed below
    # 15 'F' -> SET_SPEED or SET_BPM depending on param, decomposed below
    16: Effect.SET_GLOBAL_VOLUME,   # G
    17: Effect.GLOBAL_VOLUME_SLIDE, # H
    20: Effect.KEY_OFF,             # K
    21: Effect.SET_ENVELOPE_POSITION,  # L
    25: Effect.PANNING_SLIDE,       # P
    27: Effect.RETRIG_NOTE,         # R
    29: Effect.TREMOR,              # T
    # 33 'X' -> extra fine porta, decomposed below
}

# Exy (effect type 14, 'E'): sub-command keyed by the param's high nibble.
_EXTENDED_SUB = {
    0x1: Effect.FINE_PORTA_UP,
    0x2: Effect.FINE_PORTA_DOWN,
    0x3: Effect.SET_GLISSANDO_CONTROL,
    0x4: Effect.SET_VIBRATO_CONTROL,
    0x5: Effect.SET_FINETUNE,
    0x6: Effect.PATTERN_LOOP,
    0x7: Effect.SET_TREMOLO_CONTROL,
    0x9: Effect.RETRIG_NOTE,
    0xA: Effect.FINE_VOLSLIDE_UP,
    0xB: Effect.FINE_VOLSLIDE_DOWN,
    0xC: Effect.NOTE_CUT,
    0xD: Effect.NOTE_DELAY,
    0xE: Effect.PATTERN_DELAY,
}

# Xxy (effect type 33, 'X'): FT2's "extra fine portamento".
_EXTRA_FINE_PORTA_SUB = {
    0x1: Effect.EXTRA_FINE_PORTA_UP,
    0x2: Effect.EXTRA_FINE_PORTA_DOWN,
}


def decode_effect(effect_type: int, param: int) -> Tuple[Effect, int]:
    """Normalize a raw (effect_type, param) pattern-cell pair.

    Returns (Effect, param_byte). For decomposed Exy/Xxy commands,
    param_byte is the low nibble only (the high nibble selected the Effect
    value, so it would be redundant — and a device switching on `effect`
    should never need to re-inspect it).
    """
    if effect_type == 0 and param == 0:
        # Arpeggio with a zero param is a no-op — the standard convention
        # (shared by every XM-compatible player) for "this cell has no
        # effect at all", since XM has no dedicated "none" byte value.
        return Effect.NONE, 0
    if effect_type == 14:  # E
        sub = _EXTENDED_SUB.get(param >> 4)
        if sub is None:
            return Effect.UNKNOWN, param
        return sub, param & 0x0F
    if effect_type == 33:  # X
        sub = _EXTRA_FINE_PORTA_SUB.get(param >> 4)
        if sub is None:
            return Effect.UNKNOWN, param
        return sub, param & 0x0F
    if effect_type == 15:  # F — XM overloads one letter for both; split it.
        return (Effect.SET_SPEED, param) if param < 0x20 else (Effect.SET_BPM, param)
    eff = _SIMPLE_EFFECT.get(effect_type)
    return (eff, param) if eff is not None else (Effect.UNKNOWN, param)


def decode_volume(vol_byte: int) -> Tuple[VolEffect, int]:
    """Normalize the XM pattern volume-column byte (0x00..0xFF)."""
    if vol_byte < 0x10:
        return VolEffect.NONE, 0
    if vol_byte <= 0x50:
        return VolEffect.SET_VOLUME, vol_byte - 0x10
    band, param = divmod(vol_byte, 0x10)
    table = {
        0x6: VolEffect.VOLSLIDE_DOWN,
        0x7: VolEffect.VOLSLIDE_UP,
        0x8: VolEffect.FINE_VOLSLIDE_DOWN,
        0x9: VolEffect.FINE_VOLSLIDE_UP,
        0xA: VolEffect.SET_VIBRATO_SPEED,
        0xB: VolEffect.VIBRATO,
        0xC: VolEffect.SET_PANNING,
        0xD: VolEffect.PANSLIDE_LEFT,
        0xE: VolEffect.PANSLIDE_RIGHT,
        0xF: VolEffect.TONE_PORTA,
    }
    eff = table.get(band)
    if eff is None:
        return VolEffect.NONE, 0
    return eff, param
