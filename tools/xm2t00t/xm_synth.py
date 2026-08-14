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
FX_SAMPLE_OFFSET = 9   # 9
FX_VOLUME_SLIDE = 10   # A
FX_POSITION_JUMP = 11  # B
FX_SET_VOLUME = 12     # C
FX_PATTERN_BREAK = 13  # D
FX_EXTENDED = 14       # E -- decomposed by the param's high nibble, see _ext_param()
FX_SET_SPEED_TEMPO = 15  # F -- < 0x20 is speed, >= 0x20 is BPM
FX_RETRIG = 27         # R -- full-byte form (volume-change nibble + interval nibble)

XM_MAGIC = b"Extended Module: "


def _ext_param(sub: int, value: int) -> int:
    """Packs an Exy sub-command's selector nibble and value nibble into the
    single param byte FX_EXTENDED expects, matching effects.py's
    decode_effect() decomposition (param >> 4 selects the sub-command,
    param & 0x0F is what reaches the device as Effect's own param)."""
    return ((sub & 0xF) << 4) | (value & 0xF)


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
    ping_pong: bool = False  # #21: loop_type 2 instead of 1 when a loop is present

    @property
    def loop_type(self) -> int:
        if self.loop_end <= self.loop_start:
            return 0
        return 2 if self.ping_pong else 1


@dataclass
class SynthInstrument:
    sample: SynthSample
    name: str = ""
    # #20 envelope/autovibrato/fadeout fixtures: all optional, default to
    # "off" so every pre-#20 fixture above is unaffected. Envelope points
    # are (tick, value 0..64) pairs, XM convention (absolute tick, not
    # delta) -- at most 12, matching the file format's fixed-size table.
    vol_env_points: List[Tuple[int, int]] = field(default_factory=list)
    vol_sustain: int = 0
    vol_loop_start: int = 0
    vol_loop_end: int = 0
    vol_env_on: bool = False
    vol_sustain_on: bool = False
    vol_loop_on: bool = False
    pan_env_points: List[Tuple[int, int]] = field(default_factory=list)
    pan_sustain: int = 0
    pan_loop_start: int = 0
    pan_loop_end: int = 0
    pan_env_on: bool = False
    pan_sustain_on: bool = False
    pan_loop_on: bool = False
    vibrato_type: int = 0
    vibrato_sweep: int = 0
    vibrato_depth: int = 0
    vibrato_rate: int = 0
    fadeout: int = 0


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


def _env_points_bytes(points: List[Tuple[int, int]]) -> bytes:
    out = bytearray()
    for tick, value in points:
        out += struct.pack("<HH", tick, value)
    out += bytes(4 * (12 - len(points)))  # pad the unused slots of the fixed 12-point table
    return bytes(out)


def _env_type_byte(enabled: bool, sustain_on: bool, loop_on: bool) -> int:
    return (0x01 if enabled else 0) | (0x02 if sustain_on else 0) | (0x04 if loop_on else 0)


