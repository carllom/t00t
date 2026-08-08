#!/usr/bin/env python3
"""syx2patch.py -- DX7 32-voice .syx bulk dump -> patches.h (#47, fm.md P3 v1).

    syx2patch.py convert <in.syx> <out.h>
        Parse a 4104-byte DX7 32-voice bulk dump (F0 43 0n 09 20 00 + 4096
        packed bytes + checksum + F7), unpack all 128 packed bytes/voice,
        resolve each voice's algorithm into patch.h's mod_target/feedback
        routing shape, and write <out.h>: a device+host header
        (`enum FmPatchId`, `FM_PATCH_COUNT`, `const FmPatch FM_PATCHES[]`,
        `const char *FM_PATCH_NAMES[]`). Never wired into CMakeLists.txt or
        committed -- see the file's own generated-header comment and
        engine.md's xm2t00t precedent (copyrighted third-party patch/song
        data stays local, gitignored, user-populated).

    syx2patch.py dump <in.syx>
        Parse and print the per-voice summary (algorithm, name, any
        warnings) without writing a header. Same parse/convert path as
        `convert`, useful for sanity-checking a dump before committing to
        an output path.

Same "relocate all awkward, nonlinear, one-time work to the host" principle
as tools/xm2t00t and tools/speechgen.py: the device ships six op's worth of
plain data per patch, and the log-domain EG/level-table conversion already
lives at runtime (env_dx.h, #45) -- so unlike xm2t00t's period tables or
speechgen's formant math, THIS converter does not need to precompute any
DSP curve itself. `eg_rate`/`eg_level`/`output_level`/`vel_sensitivity` are
copied straight through as the same raw 0-99 (or 0-7) bytes DX7 hardware
uses; env_dx.h's DX7_RATE_TO_STEP/DX7_LEVEL_TO_LOG2 tables convert them at
note-on/block-rate, exactly as they already do for patch.h's hand-authored
FM_TEST_PATCH. This converter's real job is almost entirely structural:
unpack the bit-packed sysex format correctly, and turn one of 32 DX7
algorithm numbers into this engine's mod_target-per-operator shape.

v1 scope (see fm.md P3 / issue #47's acceptance criteria): algorithm
routing, EG rates/levels, output level, velocity sensitivity, coarse/fine
ratio, feedback on/off, and interleaved-feedback (algorithm 4/6) detection.
Deliberately NOT wired up, though parsed and validated: key level scaling,
rate scaling, detune, fixed-frequency mode, pitch EG, LFO, transpose --
these land in #48 (v2, calibrated against Dexed), the same v1/v2 split
speechgen.py used (#32 / #35). A patch using fixed-frequency mode on any
operator is skipped outright (not approximated) since v1 has no way to
represent it without silently changing the patch's character; a nonzero
detune is silently ignored (0 assumed) since it's a minor, continuously-
graded fidelity loss, not a change to the patch's fundamental character.

`level` (FmOpParams' reference-gain ceiling) is NOT DX7 data -- patch.h's
own comment on FmOpParams.level and FM_TEST_PATCH's hand-tuned constants
already established this: it's an artifact of this engine's fixed-point
kernel (unity ~= 1<<22, but a real hard ceiling of ~0.03-0.05 rad max
modulation depth even at gain=INT32_MAX, fm.md §3.4/§5.2/patch.h's own
extensive comment). Every carrier in every converted patch gets the same
flat FM_CARRIER_LEVEL_REF; every modulator gets the same flat
FM_MODULATOR_LEVEL_REF (both taken directly from FM_TEST_PATCH's own
already-hardware-verified precedent, its carrier op5 and its most-driven
modulator op4). The REAL per-patch, per-operator balance a DX7 patch
actually encodes is carried entirely by `output_level`/the EG, exactly as
it would be on real hardware -- `level` only sets what 100% means.
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional

FM_NUM_OPS = 6
FM_TARGET_OUT = FM_NUM_OPS  # 6 -- patch.h's carrier/OUT sentinel

# FM_TEST_PATCH's own hand-tuned, hardware-verified reference constants
# (src/engines/fm/patch.h): op5 (carrier) = 1<<21, op4 (its most-driven
# modulator) = 1400000000. Reused here as the flat per-role ceiling for
# every converted patch -- see this file's header comment for why a flat
# constant is the right choice, not a shortcut.
FM_CARRIER_LEVEL_REF = 1 << 21
FM_MODULATOR_LEVEL_REF = 1400000000

# DX7 algorithm topology (Dexed, Source/msfa/fm_core.cc, Apache License 2.0,
# Copyright 2012 Google Inc. -- https://github.com/asb2m10/dexed). One row
# per algorithm (index 0 = "Algorithm 1" in every DX7 manual/Dexed's own
# 1-based UI numbering; voice.algorithm, the raw sysex byte, is 0-31 and
# indexes this table directly). Each row is 6 bus-flag bytes, one per
# operator, in DX7's fixed hardware processing order: index 0 = OP6
# (processed first) .. index 5 = OP1 (processed last) -- the same order
# unpack_voice() below unpacks the packed sysex format in, so no
# re-indexing is needed between the two tables; only decode_algorithm()'s
# *output* gets remapped to this engine's own (arbitrary) 0-5 operator
# indexing, once, at the point patches are emitted.
#
# Byte layout (bit7 FB_OUT, bit6 FB_IN, bits5-4 in-bus 0/1/2, bit2 out-bus-
# add, bits1-0 out-bus 0/1/2 -- 0 always means "the main output", 1/2 are
# Dexed's two reusable scratch buses): decode_algorithm() reconstructs the
# real per-operator target purely from these bytes, generically, rather
# than hand-transcribing 32 diagrams -- see its own comment for the method
# and for how algorithms 4/6's second feedback loop is detected.
DX7_ALGORITHMS: List[List[int]] = [
    [0xc1, 0x11, 0x11, 0x14, 0x01, 0x14],  # 1
    [0x01, 0x11, 0x11, 0x14, 0xc1, 0x14],  # 2
    [0xc1, 0x11, 0x14, 0x01, 0x11, 0x14],  # 3
    [0xc1, 0x11, 0x94, 0x01, 0x11, 0x14],  # 4 -- second feedback loop (interleaved)
    [0xc1, 0x14, 0x01, 0x14, 0x01, 0x14],  # 5
    [0xc1, 0x94, 0x01, 0x14, 0x01, 0x14],  # 6 -- second feedback loop (interleaved)
    [0xc1, 0x11, 0x05, 0x14, 0x01, 0x14],  # 7
    [0x01, 0x11, 0xc5, 0x14, 0x01, 0x14],  # 8
    [0x01, 0x11, 0x05, 0x14, 0xc1, 0x14],  # 9
    [0x01, 0x05, 0x14, 0xc1, 0x11, 0x14],  # 10
    [0xc1, 0x05, 0x14, 0x01, 0x11, 0x14],  # 11
    [0x01, 0x05, 0x05, 0x14, 0xc1, 0x14],  # 12
    [0xc1, 0x05, 0x05, 0x14, 0x01, 0x14],  # 13
    [0xc1, 0x05, 0x11, 0x14, 0x01, 0x14],  # 14
    [0x01, 0x05, 0x11, 0x14, 0xc1, 0x14],  # 15
    [0xc1, 0x11, 0x02, 0x25, 0x05, 0x14],  # 16
    [0x01, 0x11, 0x02, 0x25, 0xc5, 0x14],  # 17
    [0x01, 0x11, 0x11, 0xc5, 0x05, 0x14],  # 18
    [0xc1, 0x14, 0x14, 0x01, 0x11, 0x14],  # 19
    [0x01, 0x05, 0x14, 0xc1, 0x14, 0x14],  # 20
    [0x01, 0x14, 0x14, 0xc1, 0x14, 0x14],  # 21
    [0xc1, 0x14, 0x14, 0x14, 0x01, 0x14],  # 22
    [0xc1, 0x14, 0x14, 0x01, 0x14, 0x04],  # 23
    [0xc1, 0x14, 0x14, 0x14, 0x04, 0x04],  # 24
    [0xc1, 0x14, 0x14, 0x04, 0x04, 0x04],  # 25
    [0xc1, 0x05, 0x14, 0x01, 0x14, 0x04],  # 26
    [0x01, 0x05, 0x14, 0xc1, 0x14, 0x04],  # 27
    [0x04, 0xc1, 0x11, 0x14, 0x01, 0x14],  # 28
    [0xc1, 0x14, 0x01, 0x14, 0x04, 0x04],  # 29
    [0x04, 0xc1, 0x11, 0x14, 0x04, 0x04],  # 30
    [0xc1, 0x14, 0x04, 0x04, 0x04, 0x04],  # 31
    [0xc4, 0x04, 0x04, 0x04, 0x04, 0x04],  # 32
]


class Syx2PatchError(Exception):
    """Raised for anything this converter cannot represent, or any sysex
    data that fails validation -- callers print str(exc) and exit non-zero
    without writing a header. Same "fail loudly, no silent approximation"
    contract as speechgen.py's PhonemeCsvError."""


