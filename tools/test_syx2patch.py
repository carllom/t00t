#!/usr/bin/env python3
"""Test suite for syx2patch.py (#47). Run: python3 tools/test_syx2patch.py

Corpus-dependent checks look for `.syx` files in `../syx/` (gitignored --
not committed, since a real DX7 bank is Yamaha's own commercial patch data,
same reasoning as xm2t00t's xm/ directory). Populate it yourself (e.g. from
https://yamahablackboxes.com/collection/yamaha-dx7-synthesizer/patches/) to
run those checks; if it's empty they're skipped with a clear message rather
than failing, so the synthetic-fixture and unit checks still run to
completion anywhere.
"""

from __future__ import annotations

import glob
import os
import struct
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import syx2patch as sp

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_DIR = os.path.join(HERE, "..", "syx")


def corpus_files():
    return sorted(glob.glob(os.path.join(CORPUS_DIR, "*.syx")))


# --- Unit checks (no corpus needed) ----------------------------------------

def test_all_algorithms_decode() -> None:
    assert len(sp.DX7_ALGORITHMS) == 32
    for i, flags in enumerate(sp.DX7_ALGORITHMS):
        decode = sp.decode_algorithm(flags)
        assert len(decode.routing) == 6
        for r in decode.routing:
            assert r.target == "OUT" or (0 <= r.target <= 5), (i, r)
        # Every algorithm has exactly one primary feedback operator, and
        # decode_algorithm() itself raises if that's ever not true -- this
        # just re-asserts it wasn't silently swallowed.
        assert decode.routing[decode.primary_fb_op].feedback_capable

    # Only algorithms 4 and 6 (human-numbered, list index 3 and 5) have a
    # second feedback-marked operator forming a real cycle once tested --
    # verified by hand against Dexed's own bus-flag semantics (#47's
    # implementation notes). Every other algorithm must NOT flag.
    flagged = [i + 1 for i, f in enumerate(sp.DX7_ALGORITHMS) if sp.decode_algorithm(f).needs_interleaved]
    assert flagged == [4, 6], flagged


def test_algorithm_1_topology() -> None:
    # The classic "two independent chains" algorithm: OP6(fb)->OP5->OP4->OP3
    # ->OUT, OP2->OP1->OUT. Bus-order index 0=OP6 .. 5=OP1.
    decode = sp.decode_algorithm(sp.DX7_ALGORITHMS[0])
    r = decode.routing
    assert r[0].target == 1 and r[0].feedback_capable  # OP6 -> OP5, feedback
    assert r[1].target == 2                             # OP5 -> OP4
    assert r[2].target == 3                             # OP4 -> OP3
    assert r[3].target == "OUT"                          # OP3 -> OUT
    assert r[4].target == 5                              # OP2 -> OP1
    assert r[5].target == "OUT"                          # OP1 -> OUT
    assert not decode.needs_interleaved


def test_algorithm_32_all_carriers() -> None:
    decode = sp.decode_algorithm(sp.DX7_ALGORITHMS[31])
    assert all(r.target == "OUT" for r in decode.routing)
    assert decode.routing[0].feedback_capable  # OP6
    assert not decode.needs_interleaved


def test_algorithm_4_and_6_interleaved() -> None:
    d4 = sp.decode_algorithm(sp.DX7_ALGORITHMS[3])
    assert d4.needs_interleaved
    assert d4.secondary_fb_ops == [2]  # OP4 (bus-order index 2)
    # The emitted routing itself never includes the secondary edge -- the
    # fallback is "don't add it", not a separate mutation step.
    assert d4.routing[2].target == "OUT" and not d4.routing[2].feedback_capable

    d6 = sp.decode_algorithm(sp.DX7_ALGORITHMS[5])
    assert d6.needs_interleaved
    assert d6.secondary_fb_ops == [1]  # OP5 (bus-order index 1)