def _instrument_bytes(inst: SynthInstrument) -> bytes:
    ext = bytearray()
    ext += struct.pack("<I", 40)          # sample_header_size
    ext += bytes([0]) * 96                # sample_map: every note -> local sample 0
    ext += _env_points_bytes(inst.vol_env_points)
    ext += _env_points_bytes(inst.pan_env_points)
    ext += bytes([len(inst.vol_env_points), len(inst.pan_env_points)])
    ext += bytes([inst.vol_sustain, inst.vol_loop_start, inst.vol_loop_end])
    ext += bytes([inst.pan_sustain, inst.pan_loop_start, inst.pan_loop_end])
    ext += bytes([_env_type_byte(inst.vol_env_on, inst.vol_sustain_on, inst.vol_loop_on),
                  _env_type_byte(inst.pan_env_on, inst.pan_sustain_on, inst.pan_loop_on)])
    ext += bytes([inst.vibrato_type, inst.vibrato_sweep, inst.vibrato_depth, inst.vibrato_rate])
    ext += struct.pack("<H", inst.fadeout)  # volume_fadeout
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
    that belongs to the FT2 quirk tail (#25), not to this fixture's asserted
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
    issued on (module_tracker.md: 'samples_per_tick ... MUST be per-block'), which a
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


# --- #20 fixtures: instrument envelopes, key-off/fadeout, volume column,
# autovibrato. Same principle as #19's per-effect fixtures: narrow enough
# that openmpt123 and player.h's #20 implementation should land in the same
# place, so a real divergence reads as a bug. All use the looped
# `_pad_sample()` so the *sample* never runs out from under the envelope --
# any silence is the envelope's/fadeout's doing, not the sample ending.

def volume_envelope_basic() -> bytes:
    """A held note through a full attack/decay/sustain/release volume
    envelope: ramp 0->64 by tick 4, decay to the sustain level (40) by tick
    10, hold there (sustain point, index 2) for as long as the note is held,
    then -- on key-off -- release down to 0 by tick 30. Exercises sustain
    hold (acceptance: 'matches sustain point') and release-after-key-off
    ('does not cut the voice instantly')."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    inst = SynthInstrument(
        pad, "vol_env",
        vol_env_points=[(0, 0), (4, 64), (10, 40), (30, 0)],
        vol_sustain=2, vol_env_on=True, vol_sustain_on=True,
    )
    rows = [[_fx_ev(49, 1)]] + [[_fx_ev()]] * 7 + [[_fx_ev(note=97)]] + [[_fx_ev()]] * 9
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=[inst], speed=4, bpm=125))


def panning_envelope_basic() -> bytes:
    """A held note, center-panned by default, with a looping panning
    envelope sweeping left/right/center/right -- exercises the envelope's
    asymmetric attenuation-toward-center formula (a centered base pan gets
    the envelope's full swing) and its independent loop (no sustain point,
    so it cycles for as long as the note plays, unlike the volume envelope
    fixture above)."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    inst = SynthInstrument(
        pad, "pan_env",
        pan_env_points=[(0, 32), (8, 64), (16, 0), (24, 32)],
        pan_loop_start=0, pan_loop_end=3, pan_env_on=True, pan_loop_on=True,
    )
    rows = [[_fx_ev(49, 1)]] + [[_fx_ev()]] * 11
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=[inst], speed=4, bpm=125))


def fadeout_basic() -> bytes:
    """A volume envelope that releases (on key-off) to a nonzero value and
    then holds there forever (no loop past its last point) -- fadeout is
    what finishes the job, decaying `fadeout_vol` the rest of the way to
    true silence once the envelope itself has nothing left to do. Verified
    against openmpt123 (#20): fadeout is a *release* mechanism for an
    envelope-driven instrument, not a substitute for one -- an instrument
    with no envelope at all cuts (almost) instantly at key-off regardless of
    its fadeout field, which is exactly what retrig_and_keyoff's/
    test_note_trigger_and_retrigger's plain (no-envelope) instrument
    exercises instead."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    inst = SynthInstrument(
        pad, "fadeout",
        vol_env_points=[(0, 0), (4, 64), (10, 40), (16, 20)],
        vol_sustain=2, vol_env_on=True, vol_sustain_on=True,
        fadeout=4096,
    )
    rows = [[_fx_ev(49, 1)]] + [[_fx_ev()]] * 7 + [[_fx_ev(note=97)]] + [[_fx_ev()]] * 9
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=[inst], speed=4, bpm=125))


def volume_column_extended() -> bytes:
    """One channel walking through the volume column's less-common *level/
    pan* bands: fine volslide down/up (instant, tick 0 only), coarse
    volslide down (continuous), and panslide left then right. Deliberately
    excludes the vol column's vibrato (Ax/Bx) and tone-porta (Fx) bands --
    those drive the shared continuous-pitch-oscillator/glide machinery,
    which (like the effect-column vibrato, tracker_tick_period()'s own
    header comment) is not chased to openmpt123 bit-exactness here; their
    *mechanism* is covered by
    test_volcol_vibrato_and_toneporta_no_retrigger() in render_xm_device.cpp
    instead. set_volume_basic/retrig_and_keyoff already cover the vol
    column's SET_VOLUME/SET_PANNING bands, so this deliberately covers the
    rest of the non-oscillator ones."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    FINE_DOWN = 0x80 + 8    # fine volslide down 8
    FINE_UP = 0x90 + 4      # fine volslide up 4
    VOLSLIDE_DOWN = 0x60 + 3  # continuous volslide down 3/tick
    PANSLIDE_L = 0xD0 + 5     # panning slide left 5/tick
    PANSLIDE_R = 0xE0 + 5     # panning slide right 5/tick

    def ev(note=0, inst=0, vol=0):
        return SynthEvent(note=note, instrument=inst, volume=vol)

    rows = [
        ev(49, 1),          # trigger, full volume/center pan
        ev(vol=FINE_DOWN),  # instant -8 (once)
        ev(vol=FINE_UP),    # instant +4 (once)
        ev(vol=VOLSLIDE_DOWN),  # continuous -3/tick for the rest of this row
        ev(vol=PANSLIDE_L), # continuous pan slide toward 0
        ev(vol=PANSLIDE_R), # continuous pan slide back toward 255
    ]
    return build_xm(SynthSong(num_channels=1, rows=[[r] for r in rows], instruments=instruments, speed=4, bpm=125))