# ---------------------------------------------------------------------------
# Algorithm decode: DX7_ALGORITHMS' bus-flag bytes -> per-operator routing.
# ---------------------------------------------------------------------------

@dataclass
class OpRouting:
    target: object       # 'OUT', or an int 0-5 (bus-order index, DX7 OP6..OP1)
    feedback_capable: bool  # this op is the algorithm's *primary* feedback op


@dataclass
class AlgorithmDecode:
    routing: List[OpRouting]  # length 6, bus-order (index 0 = OP6 .. 5 = OP1)
    primary_fb_op: int        # bus-order index of the primary feedback operator
    secondary_fb_ops: List[int]
    needs_interleaved: bool


def _has_cycle(adj: Dict[int, List[int]]) -> bool:
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {n: WHITE for n in adj}

    def dfs(u: int) -> bool:
        color[u] = GRAY
        for v in adj[u]:
            if color[v] == GRAY:
                return True
            if color[v] == WHITE and dfs(v):
                return True
        color[u] = BLACK
        return False

    return any(color[n] == WHITE and dfs(n) for n in adj)


def decode_algorithm(flags: List[int]) -> AlgorithmDecode:
    """Reconstructs the real per-operator modulation graph from Dexed's bus
    flags by simulating DX7's fixed OP6->OP1 processing order: a bus (1 or
    2) is a stack -- written by one contiguous run of operators (an
    "add"-chain), and the *next* operator that reads that bus number is,
    unambiguously, every writer in that run's actual downstream target
    (verified by hand against algorithms 1 and 32's well-known diagrams;
    see the module docstring / #47's implementation notes for the full
    derivation). This works uniformly for all 32 rows -- no per-algorithm
    special-casing.

    Algorithms 4 and 6 each have one extra operator whose FB_OUT bit is set
    alone (without the paired FB_IN bit the algorithm's *primary* feedback
    operator has) -- real DX7 hardware routes that operator's output into
    the SAME shared feedback register the primary operator reads, forming a
    genuine second, operator-spanning loop closed only across block/sample
    processing order (a one-render-block delay), not the ordinary forward
    DAG this decode already reconstructs. That shape needs a block-length
    delay to break -- exactly the multi-operator-cycle case fm.md §4.2 and
    patch.h's fm_resolve_routing() rule out entirely. Detected generically
    here (not by hardcoding "algorithm == 4 or 6"): build a test graph
    adding a tentative edge from every such secondary operator to the
    primary, and run a real cycle check over it. If a cycle is found,
    `needs_interleaved` is set and callers must not add that secondary edge
    to the emitted patch -- `routing` above never includes it in the first
    place (it only ever reflects the ordinary forward graph), so the
    fallback is simply "emit routing as decoded, and say so out loud."
    """
    n = 6
    outbus = [f & 3 for f in flags]
    add = [bool(f & 4) for f in flags]
    inbus = [(f >> 4) & 3 for f in flags]
    fb_out_bit = [bool(f & 0x80) for f in flags]
    fb_full = [(f & 0xc0) == 0xc0 for f in flags]

    primaries = [j for j in range(n) if fb_full[j]]
    if len(primaries) != 1:
        raise Syx2PatchError(
            f"algorithm decode: expected exactly one primary feedback operator, "
            f"found {len(primaries)} (flags={[hex(f) for f in flags]}) -- "
            f"DX7_ALGORITHMS table is malformed"
        )
    primary_op = primaries[0]
    secondary = [j for j in range(n) if fb_out_bit[j] and not fb_full[j]]

    pending: Dict[int, List[int]] = {1: [], 2: []}
    target: List[Optional[object]] = [None] * n
    for j in range(n):
        ib = inbus[j]
        if ib != 0:
            for writer in pending[ib]:
                target[writer] = j
            pending[ib] = []
        ob = outbus[j]
        if ob == 0:
            target[j] = "OUT"
        elif add[j]:
            pending[ob].append(j)
        else:
            pending[ob] = [j]

    for j in range(n):
        if target[j] is None:
            raise Syx2PatchError(
                f"algorithm decode: operator {j} (bus order) never resolved a "
                f"target (flags={[hex(f) for f in flags]}) -- DX7_ALGORITHMS "
                f"table is malformed"
            )

    needs_interleaved = False
    if secondary:
        adj: Dict[int, List[int]] = {j: [] for j in range(n)}
        for j in range(n):
            if target[j] != "OUT":
                adj[j].append(target[j])
        for j in secondary:
            adj[j].append(primary_op)
        needs_interleaved = _has_cycle(adj)

    routing = [OpRouting(target=target[j], feedback_capable=(j == primary_op)) for j in range(n)]
    return AlgorithmDecode(routing=routing, primary_fb_op=primary_op,
                            secondary_fb_ops=secondary, needs_interleaved=needs_interleaved)


