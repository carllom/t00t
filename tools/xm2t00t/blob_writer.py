"""XmSong -> t00t blob bytes (#14).

Layout mirrors blob_format.py's field order. Two kinds of regions:

- **Metadata** (order table, pattern events, instrument envelopes/keymaps,
  sample headers, note-increment tables): built with a reserve-then-backfill
  pattern (`BlobBuilder.reserve` + `write_at`) since these tables reference
  offsets of data written after them. Lives in flash on the device; read
  occasionally, never in the per-sample hot loop.
- **Sample PCM** (`sample_data_offset` .. `sample_data_offset +
  sample_data_bytes`): appended as one uninterrupted run, nothing else
  interleaved into it, so the device can `memcpy` this single span into SRAM
  at song load.
"""

from __future__ import annotations

import struct
from typing import List, Tuple

import blob_format as bf
import effects
import periods
from xm_parser import XmEnvelope, XmSample, XmSong


class ConversionError(Exception):
    """Raised for a module that fails a v1 hard limit (length cap, SRAM
    budget) — callers should print `str(exc)` and exit non-zero without
    writing a blob."""


class BlobBuilder:
    def __init__(self) -> None:
        self.buf = bytearray()

    def tell(self) -> int:
        return len(self.buf)

    def reserve(self, size: int) -> int:
        start = len(self.buf)
        self.buf += bytes(size)
        return start

    def write_at(self, offset: int, data: bytes) -> None:
        self.buf[offset:offset + len(data)] = data

    def append(self, data: bytes) -> int:
        start = len(self.buf)
        self.buf += data
        return start


def _guard_byte(sample: XmSample) -> int:
    """Loop-start value for looped samples, last value for one-shots — so
    the mixer's `s[idx+1]` interpolator read never needs a bounds check
    (#14)."""
    if not sample.data:
        return 0
    if sample.loop_type != 0 and 0 <= sample.loop_start < len(sample.data):
        return sample.data[sample.loop_start]
    return sample.data[-1]


def _sample_region_size(sample: XmSample) -> int:
    n = sample.length + 1  # + guard byte
    return n + (n & 1)  # even-padded, see comment at the append site


def _write_envelope(b: BlobBuilder, env: XmEnvelope) -> Tuple[int, int]:
    if not env.points:
        return 0, 0
    packed = bytearray()
    for tick, value in env.points:
        packed += bf.ENVELOPE_POINT.pack(tick=tick, value=value)
    return b.append(bytes(packed)), len(env.points)


def _envelope_flags(env: XmEnvelope) -> int:
    return ((0x01 if env.enabled else 0)
            | (0x02 if env.sustain_enabled else 0)
            | (0x04 if env.loop_enabled else 0))


