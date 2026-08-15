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
import re
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


# Every hand-built DX7Voice() in this file needs these pitch EG/LFO fields
# (no dataclass default). 50/50/50/50 for pitch_eg level matches patch.h's
# own "50 = no deviation" convention (see FmPitchEgParams' comment); the
# rest are 0 (also safe/no-effect, 0-depth LFO), same as this file's other
# "doesn't matter for what this test checks" fixture values.
_VOICE_LFO_PEG_DEFAULTS = dict(
    pitch_eg_rate=[50, 50, 50, 50], pitch_eg_level=[50, 50, 50, 50],
    lfo_speed=0, lfo_delay=0, lfo_pmd=0, lfo_amd=0,
    lfo_key_sync=0, lfo_waveform=0, lfo_pms=0,
)


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


def test_op_detune_offset_passthrough() -> None:
    # The converter does not bake detune into cents. Real DX7 detune is
    # note-dependent in ratio mode and a separate sharpen-only rule in fixed
    # mode, neither of which a note-independent converter can resolve, so the
    # raw offset goes through and op.h applies both rules at note-on.
    assert sp.op_detune_offset(_flat_op(detune=7)) == 0
    assert sp.op_detune_offset(_flat_op(detune=0)) == -7
    assert sp.op_detune_offset(_flat_op(detune=14)) == 7
    assert not hasattr(sp, "op_detune_cents")

    # Fixed-frequency operators keep their detune too.
    fixed = _flat_op(osc_mode=1, freq_coarse=2, freq_fine=0, detune=14)
    assert sp.op_detune_offset(fixed) == 7