ALGORITHM_CACHE: List[AlgorithmDecode] = [decode_algorithm(a) for a in DX7_ALGORITHMS]


# ---------------------------------------------------------------------------
# SysEx bulk-dump parsing + packed-voice unpacking.
# ---------------------------------------------------------------------------

def parse_syx_bulk(data: bytes, path: str) -> List[bytes]:
    """Validates and unwraps a 32-voice bulk dump, per the DX7 MIDI Data
    Format Sheet's own "Bulk Data for 32 Voices" structure -- returns the 32
    128-byte packed-voice chunks. Every check here fails loudly (raises)
    rather than guessing at malformed/truncated/wrong-format input."""
    expected_len = 6 + 4096 + 1 + 1
    if len(data) != expected_len:
        raise Syx2PatchError(
            f"{path}: expected a {expected_len}-byte 32-voice bulk dump "
            f"(6-byte header + 4096-byte payload + checksum + F7), got {len(data)} bytes"
        )
    if data[0] != 0xF0:
        raise Syx2PatchError(f"{path}: byte 0 = 0x{data[0]:02x}, expected 0xF0 (SysEx start)")
    if data[1] != 0x43:
        raise Syx2PatchError(f"{path}: byte 1 = 0x{data[1]:02x}, expected 0x43 (Yamaha ID) -- not a DX7 dump")
    sub_status = (data[2] >> 4) & 0x7
    if sub_status != 0:
        raise Syx2PatchError(
            f"{path}: byte 2 sub-status = {sub_status}, expected 0 (bulk data) -- "
            f"this looks like a parameter-change message, not a bulk dump"
        )
    if data[3] != 0x09:
        raise Syx2PatchError(
            f"{path}: format number = {data[3]}, expected 9 (32-voice bulk) -- "
            f"format 0 would be a single-voice dump, not supported by this converter"
        )
    byte_count = (data[4] << 7) | data[5]
    if byte_count != 4096:
        raise Syx2PatchError(f"{path}: byte count = {byte_count}, expected 4096 (32 voices x 128 bytes)")

    payload = data[6:6 + 4096]
    checksum = data[6 + 4096]
    terminator = data[6 + 4096 + 1]
    if terminator != 0xF7:
        raise Syx2PatchError(f"{path}: final byte = 0x{terminator:02x}, expected 0xF7 (SysEx end)")
    computed = (-sum(payload)) & 0x7F
    if computed != checksum:
        raise Syx2PatchError(
            f"{path}: checksum mismatch (file says 0x{checksum:02x}, computed 0x{computed:02x}) "
            f"-- corrupted or truncated dump"
        )
    return [payload[i * 128:(i + 1) * 128] for i in range(32)]