# --- #21 fixtures: ping-pong loops, 9xx sample offset. --------------------

def ping_pong_basic() -> bytes:
    """A held note on a ping-pong-looped sample: the loop region [4,20) of
    the 24-frame _pad_sample() bounces back and forth for the rest of the
    row, so a turnaround-boundary bug (off-by-one, missed reflection, wrong
    guard sample) shows up as an audible discontinuity within a single row
    rather than needing many rows to accumulate. mixer.h's own multi-bounce/
    exact-integer-boundary edge cases are unit-tested directly in
    tools/host_render/render_tracker_mixer.cpp (mixer.h has no dependency on
    the XM format); this fixture is the end-to-end openmpt123 check that the
    player+mixer pairing (loop_type=2 all the way from the blob) sounds
    right, not just that the reflection math is internally consistent."""
    pad = SynthSample(data=_pad_sample(), loop_start=4, loop_end=20, volume=64, panning=128,
                       ping_pong=True, name="pingpong")
    instruments = [SynthInstrument(pad, "pingpong")]

    rows = [[_fx_ev(49, 1)]] + [[_fx_ev()]] * 7
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def _offset_sample() -> bytes:
    """600 frames, two audibly distinct halves (loud, then quiet, both the
    same tone) -- long enough that 9xx's 256-frame-granularity units land
    solidly within each half, so starting mid-sample via 9xx is easy to tell
    apart from starting at 0 in a diff, and an implementation that ignores
    9xx or gets the *256 scaling wrong reads as a wrong (too loud) render
    rather than a subtle level shift."""
    import math
    n = 600
    return bytes(int((100 if i < 300 else 30) * math.sin(2 * math.pi * i / 16.0)) & 0xFF for i in range(n))


def sample_offset_basic() -> bytes:
    """9xx sample offset: row0 triggers a long one-shot at offset 2
    (512 frames in, deep into the sample's quiet second half) -- an
    implementation that ignores 9xx, or gets the *256 scaling wrong, starts
    at 0 and renders the loud first half instead. Row1 retriggers with
    param 0, reusing that same offset (memory). Row2 retriggers a
    *different*, short instrument with an offset (0xFF*256 = 65280 frames)
    far past its length -- FT2/openmpt123 suppress the trigger entirely in
    this case, so nothing new should sound on that row at all."""
    long_sample = SynthSample(data=_offset_sample(), loop_end=0, volume=64, panning=128, name="offset_long")
    short_sample = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="offset_short")
    instruments = [SynthInstrument(long_sample, "offset_long"), SynthInstrument(short_sample, "offset_short")]

    rows = [
        [_fx_ev(49, 1, fx=FX_SAMPLE_OFFSET, param=2)],     # offset 512 frames into the 600-frame sample
        [_fx_ev(49, 1, fx=FX_SAMPLE_OFFSET, param=0)],     # memory reuse -- same offset again
        [_fx_ev(61, 2, fx=FX_SAMPLE_OFFSET, param=0xFF)],  # far past instrument 2's length -- suppressed
        [_fx_ev()],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=8, bpm=125))