def test_coarse_ratio() -> None:
    assert sp.coarse_ratio(0) == 0.5
    assert sp.coarse_ratio(1) == 1.0
    assert sp.coarse_ratio(2) == 2.0
    assert sp.coarse_ratio(31) == 31.0

    op = sp.DX7Op(eg_rate=[0]*4, eg_level=[0]*4, output_level=99, key_vel_sens=0,
                  osc_mode=0, freq_coarse=1, freq_fine=0, break_point=0,
                  scale_left_depth=0, scale_right_depth=0, scale_left_curve=0,
                  scale_right_curve=0, rate_scale=0, detune=7, amp_mod_sens=0)
    assert abs(sp.op_ratio(op) - 1.0) < 1e-9
    op.freq_coarse, op.freq_fine = 2, 50
    assert abs(sp.op_ratio(op) - 3.0) < 1e-9  # 2 * 1.5
    op.freq_coarse, op.freq_fine = 0, 0
    assert abs(sp.op_ratio(op) - 0.5) < 1e-9


def _flat_op(**overrides) -> "sp.DX7Op":
    base = dict(eg_rate=[99, 50, 30, 40], eg_level=[99, 70, 60, 0], output_level=99,
                key_vel_sens=0, osc_mode=0, freq_coarse=1, freq_fine=0, break_point=0,
                scale_left_depth=0, scale_right_depth=0, scale_left_curve=0,
                scale_right_curve=0, rate_scale=0, detune=7, amp_mod_sens=0)
    base.update(overrides)
    return sp.DX7Op(**base)


def test_op_fixed_hz_formula() -> None:
    # Verified against Dexed's osc_freq() mode!=0 branch (#48): Hz =
    # 10^(((coarse&3)*100+fine)/100).
    op = _flat_op(freq_coarse=0, freq_fine=0)
    assert abs(sp.op_fixed_hz(op) - 1.0) < 1e-6
    op = _flat_op(freq_coarse=1, freq_fine=0)
    assert abs(sp.op_fixed_hz(op) - 10.0) < 1e-6
    op = _flat_op(freq_coarse=2, freq_fine=0)
    assert abs(sp.op_fixed_hz(op) - 100.0) < 1e-6
    op = _flat_op(freq_coarse=3, freq_fine=99)
    assert abs(sp.op_fixed_hz(op) - 10.0 ** 3.99) < 1e-3


def test_op_detune_cents_formula() -> None:
    assert sp.op_detune_cents(_flat_op(detune=7)) == 0.0
    assert sp.op_detune_cents(_flat_op(detune=0)) == -7.0
    assert sp.op_detune_cents(_flat_op(detune=14)) == 7.0