def _range_check(value: int, lo: int, hi: int, field_name: str, ctx: str) -> int:
    if not (lo <= value <= hi):
        raise Syx2PatchError(f"{ctx}: {field_name}={value} out of range [{lo},{hi}] -- corrupted sysex data")
    return value


@dataclass
class DX7Op:
    # Bus order: index into voice.ops, 0 = OP6 .. 5 = OP1 (see module docstring).
    eg_rate: List[int]    # R1-4, 0-99
    eg_level: List[int]   # L1-4, 0-99
    output_level: int     # 0-99 (DX7 TL)
    key_vel_sens: int     # 0-7
    osc_mode: int          # 0=ratio, 1=fixed -- v1 supports ratio only
    freq_coarse: int       # 0-31
    freq_fine: int         # 0-99
    # Parsed and range-checked, not yet consumed (deferred to #48, see
    # module docstring) -- kept for a future converter revision, same
    # "parsed but unread" convention speechgen.py's duration_ms uses.
    break_point: int
    scale_left_depth: int
    scale_right_depth: int
    scale_left_curve: int
    scale_right_curve: int
    rate_scale: int
    detune: int
    amp_mod_sens: int


@dataclass
class DX7Voice:
    ops: List[DX7Op]      # length 6, bus order (0=OP6..5=OP1)
    algorithm: int          # 0-31 (raw sysex byte; human/Dexed display is +1)
    feedback_level: int     # 0-7
    name: str
    # Parsed, range-checked, not yet consumed (deferred, see module docstring).
    osc_key_sync: int
    transpose: int