def build_blob(song: XmSong, sram_budget_bytes: int = bf.DEFAULT_SRAM_BUDGET_BYTES) -> bytes:
    # Flatten (instrument, local sample) into one global, blob-order list —
    # InstrumentHeader.sample_index_offset maps local -> this global index.
    global_samples: List[XmSample] = []
    for inst in song.instruments:
        global_samples.extend(inst.samples)

    oversized = [s for s in global_samples if s.length > bf.MAX_SAMPLE_FRAMES]
    if oversized:
        names = ", ".join(f"'{s.name or '<unnamed>'}' ({s.length} frames)" for s in oversized)
        raise ConversionError(
            f"{len(oversized)} sample(s) exceed the Q18.14 length cap of "
            f"{bf.MAX_SAMPLE_FRAMES} frames: {names}"
        )

    total_sample_bytes = sum(_sample_region_size(s) for s in global_samples)
    if total_sample_bytes > sram_budget_bytes:
        biggest = sorted(global_samples, key=lambda s: -s.length)[:5]
        listing = ", ".join(f"'{s.name or '<unnamed>'}'={s.length}f" for s in biggest)
        raise ConversionError(
            f"sample data is {total_sample_bytes} bytes, exceeds the "
            f"{sram_budget_bytes}-byte SRAM budget (module_tracker.md: realistically "
            f"350-400 KB after code/stacks/DMA/mixer scratch). "
            f"Largest samples: {listing}. Reduce sample count/length or raise "
            f"--budget-kb if you're confident it still fits."
        )

    b = BlobBuilder()
    song_header_off = b.reserve(bf.SONG_HEADER.size)

    order_table_off = b.append(bytes(song.order_table))

    # --- Patterns: reserve the header table, then append each pattern's
    # flat Event array, backfilling the header with the offsets once known.
    pattern_table_off = b.reserve(bf.PATTERN_HEADER.size * len(song.patterns))
    pattern_rows = []
    for pat in song.patterns:
        ev_bytes = bytearray()
        for ev in pat.events:
            vol_eff, vol_param = effects.decode_volume(ev.volume)
            eff, eff_param = effects.decode_effect(ev.effect, ev.param)
            ev_bytes += bf.EVENT.pack(
                note=ev.note, instrument=ev.instrument,
                vol_effect=int(vol_eff), vol_param=vol_param,
                effect=int(eff), effect_param=eff_param,
            )
        event_off = b.append(bytes(ev_bytes))
        pattern_rows.append((pat.num_rows, event_off))
    pt_bytes = bytearray()
    for num_rows, event_off in pattern_rows:
        pt_bytes += bf.PATTERN_HEADER.pack(num_rows=num_rows, event_offset=event_off)
    b.write_at(pattern_table_off, bytes(pt_bytes))

    # --- Instruments: reserve the header table, append each instrument's
    # keymap/sample-index/envelope arrays, backfill.
    instrument_table_off = b.reserve(bf.INSTRUMENT_HEADER.size * len(song.instruments))
    instrument_rows = []
    global_index = 0
    for inst in song.instruments:
        if inst.samples:
            sample_map_off = b.append(bytes(inst.sample_map))
            idx = list(range(global_index, global_index + len(inst.samples)))
            sample_index_off = b.append(struct.pack(f"<{len(idx)}I", *idx))
            global_index += len(inst.samples)
        else:
            sample_map_off = 0
            sample_index_off = 0

        vol_env_off, vol_env_count = _write_envelope(b, inst.vol_env)
        pan_env_off, pan_env_count = _write_envelope(b, inst.pan_env)

        instrument_rows.append(dict(
            sample_map_offset=sample_map_off, num_samples=len(inst.samples),
            sample_index_offset=sample_index_off,
            vol_env_offset=vol_env_off, vol_env_count=vol_env_count,
            vol_env_sustain=inst.vol_env.sustain,
            vol_env_loop_start=inst.vol_env.loop_start, vol_env_loop_end=inst.vol_env.loop_end,
            vol_env_flags=_envelope_flags(inst.vol_env),
            pan_env_offset=pan_env_off, pan_env_count=pan_env_count,
            pan_env_sustain=inst.pan_env.sustain,
            pan_env_loop_start=inst.pan_env.loop_start, pan_env_loop_end=inst.pan_env.loop_end,
            pan_env_flags=_envelope_flags(inst.pan_env),
            vibrato_type=inst.vibrato_type, vibrato_sweep=inst.vibrato_sweep,
            vibrato_depth=inst.vibrato_depth, vibrato_rate=inst.vibrato_rate,
            volume_fadeout=inst.volume_fadeout,
            name=inst.name.encode("latin-1", "replace"),
        ))
    it_bytes = bytearray()
    for row in instrument_rows:
        it_bytes += bf.INSTRUMENT_HEADER.pack(**row)
    b.write_at(instrument_table_off, bytes(it_bytes))

    # --- Samples: reserve the header table, append note-increment tables
    # (metadata, flash-resident), THEN the sample_data region as one
    # uninterrupted PCM+guard run, then backfill the header table.
    sample_table_off = b.reserve(bf.SAMPLE_HEADER.size * len(global_samples))

    inc_offsets = []
    for s in global_samples:
        table = periods.note_increment_table(s.relative_note, s.finetune, song.linear_freq)
        inc_offsets.append(b.append(struct.pack(f"<{len(table)}I", *table)))

    sample_data_off = b.tell()
    data_offsets = []
    for s in global_samples:
        pcm = s.data + bytes([_guard_byte(s)])
        if len(pcm) % 2:
            pcm += b"\x00"  # even-pad; length field below still excludes this
        data_offsets.append(b.append(pcm))
    sample_data_bytes = b.tell() - sample_data_off

    st_bytes = bytearray()
    for s, inc_off, data_off in zip(global_samples, inc_offsets, data_offsets):
        st_bytes += bf.SAMPLE_HEADER.pack(
            length=s.length, loop_start=s.loop_start, loop_end=s.loop_end,
            loop_type=s.loop_type, finetune=s.finetune, relative_note=s.relative_note,
            default_volume=s.volume, default_panning=s.panning,
            data_offset=data_off, note_increments_offset=inc_off,
            name=s.name.encode("latin-1", "replace"),
        )
    b.write_at(sample_table_off, bytes(st_bytes))

    total_size = b.tell()
    header = bf.SONG_HEADER.pack(
        magic=bf.MAGIC, version=bf.VERSION,
        num_channels=song.num_channels,
        freq_table=1 if song.linear_freq else 0,
        num_orders=len(song.order_table), num_patterns=len(song.patterns),
        num_instruments=len(song.instruments), num_samples=len(global_samples),
        default_tempo=song.default_tempo, default_bpm=song.default_bpm,
        restart_order=song.restart_position,
        order_table_offset=order_table_off, pattern_table_offset=pattern_table_off,
        instrument_table_offset=instrument_table_off, sample_table_offset=sample_table_off,
        sample_data_offset=sample_data_off, sample_data_bytes=sample_data_bytes,
        total_size=total_size, name=song.name.encode("latin-1", "replace"),
        tracker_name=song.tracker_name.encode("latin-1", "replace"),
    )
    b.write_at(song_header_off, header)

    return bytes(b.buf)
