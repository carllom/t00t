#!/usr/bin/env python3
"""Test suite for xm2t00t (#14). Run: python3 tools/xm2t00t/test_xm2t00t.py

Corpus-dependent checks look for `.xm` files in `../../xm/` (gitignored —
not committed, since they're third-party copyrighted modules). Populate it
yourself from any legal source (e.g. Mod Archive) to run those checks; if
it's empty they're skipped with a clear message rather than failing, so the
synthetic-fixture and unit checks still run to completion anywhere.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import blob_format as bf
import blob_writer
import effects
import openmpt_ref
import periods
import xm_parser as xp

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_DIR = os.path.join(HERE, "..", "..", "xm")


# --- Unit checks (no corpus needed) ----------------------------------------

def test_periods_sanity() -> None:
    # C-4, finetune 0, relative_note 0 is XM's reference: exactly 8363 Hz.
    freq_linear = periods.note_frequency(48.0, 0.0, linear=True)
    assert abs(freq_linear - 8363.0) < 0.01, freq_linear
    freq_amiga = periods.note_frequency(48.0, 0.0, linear=False)
    assert abs(freq_amiga - 8363.0) < 1.0, freq_amiga  # NTSC constant ~8363.19

    # One octave up must double the frequency in both modes.
    up_linear = periods.note_frequency(60.0, 0.0, linear=True)
    assert abs(up_linear - 2 * freq_linear) < 0.01, up_linear
    up_amiga = periods.note_frequency(60.0, 0.0, linear=False)
    assert abs(up_amiga - 2 * freq_amiga) < 0.5, up_amiga

    inc = periods.q8_24_increment(freq_linear)
    # inc = f/44100 * 2^24; at 8363 Hz that's ~0.1897 -> ~3,183,000
    assert 3_100_000 < inc < 3_250_000, inc


def test_effects_smoke() -> None:
    assert effects.decode_effect(0, 0) == (effects.Effect.NONE, 0)
    assert effects.decode_effect(10, 0x53) == (effects.Effect.VOLUME_SLIDE, 0x53)
    assert effects.decode_effect(14, 0xC4) == (effects.Effect.NOTE_CUT, 4)
    assert effects.decode_effect(15, 0x05) == (effects.Effect.SET_SPEED, 0x05)
    assert effects.decode_effect(15, 0x82) == (effects.Effect.SET_BPM, 0x82)
    assert effects.decode_effect(33, 0x15) == (effects.Effect.EXTRA_FINE_PORTA_UP, 5)
    assert effects.decode_effect(18, 0) == (effects.Effect.UNKNOWN, 0)  # 'I' unused

    assert effects.decode_volume(0x00) == (effects.VolEffect.NONE, 0)
    assert effects.decode_volume(0x30) == (effects.VolEffect.SET_VOLUME, 32)
    assert effects.decode_volume(0xC5) == (effects.VolEffect.SET_PANNING, 5)


def test_struct_roundtrip() -> None:
    packed = bf.PATTERN_HEADER.pack(num_rows=64, event_offset=12345)
    assert len(packed) == bf.PATTERN_HEADER.size == 8
    unpacked = bf.PATTERN_HEADER.unpack(packed)
    assert unpacked["num_rows"] == 64 and unpacked["event_offset"] == 12345

    packed2 = bf.SONG_HEADER.pack(
        magic=bf.MAGIC, version=1, num_channels=16, freq_table=1, num_orders=4,
        num_patterns=2, num_instruments=1, num_samples=1, default_tempo=6,
        default_bpm=125, restart_order=0, order_table_offset=0, pattern_table_offset=0,
        instrument_table_offset=0, sample_table_offset=0, sample_data_offset=0,
        sample_data_bytes=0, total_size=0, name=b"hello", tracker_name=b"world",
    )
    u2 = bf.SONG_HEADER.unpack(packed2)
    assert u2["magic"] == bf.MAGIC
    assert u2["name"] == b"hello"
    assert u2["tracker_name"] == b"world"


def _make_minimal_song(samples) -> xp.XmSong:
    inst = xp.XmInstrument(
        name="i", sample_map=[0] * 96, samples=samples,
        vol_env=xp.XmEnvelope(), pan_env=xp.XmEnvelope(),
        vibrato_type=0, vibrato_sweep=0, vibrato_depth=0, vibrato_rate=0,
        volume_fadeout=0,
    )
    pat = xp.XmPattern(num_rows=1, events=[xp.XmEvent(0, 0, 0, 0, 0)])
    return xp.XmSong(
        name="t", tracker_name="t", version=0x104, num_channels=1,
        restart_position=0, linear_freq=True, default_tempo=6, default_bpm=125,
        order_table=[0], patterns=[pat], instruments=[inst],
    )


def test_rejection_length_cap() -> None:
    big = xp.XmSample(name="huge", data=bytes(10), length=bf.MAX_SAMPLE_FRAMES + 1,
                       loop_start=0, loop_end=0, loop_type=0, volume=64,
                       finetune=0, panning=128, relative_note=0)
    song = _make_minimal_song([big])
    try:
        blob_writer.build_blob(song)
        raise AssertionError("expected ConversionError for over-length-cap sample")
    except blob_writer.ConversionError as exc:
        assert "262144" in str(exc), str(exc)
        assert "huge" in str(exc), str(exc)


def test_rejection_sram_budget() -> None:
    s = xp.XmSample(name="s1", data=bytes(1000), length=1000, loop_start=0, loop_end=0,
                     loop_type=0, volume=64, finetune=0, panning=128, relative_note=0)
    song = _make_minimal_song([s])
    try:
        blob_writer.build_blob(song, sram_budget_bytes=100)
        raise AssertionError("expected ConversionError for over-SRAM-budget module")
    except blob_writer.ConversionError as exc:
        assert "budget" in str(exc), str(exc)
        assert "s1" in str(exc), str(exc)


# --- Corpus-dependent checks -------------------------------------------

def corpus_files():
    return sorted(glob.glob(os.path.join(CORPUS_DIR, "*.xm")))


def _parse_openmpt123_info(path: str) -> dict:
    out = subprocess.run(["openmpt123", "--info", path], capture_output=True,
                          text=True, check=True).stdout
    fields = {}
    for line in out.splitlines():
        m = re.match(r"^([A-Za-z][A-Za-z ]*?)\.*:\s*(.*)$", line)
        if m:
            fields[m.group(1).strip()] = m.group(2).strip()
    return fields


def test_structure_vs_openmpt123(path: str, song: xp.XmSong) -> None:
    info = _parse_openmpt123_info(path)
    total_samples = sum(len(i.samples) for i in song.instruments)
    assert int(info["Channels"]) == song.num_channels
    assert int(info["Orders"]) == len(song.order_table)
    assert int(info["Patterns"]) == len(song.patterns)
    assert int(info["Instruments"]) == len(song.instruments)
    assert int(info["Samples"]) == total_samples


def test_structure_vs_libopenmpt(path: str, song: xp.XmSong) -> None:
    with openmpt_ref.load_file(path) as m:
        assert m.num_channels == song.num_channels
        assert m.num_orders == len(song.order_table)
        assert m.num_patterns == len(song.patterns)
        assert m.num_instruments == len(song.instruments)
        assert m.order_list == song.order_table, "order->pattern list mismatch"


def _reference_delta_decode_8(raw: bytes) -> bytes:
    """Independent second implementation of the algorithm xm_parser.py's
    delta_decode_8 computes via unsigned mod-256 addition — this one uses
    explicit signed arithmetic and an explicit wrap loop, a genuinely
    different code path to catch a wraparound bug either could share."""
    out = bytearray(len(raw))
    prev = 0
    for i, byte in enumerate(raw):
        delta = byte - 256 if byte >= 128 else byte
        prev += delta
        while prev > 127:
            prev -= 256
        while prev < -128:
            prev += 256
        out[i] = prev & 0xFF
    return bytes(out)


def _reference_delta_decode_16_to_8(raw: bytes) -> bytes:
    n = len(raw) // 2
    out = bytearray(n)
    prev = 0
    for i in range(n):
        delta = struct.unpack_from("<h", raw, i * 2)[0]
        prev += delta
        while prev > 32767:
            prev -= 65536
        while prev < -32768:
            prev += 65536
        out[i] = (prev >> 8) & 0xFF
    return bytes(out)


def test_delta_decode_roundtrip(path: str, song: xp.XmSong) -> None:
    checked = 0
    for inst in song.instruments:
        for s in inst.samples:
            if not s.raw:
                continue
            ref = (_reference_delta_decode_16_to_8(s.raw) if s.is_16bit
                   else _reference_delta_decode_8(s.raw))
            assert ref[:len(s.data)] == s.data, f"{path}: sample {s.name!r} decode mismatch"
            checked += 1
    assert checked > 0, f"{path}: no non-empty samples to check"


def test_guard_samples(path: str, song: xp.XmSong) -> None:
    for inst in song.instruments:
        for s in inst.samples:
            guard = blob_writer._guard_byte(s)
            if s.loop_type != 0 and s.data:
                assert guard == s.data[s.loop_start], f"{path}: {s.name!r} looped guard mismatch"
            else:
                expected = s.data[-1] if s.data else 0
                assert guard == expected, f"{path}: {s.name!r} one-shot guard mismatch"


def test_blob_self_consistency(path: str, song: xp.XmSong) -> None:
    blob = blob_writer.build_blob(song)
    hdr = bf.SONG_HEADER.unpack(blob)
    flat_samples = [s for inst in song.instruments for s in inst.samples]

    assert hdr["magic"] == bf.MAGIC
    assert hdr["version"] == bf.VERSION
    assert hdr["num_channels"] == song.num_channels
    assert hdr["num_orders"] == len(song.order_table)
    assert hdr["num_patterns"] == len(song.patterns)
    assert hdr["num_instruments"] == len(song.instruments)
    assert hdr["num_samples"] == len(flat_samples)
    assert hdr["total_size"] == len(blob)
    assert hdr["tracker_name"] == song.tracker_name.encode("latin-1", "replace")
    assert hdr["sample_data_bytes"] == sum(blob_writer._sample_region_size(s) for s in flat_samples)

    order_off = hdr["order_table_offset"]
    assert list(blob[order_off:order_off + hdr["num_orders"]]) == song.order_table

    pat_off = hdr["pattern_table_offset"]
    for i, pat in enumerate(song.patterns):
        row = bf.PATTERN_HEADER.unpack(blob, pat_off + i * bf.PATTERN_HEADER.size)
        assert row["num_rows"] == pat.num_rows

    samp_off = hdr["sample_table_offset"]
    for i, s in enumerate(flat_samples):
        row = bf.SAMPLE_HEADER.unpack(blob, samp_off + i * bf.SAMPLE_HEADER.size)
        assert row["length"] == s.length
        assert row["loop_start"] == s.loop_start
        assert row["loop_end"] == s.loop_end
        data_off = row["data_offset"]
        assert blob[data_off:data_off + s.length] == s.data
        assert blob[data_off + s.length] == blob_writer._guard_byte(s)


# --- Runner ---------------------------------------------------------------

def main() -> None:
    passed = 0
    failures = []

    def run(name, fn, *args):
        nonlocal passed
        try:
            fn(*args)
            print(f"PASS  {name}")
            passed += 1
        except AssertionError as exc:
            print(f"FAIL  {name}: {exc}")
            failures.append(name)
        except Exception:
            print(f"ERROR {name}")
            traceback.print_exc()
            failures.append(name)

    run("periods: frequency sanity", test_periods_sanity)
    run("effects: normalization smoke test", test_effects_smoke)
    run("blob_format: struct pack/unpack round-trip", test_struct_roundtrip)
    run("rejection: Q18.14 length cap", test_rejection_length_cap)
    run("rejection: SRAM budget", test_rejection_sram_budget)

    files = corpus_files()
    if not files:
        print(f"(corpus dir {CORPUS_DIR} has no .xm files - skipping corpus-dependent "
              "checks; populate it with any legally-obtained XM modules, e.g. from "
              "Mod Archive, to run them)")
    else:
        have_openmpt123 = shutil.which("openmpt123") is not None
        have_libopenmpt = openmpt_ref.available()
        for path in files:
            base = os.path.basename(path)
            song = xp.parse_xm_file(path)
            if have_openmpt123:
                run(f"{base}: structure vs openmpt123 --info", test_structure_vs_openmpt123, path, song)
            else:
                print(f"SKIP  {base}: openmpt123 not installed")
            if have_libopenmpt:
                run(f"{base}: structure vs libopenmpt", test_structure_vs_libopenmpt, path, song)
            else:
                print(f"SKIP  {base}: libopenmpt not available")
            run(f"{base}: delta-decode roundtrip", test_delta_decode_roundtrip, path, song)
            run(f"{base}: guard samples", test_guard_samples, path, song)
            run(f"{base}: blob self-consistency", test_blob_self_consistency, path, song)

    print()
    print(f"{passed} passed, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