def unpack_voice(bulk: bytes, voice_idx: int) -> DX7Voice:
    """Bit-for-bit port of Dexed's Cartridge::unpackProgram (Source/
    PluginData.cpp, Apache-2.0) -- the packed-format bit layout it implements
    was cross-checked against the DX7 MIDI Data Format Sheet's own published
    table (tools/, see #47's implementation notes) and matches exactly.
    Unlike Dexed's own `normparm` (which silently clamps out-of-range bytes
    to avoid crashing on already-loaded, possibly-corrupt patches), this
    raises Syx2PatchError instead -- fail loud, not silent, on this
    converter's own input path."""
    ctx = f"voice {voice_idx}"
    ops: List[DX7Op] = []
    for j in range(6):
        base = j * 17
        op_ctx = f"{ctx} OP{6 - j}"
        eg_rate = [_range_check(bulk[base + i] & 0x7F, 0, 99, f"eg_rate{i + 1}", op_ctx) for i in range(4)]
        eg_level = [_range_check(bulk[base + 4 + i] & 0x7F, 0, 99, f"eg_level{i + 1}", op_ctx) for i in range(4)]
        bp = _range_check(bulk[base + 8] & 0x7F, 0, 99, "break_point", op_ctx)
        ld = _range_check(bulk[base + 9] & 0x7F, 0, 99, "scale_left_depth", op_ctx)
        rd = _range_check(bulk[base + 10] & 0x7F, 0, 99, "scale_right_depth", op_ctx)
        lrcurve = bulk[base + 11] & 0x0F
        lc = lrcurve & 3
        rc = (lrcurve >> 2) & 3
        det_rs = bulk[base + 12] & 0x7F
        rs = det_rs & 7
        det = _range_check((det_rs >> 3) & 0x0F, 0, 14, "detune", op_ctx)
        kvs_ams = bulk[base + 13] & 0x1F
        ams = kvs_ams & 3
        kvs = _range_check((kvs_ams >> 2) & 7, 0, 7, "key_vel_sens", op_ctx)
        ol = _range_check(bulk[base + 14] & 0x7F, 0, 99, "output_level", op_ctx)
        fcoarse_mode = bulk[base + 15] & 0x3F
        mode = fcoarse_mode & 1
        fcoarse = _range_check((fcoarse_mode >> 1) & 0x1F, 0, 31, "freq_coarse", op_ctx)
        ffine = _range_check(bulk[base + 16] & 0x7F, 0, 99, "freq_fine", op_ctx)
        ops.append(DX7Op(
            eg_rate=eg_rate, eg_level=eg_level, output_level=ol, key_vel_sens=kvs,
            osc_mode=mode, freq_coarse=fcoarse, freq_fine=ffine,
            break_point=bp, scale_left_depth=ld, scale_right_depth=rd,
            scale_left_curve=lc, scale_right_curve=rc, rate_scale=rs,
            detune=det, amp_mod_sens=ams,
        ))

    algorithm = _range_check(bulk[110] & 0x1F, 0, 31, "algorithm", ctx)
    oks_fb = bulk[111] & 0x0F
    feedback_level = _range_check(oks_fb & 7, 0, 7, "feedback_level", ctx)
    osc_key_sync = (oks_fb >> 3) & 1
    transpose = _range_check(bulk[117] & 0x7F, 0, 48, "transpose", ctx)
    name_bytes = bytes(bulk[118 + i] & 0x7F for i in range(10))
    name = name_bytes.decode("ascii", errors="replace").rstrip()

    return DX7Voice(ops=ops, algorithm=algorithm, feedback_level=feedback_level,
                     name=name, osc_key_sync=osc_key_sync, transpose=transpose)