def test_fixed_freq_converts_with_real_hz() -> None:
    # #48: fixed-frequency operators used to be skipped outright (v1); now
    # they convert with a real Hz value and detune_cents forced to 0 (no
    # ratio-mode detune formula applies to a fixed-frequency operator).
    voice = sp.DX7Voice(ops=[_flat_op(osc_mode=1, freq_coarse=1, freq_fine=0)] + [_flat_op() for _ in range(5)],
                         algorithm=0, feedback_level=0, name="FIXEDTEST",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    fixed_op = out.ops[5]  # bus-order j=0 (OP6) -> engine index 5-0=5
    assert fixed_op.fixed_freq is True
    assert abs(fixed_op.fixed_hz - 10.0) < 1e-6
    assert fixed_op.detune_cents == 0.0
    assert not any("fixed-frequency" in w for w in warnings)


def test_key_level_and_rate_scaling_pass_through() -> None:
    op = _flat_op()
    op.break_point = 39
    op.scale_left_depth = 50
    op.scale_right_depth = 60
    op.scale_left_curve = 1
    op.scale_right_curve = 2
    op.rate_scale = 5
    voice = sp.DX7Voice(ops=[op] + [_flat_op() for _ in range(5)],
                         algorithm=0, feedback_level=0, name="SCALETEST",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    scaled_op = out.ops[5]
    assert scaled_op.scale_breakpoint == 39
    assert scaled_op.scale_left_depth == 50
    assert scaled_op.scale_right_depth == 60
    assert scaled_op.scale_left_curve == 1
    assert scaled_op.scale_right_curve == 2
    assert scaled_op.rate_scaling == 5


def test_carrier_l4_forced_to_zero() -> None:
    # Algorithm 1 (index 0): bus-order op3 (OP3) and op5 (OP1) are carriers.
    ops = [_flat_op() for _ in range(6)]
    ops[3] = _flat_op(eg_level=[99, 70, 60, 40])  # OP3, carrier, nonzero L4
    voice = sp.DX7Voice(ops=ops, algorithm=0, feedback_level=0, name="SUSTAINPAD",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    carrier_op = out.ops[2]  # engine index 5-3=2 -> OP3
    assert carrier_op.mod_target == sp.FM_TARGET_OUT
    assert carrier_op.eg_level[3] == 0
    assert any("forced to 0" in w for w in warnings)


def test_feedback_level_zero_disables_feedback() -> None:
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=0, feedback_level=0, name="NOFEEDBACK",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    # bus-order op0 (OP6) is algorithm 1's primary feedback op -> engine index 5.
    assert out.ops[5].feedback is False


def test_multi_carrier_level_scaled_down() -> None:
    # Algorithm 32 (index 31): all six operators are carriers.
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=31, feedback_level=0, name="ALLCARRIERS",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    for op in out.ops:
        assert op.mod_target == sp.FM_TARGET_OUT
        assert op.level == sp.FM_CARRIER_LEVEL_REF // 6


def test_multi_modulator_level_scaled_down() -> None:
    # Algorithm 12 (index 11): OP6/OP5/OP4 all target OP3 (3-way modulator
    # fan-in) -- #57 raised the modulator headroom ceiling ~64x, which makes
    # this the same overflow risk multi-carrier summing already was.
    decode = sp.decode_algorithm(sp.DX7_ALGORITHMS[11])
    fan_in_target = decode.routing[0].target  # OP6's target -- the shared bus
    assert decode.routing[1].target == fan_in_target  # OP5
    assert decode.routing[2].target == fan_in_target  # OP4

    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=11, feedback_level=0, name="THREEMOD",
                         osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    engine_target = sp.FM_TARGET_OUT if fan_in_target == "OUT" else (5 - fan_in_target)
    fed = [op for op in out.ops if op.mod_target == engine_target]
    assert len(fed) == 3
    for op in fed:
        assert op.level == sp.FM_MODULATOR_LEVEL_REF // 3

    # Worst-case overflow sanity: N modulators each holding 1/N of the
    # reference must still sum back to (approximately) one modulator's own
    # ceiling, comfortably under int32 range, not N times it.
    assert sum(op.level for op in fed) <= sp.FM_MODULATOR_LEVEL_REF


# --- Bit-packing / sysex parsing (synthetic fixtures) -----------------------

def _pack_op(eg_rate, eg_level, bp, ld, rd, lc, rc, rs, det, kvs, ams, ol, mode, fcoarse, ffine) -> bytes:
    b = bytearray(17)
    b[0:4] = bytes(eg_rate)
    b[4:8] = bytes(eg_level)
    b[8] = bp
    b[9] = ld
    b[10] = rd
    b[11] = (lc & 3) | ((rc & 3) << 2)
    b[12] = (rs & 7) | ((det & 0xF) << 3)
    b[13] = (ams & 3) | ((kvs & 7) << 2)
    b[14] = ol
    b[15] = (mode & 1) | ((fcoarse & 0x1F) << 1)
    b[16] = ffine
    return bytes(b)


def _pack_voice(name: str, algorithm: int, feedback_level: int) -> bytes:
    buf = bytearray(128)
    op = _pack_op([9, 19, 29, 39], [98, 68, 58, 8], 27, 11, 12, 1, 2, 3, 4, 6, 2, 77, 0, 15, 33)
    for j in range(6):
        buf[j * 17:(j + 1) * 17] = op
    buf[102:110] = bytes([1, 2, 3, 4, 5, 6, 7, 8])  # pitch EG, unread
    buf[110] = algorithm & 0x1F
    buf[111] = feedback_level & 7  # osc_key_sync=0
    buf[112:117] = bytes([10, 20, 30, 40])[:0] + bytes([10, 20, 30, 40, 0])  # lfo fields, unread
    buf[117] = 24  # transpose
    name_bytes = (name + " " * 10)[:10].encode("ascii")
    buf[118:128] = name_bytes
    return bytes(buf)


def _wrap_bulk(voices: list) -> bytes:
    assert len(voices) == 32
    payload = b"".join(voices)
    assert len(payload) == 4096
    checksum = (-sum(payload)) & 0x7F
    return b"\xF0\x43\x00\x09\x20\x00" + payload + bytes([checksum]) + b"\xF7"


def test_unpack_voice_roundtrip() -> None:
    raw = _pack_voice("ROUNDTRIP", algorithm=17, feedback_level=5)
    voice = sp.unpack_voice(raw, 0)
    assert voice.algorithm == 17
    assert voice.feedback_level == 5
    assert voice.name == "ROUNDTRIP"
    for op in voice.ops:
        assert op.eg_rate == [9, 19, 29, 39]
        assert op.eg_level == [98, 68, 58, 8]
        assert op.break_point == 27
        assert op.scale_left_depth == 11
        assert op.scale_right_depth == 12
        assert op.scale_left_curve == 1
        assert op.scale_right_curve == 2
        assert op.rate_scale == 3
        assert op.detune == 4
        assert op.amp_mod_sens == 2
        assert op.key_vel_sens == 6
        assert op.output_level == 77
        assert op.osc_mode == 0
        assert op.freq_coarse == 15
        assert op.freq_fine == 33


def test_bulk_parse_and_checksum() -> None:
    voices = [_pack_voice(f"V{i:02d}", algorithm=i % 32, feedback_level=i % 8) for i in range(32)]
    data = _wrap_bulk(voices)
    chunks = sp.parse_syx_bulk(data, "<test>")
    assert len(chunks) == 32
    assert all(len(c) == 128 for c in chunks)

    corrupted = bytearray(data)
    corrupted[6] ^= 0xFF  # flip a payload byte -> checksum mismatch
    try:
        sp.parse_syx_bulk(bytes(corrupted), "<test>")
        assert False, "expected a checksum failure"
    except sp.Syx2PatchError as exc:
        assert "checksum" in str(exc)


def test_bulk_parse_rejects_bad_header() -> None:
    voices = [_pack_voice(f"V{i:02d}", algorithm=0, feedback_level=0) for i in range(32)]
    good = bytearray(_wrap_bulk(voices))

    for offset, bad_value, needle in [(0, 0x00, "0xf0"), (1, 0x00, "yamaha"),
                                       (3, 0x00, "format"), (-1, 0x00, "0xf7")]:
        corrupted = bytearray(good)
        corrupted[offset] = bad_value
        try:
            sp.parse_syx_bulk(bytes(corrupted), "<test>")
            assert False, f"expected a header failure at offset {offset}"
        except sp.Syx2PatchError as exc:
            assert needle in str(exc).lower(), (offset, str(exc))

    try:
        sp.parse_syx_bulk(bytes(good) + b"\x00", "<test>")
        assert False, "expected a length failure"
    except sp.Syx2PatchError as exc:
        assert "byte" in str(exc).lower()


def test_out_of_range_byte_fails_loud() -> None:
    op = _pack_op([9, 19, 29, 39], [98, 68, 58, 8], 27, 11, 12, 1, 2, 3, 4, 6, 2, 77, 0, 15, 33)
    corrupt = bytearray(op)
    corrupt[0] = 0x7F  # 127 -- masked to 0x7F, still 127, out of 0-99 range
    buf = bytearray(128)
    buf[0:17] = bytes(corrupt)
    for j in range(1, 6):
        buf[j * 17:(j + 1) * 17] = op
    buf[110] = 0
    buf[118:128] = b"BAD       "[:10]
    try:
        sp.unpack_voice(bytes(buf), 0)
        assert False, "expected an eg_rate range failure"
    except sp.Syx2PatchError as exc:
        assert "eg_rate1" in str(exc)


# --- Corpus-dependent checks (real .syx files, gitignored) ------------------

def test_corpus_bank(path: str) -> None:
    patches, warnings, skipped = sp._convert_all(path)
    assert len(patches) > 0
    assert len(patches) + skipped == 32
    # Every emitted patch must independently pass the exact same DAG
    # validation the device runs at note-on -- reproduced here structurally
    # (not by importing C++), since every routing.mod_target this converter
    # emits must land in [0, FM_NUM_OPS) or == FM_TARGET_OUT, never equal to
    # its own operator index (fm_resolve_routing()'s self-loop rejection).
    for p in patches:
        assert len(p.ops) == 6
        for i, op in enumerate(p.ops):
            assert op.mod_target == sp.FM_TARGET_OUT or (0 <= op.mod_target < 6)
            assert op.mod_target != i, (p.name, i)
            assert 0 <= op.output_level <= 99
            assert 0 <= op.vel_sensitivity <= 7
            assert all(0 <= v <= 99 for v in op.eg_rate)
            assert all(0 <= v <= 99 for v in op.eg_level)


def corpus_paths():
    return corpus_files()


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

    run("algorithm decode: all 32 resolve, only 4/6 flag interleaved", test_all_algorithms_decode)
    run("algorithm decode: algorithm 1 topology", test_algorithm_1_topology)
    run("algorithm decode: algorithm 32 all-carriers", test_algorithm_32_all_carriers)
    run("algorithm decode: algorithms 4/6 interleaved fallback", test_algorithm_4_and_6_interleaved)
    run("coarse/fine ratio formula", test_coarse_ratio)
    run("op_fixed_hz() formula", test_op_fixed_hz_formula)
    run("op_detune_cents() formula", test_op_detune_cents_formula)
    run("fixed-frequency mode converts with real Hz", test_fixed_freq_converts_with_real_hz)
    run("key level/rate scaling pass through", test_key_level_and_rate_scaling_pass_through)
    run("carrier L4 forced to 0 for voice-lifetime correctness", test_carrier_l4_forced_to_zero)
    run("feedback_level=0 disables feedback exactly", test_feedback_level_zero_disables_feedback)
    run("multi-carrier level scaled down by carrier count", test_multi_carrier_level_scaled_down)
    run("multi-modulator level scaled down by fan-in count", test_multi_modulator_level_scaled_down)
    run("unpack_voice: bit-packing round-trip", test_unpack_voice_roundtrip)
    run("parse_syx_bulk: checksum validation", test_bulk_parse_and_checksum)
    run("parse_syx_bulk: header/length rejection", test_bulk_parse_rejects_bad_header)
    run("unpack_voice: out-of-range byte fails loud", test_out_of_range_byte_fails_loud)

    files = corpus_paths()
    if not files:
        print(f"(corpus dir {CORPUS_DIR} has no .syx files - skipping corpus-dependent "
              "checks; populate it with a legally-obtained DX7 bank, e.g. "
              "https://yamahablackboxes.com/collection/yamaha-dx7-synthesizer/patches/, "
              "to run them)")
    else:
        for path in files:
            run(f"{os.path.basename(path)}: full bank conversion", test_corpus_bank, path)

    print()
    print(f"{passed} passed, {len(failures)} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