def test_fixed_freq_converts_with_real_hz() -> None:
    # Fixed-frequency operators convert with a real Hz value and keep their
    # raw detune offset rather than having it forced to zero -- fixed mode
    # has its own sharpen-only detune rule, applied by op.h at note-on.
    voice = sp.DX7Voice(ops=[_flat_op(osc_mode=1, freq_coarse=1, freq_fine=0)] + [_flat_op() for _ in range(5)],
                         algorithm=0, feedback_level=0, name="FIXEDTEST",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    fixed_op = out.ops[5]  # bus-order j=0 (OP6) -> engine index 5-0=5
    assert fixed_op.fixed_freq is True
    assert abs(fixed_op.fixed_hz - 10.0) < 1e-6
    assert fixed_op.detune_offset == 0  # this fixture's detune byte is 7 (neutral)
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
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
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


def test_lfo_and_pitch_eg_pass_through() -> None:
    # #49: voice-wide LFO/pitch EG bytes are copied straight through,
    # unmodified, same "no host-side DSP math" reasoning as every other
    # field -- fm/lfo.h and fm/pitch_eg.h do the real conversion at note-on/
    # block-rate on the device.
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(
        ops=ops, algorithm=0, feedback_level=0, name="LFOTEST",
        pitch_eg_rate=[11, 22, 33, 44], pitch_eg_level=[55, 66, 77, 88],
        lfo_speed=37, lfo_delay=12, lfo_pmd=50, lfo_amd=25,
        lfo_key_sync=1, lfo_waveform=4, lfo_pms=6,
        osc_key_sync=0, transpose=24,
    )
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    assert out.pitch_eg.rate == [11, 22, 33, 44]
    assert out.pitch_eg.level == [55, 66, 77, 88]
    assert out.lfo.rate == 37
    assert out.lfo.delay == 12
    assert out.lfo.pmd == 50
    assert out.lfo.amd == 25
    assert out.lfo.key_sync is True
    assert out.lfo.waveform == 4
    assert out.lfo.pms == 6


def test_carrier_l4_is_preserved() -> None:
    """A carrier's nonzero L4 survives conversion (history_fm.md §5.16).

    This test used to assert the opposite -- that the converter forced it to 0
    so the voice could always reach EG_IDLE. voice_alloc.cpp reclaims a
    released-but-sounding voice at priority 2, so that was never necessary, and
    it silently deleted the design of every patch that sustains past key-off.
    Inverted rather than deleted, so the old behaviour cannot quietly return.
    """
    # Algorithm 1 (index 0): bus-order op3 (OP3) and op5 (OP1) are carriers.
    ops = [_flat_op() for _ in range(6)]
    ops[3] = _flat_op(eg_level=[99, 70, 60, 40])  # OP3, carrier, nonzero L4
    voice = sp.DX7Voice(ops=ops, algorithm=0, feedback_level=0, name="SUSTAINPAD",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    carrier_op = out.ops[2]  # engine index 5-3=2 -> OP3
    assert carrier_op.mod_target == sp.FM_TARGET_OUT
    assert carrier_op.eg_level[3] == 40
    assert not any("forced to 0" in w for w in warnings)


def test_feedback_level_zero_disables_feedback() -> None:
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=0, feedback_level=0, name="NOFEEDBACK",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    # bus-order op0 (OP6) is algorithm 1's primary feedback op -> engine index 5.
    assert out.ops[5].feedback_level == 0


def test_feedback_level_nonzero_passes_through() -> None:
    # The real 0-7 depth must survive convert_voice() unchanged (not
    # collapsed to a bool) -- op.h's op_render_fb relies on the exact value
    # to reproduce DX7's real per-level feedback depth range.
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=0, feedback_level=5, name="FEEDBACK5",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    assert out.ops[5].feedback_level == 5
    # Every non-feedback-capable operator stays at 0 regardless of the
    # voice's feedback_level -- only the algorithm's primary op carries it.
    assert all(out.ops[i].feedback_level == 0 for i in range(5))


def test_every_op_field_is_emitted() -> None:
    """Every FmOpOut field must reach the generated C++ initialiser.

    A C++ aggregate initialiser with one too few members just zero-fills the
    rest -- no warning, no error, and the only symptom is a silently dead
    field (e.g. amp_mod_sens, parsed but never emitted, made LFO amplitude
    modulation silently dead in every converted patch). Counting emitted
    values against the dataclass is what makes the next dropped field loud
    instead of silent.
    """
    import dataclasses

    voice = sp.DX7Voice(ops=[_flat_op(amp_mod_sens=3) for _ in range(6)],
                        algorithm=0, feedback_level=0, name="AMSTEST",
                        **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    out = sp.convert_voice(0, voice, [])
    assert out is not None
    assert out.ops[0].am_sensitivity == 3, "amp_mod_sens dropped between parse and output"

    text = sp.render_header([out], "synthetic")

    # The first operator initialiser line inside FM_PATCHES.
    op_line = next(l.strip() for l in text.splitlines()
                   if l.strip().startswith("{ ") and "f, " in l and l.rstrip().endswith("},"))
    body = op_line.strip().rstrip(",").strip()[1:-1]      # drop the outer braces
    # Collapse each { ... } group (eg_rate, eg_level) to a single token so the
    # split below counts struct members, not array elements.
    flat = re.sub(r"\{[^}]*\}", "GROUP", body)
    emitted = len([v for v in flat.split(",") if v.strip()])
    expected = len(dataclasses.fields(sp.FmOpOut))
    assert emitted == expected, (
        f"{emitted} values emitted per operator but FmOpOut has {expected} fields -- "
        f"a field was added without updating the emitter")


def test_converter_emits_no_gain_constant() -> None:
    # The converter has no opinion about absolute level (history_fm.md §2):
    # op.h owns the single engine-wide ceiling and this side is pure
    # translation. Asserted structurally so a reintroduced level field, or a
    # reintroduced module constant, fails here rather than quietly at the far
    # end of a listening test.
    assert not hasattr(sp.FmOpOut, "level")
    assert "level" not in getattr(sp.FmOpOut, "__annotations__", {})
    assert not hasattr(sp, "FM_CARRIER_LEVEL_REF")
    assert not hasattr(sp, "FM_MODULATOR_LEVEL_REF")


def test_multi_carrier_output_level_passes_through() -> None:
    # Algorithm 32 (index 31): all six operators are carriers. There is no
    # 1/carrier_count attenuation -- real DX7 hardware does not apply it, and
    # applying it would make every multi-carrier algorithm quieter than its
    # patch asked for. op.h's FM_CYCLE headroom is what makes the sum safe.
    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=31, feedback_level=0, name="ALLCARRIERS",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    for engine_idx, op in enumerate(out.ops):
        assert op.mod_target == sp.FM_TARGET_OUT
        # Straight through from the DX7 byte, untouched by carrier count.
        assert op.output_level == ops[5 - engine_idx].output_level


def test_multi_modulator_output_level_passes_through() -> None:
    # Algorithm 12 (index 11): OP6/OP5/OP4 all target OP3 (3-way modulator
    # fan-in), with no 1/fan_in attenuation applied. The routing itself is
    # what this still needs to get right.
    decode = sp.decode_algorithm(sp.DX7_ALGORITHMS[11])
    fan_in_target = decode.routing[0].target  # OP6's target -- the shared bus
    assert decode.routing[1].target == fan_in_target  # OP5
    assert decode.routing[2].target == fan_in_target  # OP4

    ops = [_flat_op() for _ in range(6)]
    voice = sp.DX7Voice(ops=ops, algorithm=11, feedback_level=0, name="THREEMOD",
                         **_VOICE_LFO_PEG_DEFAULTS, osc_key_sync=0, transpose=24)
    warnings: list = []
    out = sp.convert_voice(0, voice, warnings)
    assert out is not None
    engine_target = sp.FM_TARGET_OUT if fan_in_target == "OUT" else (5 - fan_in_target)
    fed = [op for op in out.ops if op.mod_target == engine_target]
    assert len(fed) == 3
    for op in fed:
        assert op.output_level == _flat_op().output_level


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
    buf[102:110] = bytes([1, 2, 3, 4, 5, 6, 7, 8])  # pitch EG rate1-4, level1-4 (#49)
    buf[110] = algorithm & 0x1F
    buf[111] = feedback_level & 7  # osc_key_sync=0
    buf[112] = 10  # lfo_speed
    buf[113] = 20  # lfo_delay
    buf[114] = 30  # lfo_pmd
    buf[115] = 40  # lfo_amd
    buf[116] = 1 | (3 << 1) | (5 << 4)  # #49: LKS=1, LFW=3 (square), LPMS=5 -> 0x57
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
    # #49: voice-wide pitch EG (bulk 102-109) + LFO (bulk 112-116).
    assert voice.pitch_eg_rate == [1, 2, 3, 4]
    assert voice.pitch_eg_level == [5, 6, 7, 8]
    assert voice.lfo_speed == 10
    assert voice.lfo_delay == 20
    assert voice.lfo_pmd == 30
    assert voice.lfo_amd == 40
    assert voice.lfo_key_sync == 1
    assert voice.lfo_waveform == 3
    assert voice.lfo_pms == 5
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


def _voice_with_corrupt_byte(index: int, value: int) -> bytes:
    op = _pack_op([9, 19, 29, 39], [98, 68, 58, 8], 27, 11, 12, 1, 2, 3, 4, 6, 2, 77, 0, 15, 33)
    corrupt = bytearray(op)
    corrupt[index] = value
    buf = bytearray(128)
    buf[0:17] = bytes(corrupt)
    for j in range(1, 6):
        buf[j * 17:(j + 1) * 17] = op
    buf[110] = 0
    buf[118:128] = b"BAD       "[:10]
    return bytes(buf)


def test_out_of_range_byte_fails_loud() -> None:
    """Fail-loud still holds for fields the reference does not define past 99.

    Probes break_point rather than an EG field, since EG rate/level are
    widened to the full 7-bit byte range (real factory patches use values
    above 99) -- the point of the test is that the fail-loud policy exists,
    not which byte carries it.
    """
    try:
        sp.unpack_voice(_voice_with_corrupt_byte(8, 0x7F), 0)   # break_point = 127
        assert False, "expected a break_point range failure"
    except sp.Syx2PatchError as exc:
        assert "break_point" in str(exc)


def test_eg_bytes_above_panel_range_pass_through() -> None:
    """100-127 in an EG field is kept, not clamped to 99.

    Clamping darkens the patch audibly (measured against Dexed on ROM3A #19
    TIMPANI) -- the byte is real data, not corruption, so passing it through
    unmodified is the correct behaviour.
    """
    warnings: list = []
    voice = sp.unpack_voice(_voice_with_corrupt_byte(4, 127), 0, warnings)   # eg_level1 = 127
    assert voice.ops[0].eg_level[0] == 127
    assert any("above the DX7 panel range" in w for w in warnings)


# --- Corpus-dependent checks (real .syx files, gitignored) ------------------

def test_corpus_bank(path: str) -> None:
    patches, warnings, skipped = sp._convert_all([path])
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
            # 0-127, not 0-99: eg_rate/eg_level are widened to the full 7-bit
            # byte range (real factory patches, e.g. ROM3A TIMPANI, ship EG
            # bytes above 99 -- see _range_check_byte()'s comment and
            # test_eg_bytes_above_panel_range_pass_through() above).
            assert all(0 <= v <= 127 for v in op.eg_rate)
            assert all(0 <= v <= 127 for v in op.eg_level)
        assert all(0 <= v <= 99 for v in p.pitch_eg.rate)
        assert all(0 <= v <= 99 for v in p.pitch_eg.level)
        assert 0 <= p.lfo.rate <= 99 and 0 <= p.lfo.delay <= 99
        assert 0 <= p.lfo.pmd <= 99 and 0 <= p.lfo.amd <= 99
        assert 0 <= p.lfo.waveform <= 5
        assert 0 <= p.lfo.pms <= 7


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
    run("op_detune_offset() raw passthrough (F5)", test_op_detune_offset_passthrough)
    run("fixed-frequency mode converts with real Hz", test_fixed_freq_converts_with_real_hz)
    run("key level/rate scaling pass through", test_key_level_and_rate_scaling_pass_through)
    run("LFO/pitch EG pass through", test_lfo_and_pitch_eg_pass_through)
    run("carrier L4 preserved (F6: allocator priority 2 makes zeroing unnecessary)", test_carrier_l4_is_preserved)
    run("feedback_level=0 disables feedback exactly", test_feedback_level_zero_disables_feedback)
    run("feedback_level nonzero passes through exactly", test_feedback_level_nonzero_passes_through)
    run("every FmOpOut field reaches the output (F5)", test_every_op_field_is_emitted)
    run("converter emits no gain/level constant (F2)", test_converter_emits_no_gain_constant)
    run("multi-carrier output_level passes through unattenuated", test_multi_carrier_output_level_passes_through)
    run("multi-modulator output_level passes through unattenuated", test_multi_modulator_output_level_passes_through)
    run("unpack_voice: bit-packing round-trip", test_unpack_voice_roundtrip)
    run("parse_syx_bulk: checksum validation", test_bulk_parse_and_checksum)
    run("parse_syx_bulk: header/length rejection", test_bulk_parse_rejects_bad_header)
    run("unpack_voice: out-of-range byte fails loud", test_out_of_range_byte_fails_loud)
    run("unpack_voice: EG byte above panel range passes through (F7)", test_eg_bytes_above_panel_range_pass_through)

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