# ---------------------------------------------------------------------------
# DX7 voice -> FmPatch.
# ---------------------------------------------------------------------------

def coarse_ratio(coarse: int) -> float:
    # DX7's own quirk, verified against Dexed's osc_freq()/coarsemul[]
    # (Source/msfa/dx7note.cc): coarse=0 is a special case (ratio 0.5), not
    # "0x the fundamental"; coarse>=1 is the ratio itself.
    return 0.5 if coarse == 0 else float(coarse)


def op_ratio(op: DX7Op) -> float:
    return coarse_ratio(op.freq_coarse) * (1.0 + 0.01 * op.freq_fine)


@dataclass
class FmOpOut:
    ratio: float
    level: int
    mod_target: int  # 0-5, or FM_TARGET_OUT
    feedback: bool
    output_level: int
    vel_sensitivity: int
    eg_rate: List[int]
    eg_level: List[int]


@dataclass
class FmPatchOut:
    name: str
    ident: str
    ops: List[FmOpOut]  # length 6, this engine's own op index order
    needs_interleaved: bool
    algorithm_display: int  # 1-32, for log messages


def convert_voice(idx: int, voice: DX7Voice, warnings: List[str]) -> Optional[FmPatchOut]:
    if any(op.osc_mode == 1 for op in voice.ops):
        warnings.append(
            f"voice {idx} ({voice.name!r}): uses fixed-frequency mode on at least one "
            f"operator -- deferred to #48, skipped"
        )
        return None

    decode = ALGORITHM_CACHE[voice.algorithm]
    # Algorithms 19-32 sum 3-6 carriers into the same voice output (see
    # DX7_ALGORITHMS' decode table); FM_CARRIER_LEVEL_REF was tuned against
    # FM_TEST_PATCH's single carrier, so N summed carriers each at that same
    # reference would sum to N times the intended full-scale peak -- a real,
    # measured int16 overflow on e.g. BRASS 1 (4 carriers) and E.ORGAN 1 (6
    # carriers) during #47's own host-render pass, not a hypothetical. Each
    # carrier gets 1/carrier_count of the reference so N summed carriers at
    # output_level=99 land back at the same envelope one carrier alone would.
    carrier_count = sum(1 for r in decode.routing if r.target == "OUT")
    # #57 raised the modulator headroom ceiling ~64x (FM_OUT_SHIFT_MODULATOR),
    # which makes the same overflow risk real on the *modulator* side too:
    # several real algorithms (7, 8, 10, 12, 13 -- all present in ROM1A/B)
    # sum 2-3 modulators into one shared bus. Each gets 1/fan_in of the
    # reference, same reasoning as carrier_count above -- N summed
    # modulators at output_level=99 land back at one modulator's intended
    # ceiling, not N times it (risking int32 wraparound on the shared bus).
    fan_in: Dict[object, int] = {}
    for r in decode.routing:
        if r.target != "OUT":
            fan_in[r.target] = fan_in.get(r.target, 0) + 1

    ops_out: List[Optional[FmOpOut]] = [None] * FM_NUM_OPS
    for j in range(6):
        r = decode.routing[j]
        op = voice.ops[j]
        engine_idx = 5 - j  # bus order (OP6..OP1) -> this engine's op index

        is_carrier = (r.target == "OUT")
        if is_carrier:
            level = FM_CARRIER_LEVEL_REF // carrier_count
        else:
            level = FM_MODULATOR_LEVEL_REF // fan_in[r.target]
        # Feedback level 0 means real, exact silence on real hardware too --
        # not an approximation, so no warning needed for this branch.
        feedback = r.feedback_capable and voice.feedback_level > 0

        eg_level = list(op.eg_level)
        if is_carrier and eg_level[3] != 0:
            warnings.append(
                f"voice {idx} ({voice.name!r}): OP{6 - j} (carrier) release level "
                f"L4={eg_level[3]} forced to 0 -- a nonzero carrier L4 never reaches "
                f"env_dx.h's EG_IDLE, so the voice could never be reclaimed by the "
                f"allocator (the tracker's #21 bug shape); see env_dx.h's own L4=0 note"
            )
            eg_level[3] = 0

        mod_target = FM_TARGET_OUT if r.target == "OUT" else (5 - r.target)
        ops_out[engine_idx] = FmOpOut(
            ratio=op_ratio(op), level=level, mod_target=mod_target, feedback=feedback,
            output_level=op.output_level, vel_sensitivity=op.key_vel_sens,
            eg_rate=op.eg_rate, eg_level=eg_level,
        )

    if decode.needs_interleaved:
        warnings.append(
            f"voice {idx} ({voice.name!r}): algorithm {voice.algorithm + 1} has a second, "
            f"operator-spanning feedback loop this engine can't represent yet (#54, "
            f"fm.md open question 6) -- collapsed to single self-feedback on the primary "
            f"operator only; the secondary loop is dropped, explicitly, not silently"
        )

    ident_base = _sanitize_ident(voice.name)
    return FmPatchOut(name=voice.name, ident=ident_base, ops=[o for o in ops_out if o is not None],
                       needs_interleaved=decode.needs_interleaved, algorithm_display=voice.algorithm + 1)