def voice_count_profile() -> bytes:
    """Not a correctness fixture -- deliberately **not** in FIXTURES below, so
    `diff_xm.py` never touches it. A device-side hardware profiling aid for
    #21's acceptance criterion ("per-voice cost after ping-pong support is
    re-measured against #16's baseline"): reproduces #16's self-cycling
    idle/8/16/24/32-voice profiling-pin measurement, but as a real song
    played through the actual #18 player+mixer, since audio_engine.cpp's own
    synthetic phase-cycling rig (the `PHASE_IDLE`/`PHASE_8`/.../`PHASE_32_*`
    enum in the #16 commit) no longer exists post-#18 -- the real
    `tracker_render_buffer()` call in today's `audio_engine.cpp` always
    renders every channel the song actually uses. No firmware changes
    needed, just a one-command `tracker_song_blob.h` regenerate (see
    `tracker_song_blob.h`'s own header comment for the exact command) to
    swap this in for the normal demo song, then swap back afterward.

    32 channels, one pattern, ~3.8s per phase at speed=6/bpm=125 (32 rows/
    phase; samples_per_tick=882, so one row = 6*882 = 5292 frames = 120ms,
    close to #16's 4s PHASE_HOLD_FRAMES): silence, then channels 0-7, 8-15,
    16-23, 24-31 join cumulatively and hold (8/16/24/32 active), then every
    channel *retriggers* onto a tiny one-shot silent sample before the
    pattern loops back to row 0, repeating the whole cycle forever exactly
    like #16's rig did.

    The release row deliberately does **not** use key-off (note 97): this
    engine's key-off never deactivates a `TrackerVoice` in mixer.h, only its
    *target volume* (via the envelope/fadeout path, #20) -- with no volume
    envelope, that reaches 0 almost instantly, so the channel goes silent,
    but `v->active` stays true and mix_voice() keeps fully interpolating and
    accumulating it forever at zero output. Measured on real hardware
    (the author, 2026-08-07): the profiling pin stays pinned at the 32-voice duty
    cycle straight through the "silent" idle phase after the first lap --
    display/UI correctly shows the channels as off (its active_mask reads
    target volume, not v->active), but the mixer was still doing full
    32-voice work the whole time. A genuine, pre-existing engine
    characteristic this song's own idle-phase readings would otherwise
    mislead about, not a #21 regression -- worth its own future issue
    (proper `ECx` note-cut, or making key-off deactivate a voice once its
    volume has fully decayed) but out of scope here. The fix used instead:
    retrigger each channel onto a **one-shot** sample every real trigger
    naturally deactivates once played through (mixer.h's `wrap_loop()`:
    "One-shot voices go inactive at their end instead of wrapping") --
    silent (all-zero data) and short (256 frames -- a few ms at any
    realistic pitch, negligible against a 3.8s idle window) so it's both
    inaudible and fast to finish.

    Channel 0 is always a *ping-pong* tight (4-sample, loop covering the
    entire sample) loop -- #16's own "one voice on a deliberately tight
    loop" worst case, carried over to ping-pong specifically since a tighter
    loop means more `wrap_ping_pong()` boundary crossings per second, the
    new cost #21 adds -- so it's active, and its overhead measured, through
    every non-idle phase. Channels 1-31 hold `_pad_sample()`'s ordinary
    forward loop, same sustain sample every other fixture in this module
    uses, spread across a couple of octaves so the result isn't a flat
    unison drone."""
    tight = SynthSample(data=bytes(v & 0xFF for v in (100, 60, -60, -100)),
                         loop_start=0, loop_end=4, volume=48, panning=128,
                         ping_pong=True, name="tight_pingpong")
    chorus = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()),
                          volume=32, panning=128, name="chorus")
    silence = SynthSample(data=bytes(256), loop_end=0, volume=0, panning=128, name="silence")
    instruments = [SynthInstrument(tight, "tight_pingpong"), SynthInstrument(chorus, "chorus"),
                   SynthInstrument(silence, "silence")]

    NUM_CHANNELS = 32
    ROWS_PER_PHASE = 32

    def empty_row() -> List[SynthEvent]:
        return [SynthEvent() for _ in range(NUM_CHANNELS)]

    def join_row(lo: int, hi: int) -> List[SynthEvent]:
        row = empty_row()
        for ch in range(lo, hi):
            row[ch] = SynthEvent(note=49, instrument=1) if ch == 0 \
                else SynthEvent(note=40 + (ch % 20), instrument=2)
        return row

    rows: List[List[SynthEvent]] = [empty_row() for _ in range(ROWS_PER_PHASE)]  # idle
    for lo, hi in ((0, 8), (8, 16), (16, 24), (24, 32)):
        rows.append(join_row(lo, hi))
        rows.extend(empty_row() for _ in range(ROWS_PER_PHASE - 1))
    # Release: retrigger every channel onto the one-shot silent sample so
    # each voice actually goes inactive (v->active = false) once it plays
    # through, instead of key-off's "silent but still fully mixed forever".
    rows.append([SynthEvent(note=49, instrument=3) for _ in range(NUM_CHANNELS)])
    rows.extend(empty_row() for _ in range(ROWS_PER_PHASE - 1))

    return build_xm(SynthSong(num_channels=NUM_CHANNELS, rows=rows, instruments=instruments,
                               speed=6, bpm=125, name="voice_count_profile"))


