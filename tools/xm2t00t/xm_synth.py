"""Synthetic, byte-valid .xm builder (#17): produces small regression-corpus
modules *from checked-in code* rather than checked-in binary files.

`test_xm2t00t.py`'s corpus (`../../xm/`) is real, third-party Mod Archive
material and deliberately gitignored -- not something this repo can commit.
`tools/host_render/diff_xm.py`'s reference-diff harness needs at least one
module it can assert a tight tolerance against (a real module will legitimately
diverge the moment it uses anything beyond notes -- envelopes, effects -- none
of which src/engines/tracker/player.h implements yet). A module built here,
by contrast, is authored to stay entirely within player.h's notes-only scope,
so openmpt123 and the t00t player should agree closely: this is the "expected
result" half of issue #17's "module + expected result" acceptance criterion.

#19 adds one small, targeted fixture per effect command (arpeggio, porta
up/down, tone porta, vibrato, volume slide, set volume, position jump +
pattern break, speed/tempo) on the same principle: author something narrow
enough that openmpt123 and player.h's effect state machines should land in
the same place, so a real divergence reads as a bug instead of noise. Kept
short and low-tempo on purpose -- a continuous pitch effect (porta, tone
porta, vibrato) accumulates phase error against the reference every sample
it runs, so a fixture that held a bend for a long time would fail on a
timing-precision question the -10dB windowed-RMS metric was never meant to
answer, not a correctness one.

Implements just enough of the standard FastTracker 2 XM binary layout (the
inverse of xm_parser.py's read side) to produce a file real players accept:
module header, one pattern (uncompressed 5-byte cells -- no need to implement
the bitmask compression, only unpacking it), one instrument per voice with a
single 8-bit sample, delta-encoded PCM (the inverse of xm_parser.delta_decode_8).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# XM effect-column letters ('0'-'9'->0-9, 'A'-'Z'->10-35), matching
# tools/xm2t00t/effects.py's raw-byte convention (SynthEvent.effect is the
# same small integer the real format's pattern cell stores, not an ASCII
# letter).
FX_ARPEGGIO = 0
FX_PORTA_UP = 1
FX_PORTA_DOWN = 2
FX_TONE_PORTA = 3
FX_VIBRATO = 4
FX_VOLUME_SLIDE = 10   # A
FX_POSITION_JUMP = 11  # B
FX_SET_VOLUME = 12     # C
FX_PATTERN_BREAK = 13  # D
FX_SET_SPEED_TEMPO = 15  # F -- < 0x20 is speed, >= 0x20 is BPM

XM_MAGIC = b"Extended Module: "


def encode_delta_8(data: bytes) -> bytes:
    """Inverse of xm_parser.delta_decode_8: running difference mod 256, so
    decoding this back with the running sum reproduces `data` exactly."""
    out = bytearray(len(data))
    prev = 0
    for i, v in enumerate(data):
        out[i] = (v - prev) & 0xFF
        prev = v
    return bytes(out)


@dataclass
class SynthEvent:
    note: int = 0
    instrument: int = 0
    volume: int = 0   # raw XM volume-column byte (0 = none)
    effect: int = 0
    param: int = 0


@dataclass
class SynthSample:
    data: bytes             # 8-bit signed PCM, as unsigned bytes (two's complement)
    loop_start: int = 0
    loop_end: int = 0       # 0 = no loop (loop_type forced to 0)
    volume: int = 64
    panning: int = 128
    finetune: int = 0
    relative_note: int = 0
    name: str = ""

    @property
    def loop_type(self) -> int:
        return 1 if self.loop_end > self.loop_start else 0


@dataclass
class SynthInstrument:
    sample: SynthSample
    name: str = ""


@dataclass
class SynthSong:
    num_channels: int
    rows: List[List[SynthEvent]]        # single-pattern shorthand; ignored if `patterns` is set
    instruments: List[SynthInstrument]
    speed: int = 6                      # ticks per row
    bpm: int = 125
    linear_freq: bool = True
    name: str = "t00t synth fixture"
    # Multi-pattern form (#19, for position-jump/pattern-break fixtures):
    # each entry is one pattern's rows, same shape as `rows`. `order_table`
    # indexes into `patterns`; defaults to playing each pattern once in
    # order. Leave both None for the original single-pattern behaviour.
    patterns: Optional[List[List[List[SynthEvent]]]] = None
    order_table: Optional[List[int]] = None
    restart_position: int = 0


def _pad(s: str, n: int, fill: bytes = b" ") -> bytes:
    b = s.encode("latin-1", "replace")[:n]
    return b + fill * (n - len(b))


def _sample_header(s: SynthSample) -> bytes:
    type_byte = s.loop_type & 0x03  # bit4 (16-bit) stays 0 -- v1 is 8-bit only
    return struct.pack(
        "<IIIBbBBb",
        len(s.data), s.loop_start, max(0, s.loop_end - s.loop_start),
        s.volume & 0xFF, s.finetune, type_byte, s.panning & 0xFF, s.relative_note,
    ) + b"\x00" + _pad(s.name, 22)  # +1 reserved byte per the standard 40-byte layout


def _instrument_bytes(inst: SynthInstrument) -> bytes:
    ext = bytearray()
    ext += struct.pack("<I", 40)          # sample_header_size
    ext += bytes([0]) * 96                # sample_map: every note -> local sample 0
    ext += bytes(12 * 4)                  # vol envelope points (unused, count=0)
    ext += bytes(12 * 4)                  # pan envelope points (unused, count=0)
    ext += bytes([0, 0])                  # num_vol_points, num_pan_points
    ext += bytes([0, 0, 0])               # vol sustain/loop_start/loop_end
    ext += bytes([0, 0, 0])               # pan sustain/loop_start/loop_end
    ext += bytes([0, 0])                  # vol_type, pan_type (envelopes off)
    ext += bytes([0, 0, 0, 0])            # vibrato type/sweep/depth/rate
    ext += struct.pack("<H", 0)           # volume_fadeout
    ext += bytes(22)                      # reserved -- real FT2 files pad the extended
    # instrument header to 234 bytes here (giving the classic header_size=263 total);
    # xm_parser.py itself doesn't care (it trusts the declared header_size to skip
    # forward), but a size short of that apparently makes at least one real-world
    # player (libopenmpt/openmpt123, empirically -- see #17) treat the instrument as
    # a legacy/degenerate one with no attached samples, silently. Match the real
    # constant instead of the format's documented minimum.
    assert len(ext) == 4 + 96 + 48 + 48 + 16 + 22 == 234

    header_size = 29 + len(ext)
    head = struct.pack("<I", header_size) + _pad(inst.name, 22) + b"\x00" + struct.pack("<H", 1)
    assert len(head) == 29
    sample_hdr = _sample_header(inst.sample)
    pcm = encode_delta_8(inst.sample.data)
    return bytes(head) + bytes(ext) + sample_hdr + pcm


def _pattern_bytes(rows: List[List[SynthEvent]], num_channels: int) -> bytes:
    cells = bytearray()
    for row in rows:
        assert len(row) == num_channels
        for ev in row:
            cells += bytes([ev.note & 0xFF, ev.instrument & 0xFF, ev.volume & 0xFF,
                             ev.effect & 0xFF, ev.param & 0xFF])
    header = struct.pack("<IBHH", 9, 0, len(rows), len(cells))
    assert len(header) == 9
    return header + bytes(cells)


def build_xm(song: SynthSong) -> bytes:
    out = bytearray()
    out += XM_MAGIC
    out += _pad(song.name, 20)
    out += b"\x1a"
    out += _pad("t00t xm_synth", 20)
    out += struct.pack("<H", 0x0104)  # version

    patterns = song.patterns if song.patterns is not None else [song.rows]
    order = song.order_table if song.order_table is not None else list(range(len(patterns)))
    order_table = bytes(order) + bytes(256 - len(order))

    body = struct.pack(
        "<HHHHHHHH",
        len(order),                         # song_length (orders used)
        song.restart_position,
        song.num_channels,
        len(patterns),                      # num_patterns
        len(song.instruments),
        1 if song.linear_freq else 0,       # flags
        song.speed,
        song.bpm,
    ) + order_table
    # header_size counts itself (xm_parser.py: `pos = 60 + header_size` lands on the
    # first pattern, and 60 is where the header_size field itself starts) -- +4 for
    # that field, not just the body that follows it.
    out += struct.pack("<I", len(body) + 4)
    out += body

    for pat_rows in patterns:
        out += _pattern_bytes(pat_rows, song.num_channels)
    for inst in song.instruments:
        out += _instrument_bytes(inst)

    return bytes(out)


# --- Concrete fixtures -----------------------------------------------------

def _pluck_sample() -> bytes:
    """A short, one-shot, harmonically simple 8-bit waveform (a few cycles of
    a fattened sine) -- enough spectral content for a pitch/loop/retrigger
    bug to actually show up in a WAV diff, not silence or a DC value that
    would hide one. XM sample data is *signed* 8-bit, centered at 0 (mixer.h
    reads it via `const int8_t*`) -- no +128 bias, unlike unsigned PCM/WAV
    convention."""
    import math
    n = 64
    return bytes(int(100 * math.sin(2 * math.pi * i / 16.0)) & 0xFF for i in range(n))


def _pad_sample() -> bytes:
    """A short, cleanly-looping 8-bit waveform for the sustain/loop-wrap
    coverage (retrig_and_keyoff's ch1 sustains this across several rows)."""
    n = 24
    return bytes((((i * 255) // n) - 128) & 0xFF for i in range(n))  # one saw cycle


def notes_basic() -> bytes:
    """ch0: four distinct ascending notes, one per row, default sample
    volume. ch1: a single looped note held under all four rows (loop-wrap
    coverage, per-note-only, no retrigger). Exercises: multi-note sequencing,
    correct per-note increment lookup, and a sustained loop.

    Both samples use dead-center panning (128): pan.h deliberately uses an
    equal-power law (see its own header comment), which real FT2/libopenmpt
    does not necessarily match -- a permanent, by-design divergence source
    that belongs to the FT2 quirk tail (#22), not to this fixture's asserted
    tolerance. Center pan sidesteps it so this corpus tests what player.h
    actually claims to get right: notes, pitch, volume, triggers, loops."""
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=48, panning=128, name="pad")
    instruments = [SynthInstrument(pluck, "pluck"), SynthInstrument(pad, "pad")]

    def ev(note=0, inst=0, vol=0):
        return SynthEvent(note=note, instrument=inst, volume=vol)

    notes = [49, 53, 56, 61]  # C-4, E-4, G-4, C-5 (relative_note=0, so table index == note-1)
    rows = []
    for i, n in enumerate(notes):
        rows.append([ev(n, 1), ev(60, 2) if i == 0 else ev()])
    return build_xm(SynthSong(num_channels=2, rows=rows, instruments=instruments, speed=6, bpm=125))


def retrig_and_keyoff() -> bytes:
    """ch0: trigger, retrigger the same note (generation-counter coverage),
    then a key-off row. ch1: SET_VOLUME then SET_PANNING vol-column commands
    on separate triggers (both are direct sets, not per-tick effects, so
    in scope for the notes-only player -- see player.h's header comment)."""
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    instruments = [SynthInstrument(pluck, "pluck")]

    SET_VOLUME = 0x10 + 40   # vol-column encoding: 0x10..0x50 -> volume 0..64
    SET_PANNING = 0xC0 + 8   # vol-column encoding: 0xC0..0xCF -> pan 0..15

    def ev(note=0, inst=0, vol=0):
        return SynthEvent(note=note, instrument=inst, volume=vol)

    rows = [
        [ev(45, 1), ev(45, 1, SET_VOLUME)],
        [ev(),      ev()],
        [ev(45, 1), ev(45, 1, SET_PANNING)],
        [ev(97),    ev()],
    ]
    return build_xm(SynthSong(num_channels=2, rows=rows, instruments=instruments, speed=4, bpm=140))


# --- #19 effect fixtures ----------------------------------------------------
# One small module per effect (or closely-related pair), each just long
# enough to exercise memory/restatement and tick-0-vs-later-tick semantics.
# All use the sustained `_pad_sample()` loop except where a one-shot
# transient makes the trigger point easier to read off in a diff (jump/break).

def _fx_ev(note=0, inst=0, vol=0, fx=0, param=0):
    return SynthEvent(note=note, instrument=inst, volume=vol, effect=fx, param=param)


def arpeggio_basic() -> bytes:
    """One channel, one held note, arpeggio (0xy) cycling base/+x/+y every
    tick -- exercises tick_in_row%3 cycling (acceptance: 'cycles correctly,
    including at high speeds') and that an empty effect column stops it."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],                          # C-4 trigger, no effect
        [_fx_ev(fx=FX_ARPEGGIO, param=0x47)],     # base / +4 / +7 semitones
        [_fx_ev(fx=FX_ARPEGGIO, param=0x37)],     # different offsets, no retrigger
        [_fx_ev()],                               # empty effect column -- arpeggio stops
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def porta_up_down() -> bytes:
    """One channel: trigger, porta up (1xx) for two rows (second reuses
    memory via a zero param), then porta down (2xx) for two rows -- separate
    per-command memory, and `period` persisting/reversing correctly."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_PORTA_UP, param=0x08)],
        [_fx_ev(fx=FX_PORTA_UP, param=0x00)],    # memory reuse
        [_fx_ev(fx=FX_PORTA_DOWN, param=0x08)],
        [_fx_ev(fx=FX_PORTA_DOWN, param=0x00)],  # memory reuse
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


def tone_porta_basic() -> bytes:
    """Trigger C-4, then a higher note with tone portamento (3xx) -- must
    NOT retrigger (no new generation, no click), just glide the existing
    voice toward the new target over a couple of rows using memory."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],                            # C-4
        [_fx_ev(56, 1, fx=FX_TONE_PORTA, param=0x20)],  # target G-4
        [_fx_ev(fx=FX_TONE_PORTA, param=0x00)],     # memory reuse, keep gliding
        [_fx_ev(fx=FX_TONE_PORTA, param=0x00)],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


def vibrato_basic() -> bytes:
    """One held note, vibrato (4xy) for a single row, minimal speed/depth --
    just enough exposure to prove the sine table/waveform is being applied
    (direction and rough shape) without running long enough to accumulate
    the phase-drift-vs-openmpt123 error a sustained oscillation would (see
    module docstring). Effect memory itself is already covered by
    porta_up_down/tone_porta_basic/volume_slide_basic -- same decode path,
    no need to re-prove it here at vibrato's much tighter error budget."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_VIBRATO, param=0x11)],  # speed 1, depth 1
        [_fx_ev()],                           # stops
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def volume_slide_basic() -> bytes:
    """Full-volume trigger, slide down (Axy, y nonzero) to clamp at 0, then
    slide back up (x nonzero) -- exercises nibble decode, the 0..64 clamp,
    and memory reuse."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_VOLUME_SLIDE, param=0x0F)],  # down 15/tick
        [_fx_ev(fx=FX_VOLUME_SLIDE, param=0x00)],  # memory reuse (still down)
        [_fx_ev(fx=FX_VOLUME_SLIDE, param=0xF0)],  # up 15/tick
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


def set_volume_basic() -> bytes:
    """Effect-column Cxx (distinct from the vol column, already covered by
    retrig_and_keyoff) sets volume immediately, tick 0 only."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],                      # full volume (64)
        [_fx_ev(fx=FX_SET_VOLUME, param=0x20)],  # instantly half (32)
        [_fx_ev(fx=FX_SET_VOLUME, param=0x40)],  # instantly back to full (64)
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


def jump_and_break() -> bytes:
    """4 patterns, 2 channels (ch0 carries an audible, distinct note per
    landing point so a diff shows exactly where playback actually went; ch1
    carries the transport effects so they never share a cell with a note).

    pat0 row1: Dxx alone -> pattern break to pat1 *row 1* (not row 0) --
               row 0 of pat1 must never sound.
    pat1 row1: Bxx alone -> position jump to order 2 (pat2), landing row 0.
    pat2 row1: Bxx AND Dxx on the *same row* (different channels) -> jump to
               order 3 (pat3) landing at row 1, not row 0 -- the "B and D
               interact ... including break-with-jump on the same row"
               acceptance criterion.
    pat3 row1: lands here; the order list then ends normally and loops.
    """
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    instruments = [SynthInstrument(pluck, "pluck")]

    def note_row(note, fx0=0, param0=0, fx1=0, param1=0):
        return [_fx_ev(note, 1 if note else 0, fx=fx0, param=param0), _fx_ev(fx=fx1, param=param1)]

    pat0 = [
        note_row(49),                                   # row0: C-4
        note_row(0, fx1=FX_PATTERN_BREAK, param1=0x01),  # row1: D -> row 1 of next pattern
    ]
    pat1 = [
        note_row(61),                                    # row0: C-5 -- must be skipped
        note_row(63, fx1=FX_POSITION_JUMP, param1=2),     # row1: D-5, then B -> order 2
        note_row(65),                                     # row2: E-5 -- must be skipped
    ]
    pat2 = [
        note_row(66),                                                              # row0: F-5
        note_row(68, fx0=FX_POSITION_JUMP, param0=3, fx1=FX_PATTERN_BREAK, param1=0x01),  # row1: G-5, B+D combined -> order 3 row 1
    ]
    pat3 = [
        note_row(70),  # row0: A-5 -- must be skipped (landed at row 1)
        note_row(72),  # row1: B-5 -- the actual landing point
    ]
    return build_xm(SynthSong(
        num_channels=2, rows=[], instruments=instruments, speed=3, bpm=125,
        patterns=[pat0, pat1, pat2, pat3],
    ))


def speed_tempo_basic() -> bytes:
    """One held note across a speed change (Fxx, param < 0x20) then a tempo
    change (Fxx, param >= 0x20) -- both must take effect at the tick they're
    issued on (tracker.md: 'samples_per_tick ... MUST be per-block'), which a
    timing-sensitive diff against openmpt123 is exactly the right check."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_SET_SPEED_TEMPO, param=0x03)],  # speed -> 3
        [_fx_ev(fx=FX_SET_SPEED_TEMPO, param=0x64)],  # bpm -> 100
        [_fx_ev()],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


FIXTURES = {
    "notes_basic": notes_basic,
    "retrig_and_keyoff": retrig_and_keyoff,
    "arpeggio_basic": arpeggio_basic,
    "porta_up_down": porta_up_down,
    "tone_porta_basic": tone_porta_basic,
    "vibrato_basic": vibrato_basic,
    "volume_slide_basic": volume_slide_basic,
    "set_volume_basic": set_volume_basic,
    "jump_and_break": jump_and_break,
    "speed_tempo_basic": speed_tempo_basic,
}