def _sanitize_ident(name: str) -> str:
    out = "".join(c if (c.isalnum() or c == "_") else "_" for c in name.strip())
    out = out.strip("_")
    if not out or out[0].isdigit():
        out = "P_" + out
    return out.upper()


def _make_unique(base: str, used: Dict[str, int]) -> str:
    if base not in used:
        used[base] = 1
        return base
    used[base] += 1
    return f"{base}_{used[base]}"


# ---------------------------------------------------------------------------
# patches.h emission.
# ---------------------------------------------------------------------------

def render_header(patches: List[FmPatchOut], syx_path: str) -> str:
    lines = [
        f"// GENERATED by tools/syx2patch.py convert from {os.path.basename(syx_path)} -- do not hand-edit.",
        "// v1 (#47, fm.md P3): algorithm routing, EG rates/levels, output level,",
        "// velocity sensitivity, coarse/fine ratio, feedback on/off, algorithm 4/6",
        "// interleaved-feedback detection (collapsed to single self-feedback, see the",
        "// per-patch comment below where it applies). Key level scaling, rate",
        "// scaling, detune, fixed-frequency mode, pitch EG, LFO, and transpose are",
        "// parsed from the source .syx but deliberately not wired into this engine",
        "// yet -- deferred to #48 (v2, calibrated against Dexed).",
        "//",
        "// `level` (the reference-gain ceiling) is NOT DX7 data -- see patch.h's own",
        "// FmOpParams comment. Every carrier here uses FM_CARRIER_LEVEL_REF, every",
        "// modulator FM_MODULATOR_LEVEL_REF (both taken from FM_TEST_PATCH's already",
        "// hardware-verified constants, tools/syx2patch.py); the real per-patch,",
        "// per-operator balance comes entirely from output_level/the EG, same as on",
        "// real DX7 hardware.",
        "#pragma once",
        "",
        '#include "patch.h"',
        "",
        "enum FmPatchId : uint8_t {",
    ]
    for p in patches:
        lines.append(f"    FMPATCH_{p.ident},")
    lines.append("    FM_PATCH_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr FmPatch FM_PATCHES[FM_PATCH_COUNT] = {")
    for p in patches:
        note = f"  // algorithm {p.algorithm_display}: second feedback loop dropped, see #54" if p.needs_interleaved else ""
        lines.append(f'    {{ "{p.name}", {{{note}')
        for op in p.ops:
            eg_r = ", ".join(str(v) for v in op.eg_rate)
            eg_l = ", ".join(str(v) for v in op.eg_level)
            mod_target = "FM_TARGET_OUT" if op.mod_target == FM_TARGET_OUT else str(op.mod_target)
            fb = "true" if op.feedback else "false"
            lines.append(
                f"        {{ {op.ratio:.6f}f, 0.0f, false, 0.0f, {op.level}, {mod_target}, {fb}, "
                f"{op.output_level}, {op.vel_sensitivity}, {{ {eg_r} }}, {{ {eg_l} }} }},"
            )
        lines.append("    } },")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr const char *FM_PATCH_NAMES[FM_PATCH_COUNT] = {")
    for p in patches:
        lines.append(f'    "{p.name}",')
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


# sizeof(FmPatch) estimate for the printed flash-cost summary: FmOpParams is
# 3 floats (12) + bool (1, padded) + int32_t level (4) + mod_target (1) +
# feedback bool (1) + output_level (1) + vel_sensitivity (1) + eg_rate[4]
# (4) + eg_level[4] (4) -- rounds to 32 bytes/op under normal 4-byte
# struct alignment; plus one 4-byte name pointer per patch. Matches fm.md
# §8's ~200 B/patch budget (6*32 + 4 = 196); not a compiled sizeof() (no
# device toolchain invoked here), just a documented estimate.
_BYTES_PER_OP_ESTIMATE = 32
_BYTES_PER_PATCH_ESTIMATE = 4 + FM_NUM_OPS * _BYTES_PER_OP_ESTIMATE


def _convert_all(syx_path: str) -> tuple[List[FmPatchOut], List[str], int]:
    with open(syx_path, "rb") as f:
        data = f.read()
    voices_raw = parse_syx_bulk(data, syx_path)

    warnings: List[str] = []
    used_idents: Dict[str, int] = {}
    patches: List[FmPatchOut] = []
    skipped = 0
    for idx, raw in enumerate(voices_raw):
        voice = unpack_voice(raw, idx)
        out = convert_voice(idx, voice, warnings)
        if out is None:
            skipped += 1
            continue
        out.ident = _make_unique(out.ident, used_idents)
        patches.append(out)
    return patches, warnings, skipped


def cmd_convert(args: argparse.Namespace) -> None:
    try:
        patches, warnings, skipped = _convert_all(args.syx)
    except Syx2PatchError as exc:
        print(f"syx2patch: {exc}", file=sys.stderr)
        sys.exit(1)

    if not patches:
        print("syx2patch: no patches survived conversion -- nothing to write", file=sys.stderr)
        sys.exit(1)

    with open(args.header, "w") as f:
        f.write(render_header(patches, args.syx))

    table_bytes = len(patches) * _BYTES_PER_PATCH_ESTIMATE
    print(f"Wrote {args.header} ({len(patches)}/{len(patches) + skipped} voices converted, "
          f"{skipped} skipped, ~{table_bytes} bytes in flash -- estimate, see fm.md §8)")
    for w in warnings:
        print(f"  note: {w}")


def cmd_dump(args: argparse.Namespace) -> None:
    try:
        patches, warnings, skipped = _convert_all(args.syx)
    except Syx2PatchError as exc:
        print(f"syx2patch: {exc}", file=sys.stderr)
        sys.exit(1)

    for p in patches:
        flag = " [INTERLEAVED-FALLBACK]" if p.needs_interleaved else ""
        print(f"  {p.ident:24s} algo={p.algorithm_display:2d}{flag}  \"{p.name}\"")
    print(f"{len(patches)} converted, {skipped} skipped")
    for w in warnings:
        print(f"  note: {w}")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="syx2patch")
    sub = parser.add_subparsers(dest="command", required=True)

    p_convert = sub.add_parser("convert", help="parse a 32-voice .syx bulk dump and emit patches.h")
    p_convert.add_argument("syx", help="input .syx file (32-voice bulk dump, 4104 bytes)")
    p_convert.add_argument("header", help="output path for patches.h")
    p_convert.set_defaults(func=cmd_convert)

    p_dump = sub.add_parser("dump", help="parse and print a per-voice summary, write nothing")
    p_dump.add_argument("syx")
    p_dump.set_defaults(func=cmd_dump)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