# --- #22 fixtures: the remaining Exy sub-commands (E60 pattern loop and the
# other named FT2 quirks are deliberately deferred, not covered here -- see
# module_tracker.md's Open Questions / Build Order for the split). Same principle
# as every earlier per-effect fixture in this module: narrow enough that
# openmpt123 and player.h's implementation should land in the same place.

def fine_slides_basic() -> bytes:
    """Fine portamento (E1x/E2x) and fine volume slide (EAx/EBx) -- each
    applies once, at tick 0 only, unlike the continuous 1xx/2xx/Axy effects
    already covered by porta_up_down/volume_slide_basic. A held note steps
    pitch up then back down, and volume up then down, one discrete step per
    row -- audibly and structurally distinct from a continuous slide."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=32, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0x1, 8))],  # E18: fine porta up 8
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0x2, 8))],  # E28: fine porta down 8 -- back to start
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0xA, 8))],  # EA8: fine volslide up 8
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0xB, 4))],  # EB4: fine volslide down 4
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


# No glissando_basic fixture: E3x (glissando control) was tried and
# reverted from player.h -- two reasonable snap-to-semitone implementations
# both diverged from openmpt123 within a couple of ticks of the glide
# starting, which makes the exact behaviour genuinely diff-driven quirk work
# rather than the bounded/mechanical case the rest of this module's #22
# fixtures cover. Tracked in #25 alongside the four named FT2 quirks
# (module_tracker.md's Open Questions).


def retrig_basic() -> bytes:
    """E9x retriggers the sample every y ticks within the row -- a rapid
    stutter, structurally distinct from a single sustained trigger. Uses the
    one-shot pluck sample so each retrigger's restart-from-0 is unambiguous
    (a looped sample's own wrap could otherwise coincidentally line up)."""
    pluck = SynthSample(data=_pluck_sample(), loop_end=0, volume=64, panning=128, name="pluck")
    instruments = [SynthInstrument(pluck, "pluck")]

    rows = [
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0x9, 2))],  # E92: retrigger every 2 ticks
        [_fx_ev()],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def note_cut_basic() -> bytes:
    """ECx cuts the channel's volume to 0 partway through the row -- the
    tail of the row is silent instead of sustaining. Row 2 uses param 0
    (cut on the trigger tick itself, immediately) as the boundary case."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1, fx=FX_EXTENDED, param=_ext_param(0xC, 3))],  # EC3: cut at tick 3 of this row
        [_fx_ev(49, 1)],
        [_fx_ev(fx=FX_EXTENDED, param=_ext_param(0xC, 0))],         # EC0: cut immediately
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def note_delay_basic() -> bytes:
    """EDx defers this row's trigger to a later tick within the row -- the
    row starts silent and the note only begins partway through it."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev()],
        [_fx_ev(49, 1, fx=FX_EXTENDED, param=_ext_param(0xD, 3))],  # ED3: trigger delayed to tick 3
        [_fx_ev()],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=6, bpm=125))


def pattern_delay_basic() -> bytes:
    """EEx holds the current row for y additional full-speed passes before
    advancing -- row 0's note sustains roughly twice as long as row 1's
    otherwise-identical duration."""
    pad = SynthSample(data=_pad_sample(), loop_start=0, loop_end=len(_pad_sample()), volume=64, panning=128, name="pad")
    instruments = [SynthInstrument(pad, "pad")]

    rows = [
        [_fx_ev(49, 1, fx=FX_EXTENDED, param=_ext_param(0xE, 1))],  # EE1: hold this row for 1 extra pass
        [_fx_ev(61, 1)],
    ]
    return build_xm(SynthSong(num_channels=1, rows=rows, instruments=instruments, speed=4, bpm=125))


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
    "volume_envelope_basic": volume_envelope_basic,
    "panning_envelope_basic": panning_envelope_basic,
    "fadeout_basic": fadeout_basic,
    "volume_column_extended": volume_column_extended,
    "ping_pong_basic": ping_pong_basic,
    "sample_offset_basic": sample_offset_basic,
    "fine_slides_basic": fine_slides_basic,
    "retrig_basic": retrig_basic,
    "note_cut_basic": note_cut_basic,
    "note_delay_basic": note_delay_basic,
    "pattern_delay_basic": pattern_delay_basic,
}
