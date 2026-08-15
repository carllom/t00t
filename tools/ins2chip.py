#!/usr/bin/env python3
"""ins2chip.py -- GoatTracker .ins instrument file -> chip_instruments.txt block
(module_chip.md §1, §11.2 ".ins compatibility").

    ins2chip.py <file.ins> [file2.ins ...] [-o out.txt]

Emits one `instrument NAME ... end` block (chipgen.py's grammar, see
tools/chip_instruments.txt's header comment) per input file, to stdout or
-o's file. Run the result through chipgen.py same as any hand-authored
source:

    ins2chip.py mypatch.ins >> tools/chip_instruments.txt
    chipgen.py gen tools/chip_instruments.txt src/engines/chip/instruments.h

Byte layout below was read directly out of GoatTracker2's own save/load
source (src/gsong.c: saveinstrument()/loadinstrument(), src/gcommon.h's
INSTR struct) and cross-checked field-by-field against two real .ins files
(examples/sfx_arp1.ins, examples/sfx_gun.ins) from the same tree --
leafo/goattracker2 on GitHub, commit current as of 2026-08. ptr[] relocation
and the wave/pulse/filter row grammar below are only visible in gplay.c's
actual playback code, not documented in the repo's own readme or
ChiptuneSAK's docs.

What this converter refuses to guess at, and errors out on instead of
silently mis-translating (module_chip.md §11.2 already flags most of this at
the design level):

  * WAVECMD rows (wavetable-embedded portamento/vibrato/set-AD/set-SR/
    set-wave/set-filterptr/etc, GT's 0xF0-0xFE wave byte range) -- these
    dispatch through GT's per-tick command executor, which chip's frame VM
    has no equivalent of.
  * Absolute-pitch wavetable rows (note byte 0x80-0xFF, used as a literal
    freqtable index instead of an offset from the played note) -- chip's
    WaveRow.note is always relative to the played note; there is no slot
    for "ignore the played note, use this fixed pitch".
  * A pulse or filter table that sets its absolute value anywhere but the
    very first row -- chip's Instrument has one pulse_init/filter_cutoff,
    not a re-seekable pointer.
  * GT's real vibrato (CMD_VIBRATO, itself a WAVECMD case, driven by a
    speed-table entry) has no relationship to chip's per-instrument
    depth/speed/delay LFO; standalone .ins files can't carry it anyway
    since vibdelay only means anything paired with a pattern-level vibrato
    command that lives in the .sng, not the .ins.
"""

import argparse
import os
import re
import sys

WAVE_BITS = {"tri": 0x1, "saw": 0x2, "pulse": 0x4, "noise": 0x8}
FILTER_BITS = {"lp": 0x1, "bp": 0x2, "hp": 0x4}

MAX_TABLELEN = 255

# Real SID $d404-style waveform/control byte: bit4-7 = tone, bit0-3 = gate/
# sync/ring/test (gcommon.h doesn't name these, they're SID register bits).
SID_TONE_MASK = 0xF0
SID_CTRL_MASK = 0x0F
SID_GATE = 0x01
SID_SYNC = 0x02
SID_RING = 0x04
SID_TEST = 0x08

WAVEDELAY = 0x01
WAVEDELAY_MAX = 0x0F
WAVESILENT, WAVELASTSILENT = 0xE0, 0xEF
WAVECMD, WAVELASTCMD = 0xF0, 0xFE
WAVE_JUMP = 0xFF

WAVECMD_NAMES = {
    0: "CMD_DONOTHING", 1: "CMD_PORTAUP", 2: "CMD_PORTADOWN", 3: "CMD_TONEPORTA",
    4: "CMD_VIBRATO", 5: "CMD_SETAD", 6: "CMD_SETSR", 7: "CMD_SETWAVE",
    8: "CMD_SETWAVEPTR", 9: "CMD_SETPULSEPTR", 10: "CMD_SETFILTERPTR",
    11: "CMD_SETFILTERCTRL", 12: "CMD_SETFILTERCUTOFF", 13: "CMD_SETMASTERVOL",
    14: "CMD_FUNKTEMPO",
}


class ConvertError(Exception):
    def __init__(self, path, where, msg):
        super().__init__(f"{path}: {where}: {msg}")


def fmt_wave_bits(our_bits):
    names = [name for name, bit in WAVE_BITS.items() if our_bits & bit]
    return "+".join(names) if names else "hold"


def translate_tone_bits(real_byte, path, where):
    ctrl = real_byte & SID_CTRL_MASK
    if ctrl & SID_SYNC:
        raise ConvertError(path, where, "sync bit set -- not representable (chip's WaveRow has no modulation flags yet)")
    if ctrl & SID_RING:
        raise ConvertError(path, where, "ring-mod bit set -- not representable (chip's WaveRow has no modulation flags yet)")
    if ctrl & SID_TEST:
        raise ConvertError(path, where, "test bit set -- not representable")
    return (real_byte & SID_TONE_MASK) >> 4  # 0x10/0x20/0x40/0x80 -> 0x1/0x2/0x4/0x8


def read8(data, pos, path):
    if pos >= len(data):
        raise ConvertError(path, f"byte {pos}", "unexpected end of file")
    return data[pos]


def parse_ins(path):
    with open(path, "rb") as f:
        data = f.read()

    magic = data[0:4]
    if magic in (b"GTI3", b"GTI4", b"GTI5"):
        n_tables = 4
    elif magic == b"GTI2":
        n_tables = 3
    else:
        raise ConvertError(path, "header", f"unrecognized magic {magic!r} (expected GTI2/GTI3/GTI4/GTI5)")

    pos = 4
    ad = read8(data, pos, path); pos += 1
    sr = read8(data, pos, path); pos += 1
    optr = [read8(data, pos + i, path) for i in range(n_tables)]
    pos += n_tables
    vibdelay = read8(data, pos, path); pos += 1
    if n_tables == 3:
        # GTI2: no on-disk speed-table pointer slot -- the byte here is a
        # raw old-format vibrato speed, converted at load time via
        # makespeedtable() into a fresh STBL entry. Not representable
        # standalone (see module docstring); recorded only for the warning.
        old_vib_speed = read8(data, pos, path); pos += 1
    else:
        old_vib_speed = None
    gatetimer = read8(data, pos, path); pos += 1
    firstwave = read8(data, pos, path); pos += 1
    name_raw = data[pos:pos + 16]; pos += 16
    name = name_raw.split(b"\x00", 1)[0].decode("latin-1", "replace")

    tables = []
    for c in range(n_tables):
        length = read8(data, pos, path); pos += 1
        ltab = list(data[pos:pos + length]); pos += length
        rtab = list(data[pos:pos + length]); pos += length
        if len(ltab) != length or len(rtab) != length:
            raise ConvertError(path, f"table {c}", "truncated file")
        tables.append((ltab, rtab))

    return {
        "path": path, "n_tables": n_tables, "ad": ad, "sr": sr, "optr": optr,
        "vibdelay": vibdelay, "old_vib_speed": old_vib_speed,
        "gatetimer": gatetimer, "firstwave": firstwave, "name": name,
        "tables": tables,
    }


def decode_wave_table(ltab, rtab, optr_w, path):
    # gplay.c's WAVEEXEC, traced tick-by-tick (verified against a real
    # "arp minor 7" instrument's actual note sequence, not just the source):
    # a delay row (w in 1..15) holds the *previous* row's pitch for w ticks,
    # then on tick w+1 falls through and, only if note != 0x80, applies its
    # own note as a new one-tick event. So note == 0x80 extends the previous
    # emitted row by w+1 ticks (nothing ever changes); a real note extends
    # it by w ticks of pure hold, then a separate new one-tick row (waveform
    # untouched -- a delay row never sets cptr->wave, so "hold" is exact).
    rows = []          # list of [wave_bits_or_None(hold), note_offset, repeat]
    raw_to_emitted = {}
    i = 0
    n = len(ltab)
    while i < n:
        w, note = ltab[i], rtab[i]
        where = f"wave row {i}"

        if w == WAVE_JUMP:
            # Loop marker -- must be the last row. A raw target of exactly 0
            # is GT's own "no table" null pointer (gplay.c: `if
            # (cptr->ptr[WTBL])`), not row 0 -- loadinstrument() skips the
            # relocation arithmetic entirely when the raw value is 0
            # (`if (rtable[c][d]) rtable[c][d] = ...`). It means "table ends
            # here, don't loop", same as chip's own wave_loop being omitted.
            if i != n - 1:
                raise ConvertError(path, where, "wave-table jump row before end of table -- not supported")
            if note == 0:
                return rows, None
            raw_target = note - optr_w
            if raw_target not in raw_to_emitted:
                raise ConvertError(path, where, f"loop target resolves outside emitted table (raw row {raw_target})")
            return rows, raw_to_emitted[raw_target]
        elif WAVECMD <= w <= WAVELASTCMD:
            cmd = WAVECMD_NAMES.get(w & 0xF, f"cmd 0x{w:x}")
            raise ConvertError(path, where, f"wavetable command row ({cmd}) -- not supported")
        elif WAVEDELAY <= w <= WAVEDELAY_MAX:
            # Note w == 0 is deliberately NOT in this range -- see below,
            # it's a real (if waveform-less) row of its own, not a delay.
            if not rows:
                raise ConvertError(path, where, "delay row with no preceding row to extend")
            if note == 0x80:
                # Pure hold: this raw row contributes nothing new, a jump
                # landing here is indistinguishable from landing on the row
                # it's extending.
                rows[-1][2] += w + 1
                raw_to_emitted[i] = len(rows) - 1
                i += 1
                continue
            if note >= 0x80:
                raise ConvertError(path, where, f"absolute-pitch note byte 0x{note:x} -- chip's WaveRow is always relative to the played note")
            # Hold-then-transition. A jump landing exactly on this raw row
            # would, in real GT, restart its own w-frame hold (at whatever
            # pitch is playing then) before transitioning -- chip's model
            # can't express "hold at whatever's current" as a fixed target,
            # so a loop here maps straight to the transition row instead,
            # dropping up to w frames (<=15, 300ms) of hold on repeat
            # passes only. Approximation, not exact -- see history_chip.md §14e.3.
            rows[-1][2] += w
            raw_to_emitted[i] = len(rows)
            rows.append([None, note, 1])
            i += 1
            continue
        elif w == 0:
            # gplay.c's WAVEEXEC: wave <= WAVELASTDELAY(15) is the "delay"
            # branch, but wavetime(0) != wave(0) is false for w == 0, so it
            # never actually holds -- it falls straight through to applying
            # this row's own note this same tick. A real, single-frame row
            # that changes note without changing waveform, i.e. chip's own
            # "hold" keyword used deliberately rather than as a repeat-fold.
            wave_bits = None
        elif WAVESILENT <= w <= WAVELASTSILENT:
            wave_bits = None  # "hold" -- keep current waveform, silent variant collapses to same slot
        else:
            wave_bits = translate_tone_bits(w, path, where)

        if note == 0x80:
            note_offset = 0
        elif note < 0x80:
            note_offset = note
        else:
            raise ConvertError(path, where, f"absolute-pitch note byte 0x{note:x} -- chip's WaveRow is always relative to the played note")

        raw_to_emitted[i] = len(rows)
        rows.append([wave_bits, note_offset, 1])
        i += 1

    return rows, None


def decode_pulse_table(ltab, rtab, optr_p, path):
    # A mid-table absolute-set row (real GT: legitimately resets the pulse
    # register to a fixed value partway through a sweep, not just at the
    # start) has no chip equivalent -- there's one pulse_init, not a
    # reseekable pointer. But since we're statically decoding the whole
    # table anyway, we can simulate the running pulse value through every
    # prior delta/duration row (chip's own vm_frame_tick clamps [0, 4095]
    # every frame, replicated frame-by-frame below) and synthesize an exact
    # one-frame delta row that lands on the same absolute value instead of
    # refusing outright.
    if not ltab:
        return None, [], None
    pulse_init = None
    rows = []
    raw_to_emitted = {}
    running = 2048  # matches emit_block's own fallback if row 0 never sets one
    i = 0
    n = len(ltab)
    while i < n:
        where = f"pulse row {i}"
        if ltab[i] == WAVE_JUMP:
            if i != n - 1:
                raise ConvertError(path, where, "pulse-table jump row before end of table -- not supported")
            if rtab[i] == 0:  # raw 0 = GT's null pointer, "stop", not "loop to row 0"
                return pulse_init, rows, None
            raw_target = rtab[i] - optr_p
            if raw_target == 0 and pulse_init is not None and 0 not in raw_to_emitted:
                # Loops back to the initial absolute-set row itself -- not a
                # `rows` entry (pulse_init is a scalar, applied once at
                # trigger, not a table row), so synthesize the equivalent
                # one-frame snap back to that value instead of refusing.
                delta = pulse_init - running
                raw_to_emitted[0] = len(rows)
                rows.append((delta, 1))
            if raw_target not in raw_to_emitted:
                raise ConvertError(path, where, f"loop target resolves outside emitted table (raw row {raw_target})")
            return pulse_init, rows, raw_to_emitted[raw_target]
        if ltab[i] >= 0x80:
            value = ((ltab[i] & 0xF) << 8) | rtab[i]
            if i == 0:
                pulse_init = value
            else:
                delta = value - running
                raw_to_emitted[i] = len(rows)
                rows.append((delta, 1))
            running = value
            i += 1
            continue
        if ltab[i] == 0:
            raise ConvertError(path, where, "zero-duration pulse row (never advances in real GT either -- likely authoring error)")
        duration = ltab[i]
        raw = rtab[i]
        delta = raw - 0x100 if raw >= 0x80 else raw
        for _ in range(duration):
            running = max(0, min(4095, running + delta))
        raw_to_emitted[i] = len(rows)
        rows.append((delta, duration))
        i += 1
    return pulse_init, rows, None


def decode_filter_table(ltab, rtab, optr_f, path):
    if not ltab:
        return None
    n = len(ltab)
    if n < 2 or ltab[0] < 0x80 or ltab[1] != 0x00:
        raise ConvertError(path, "filter row 0",
                            "expected a type/resonance row (>=0x80) immediately followed by a cutoff row (0x00) -- "
                            "chip needs a static mode/cutoff/resonance, and a standalone .ins can't tell us what the "
                            "cutoff was before this table starts otherwise")
    mode_real = ltab[0] & 0x70
    mode = 0
    if mode_real & 0x10:
        mode |= FILTER_BITS["lp"]
    if mode_real & 0x20:
        mode |= FILTER_BITS["bp"]
    if mode_real & 0x40:
        mode |= FILTER_BITS["hp"]
    if mode == 0:
        raise ConvertError(path, "filter row 0", "type row selects no lp/bp/hp bit")
    resonance = (rtab[0] >> 4) & 0xF
    cutoff = rtab[1]

    # Same reasoning as decode_pulse_table's mid-table absolute-set case: a
    # later cutoff-set row (ltab == 0) has no direct chip equivalent, but we
    # can simulate the running cutoff through every prior delta/duration row
    # (chip's own vm_frame_tick clamps [0, 2047] every frame) and synthesize
    # an exact one-frame delta landing on the same value.
    rows = []
    raw_to_emitted = {}
    running = cutoff
    i = 2
    while i < n:
        where = f"filter row {i}"
        if ltab[i] == WAVE_JUMP:
            if i != n - 1:
                raise ConvertError(path, where, "filter-table jump row before end of table -- not supported")
            if rtab[i] == 0:  # raw 0 = GT's null pointer, "stop", not "loop to row 0"
                return {"mode": mode, "cutoff": cutoff, "resonance": resonance, "rows": rows, "loop": None}
            raw_target = rtab[i] - optr_f
            if raw_target in (0, 1) and raw_target not in raw_to_emitted:
                # Loops back to the type or cutoff header row -- neither is
                # a `rows` entry (both fold into the static cutoff/resonance
                # this function returns, applied once at trigger), so
                # synthesize the equivalent one-frame snap back to the
                # initial cutoff instead of refusing.
                delta = cutoff - running
                raw_to_emitted[raw_target] = len(rows)
                rows.append((delta, 1))
            if raw_target not in raw_to_emitted:
                raise ConvertError(path, where, f"loop target resolves outside emitted table (raw row {raw_target})")
            return {"mode": mode, "cutoff": cutoff, "resonance": resonance, "rows": rows, "loop": raw_to_emitted[raw_target]}
        if ltab[i] >= 0x80:
            raise ConvertError(path, where, "filter type/ctrl row after the first row -- not supported")
        if ltab[i] == 0:
            value = rtab[i]
            delta = value - running
            raw_to_emitted[i] = len(rows)
            rows.append((delta, 1))
            running = value
            i += 1
            continue
        duration = ltab[i]
        raw = rtab[i]
        delta = raw - 0x100 if raw >= 0x80 else raw
        for _ in range(duration):
            running = max(0, min(2047, running + delta))
        raw_to_emitted[i] = len(rows)
        rows.append((delta, duration))
        i += 1
    return {"mode": mode, "cutoff": cutoff, "resonance": resonance, "rows": rows, "loop": None}


IDENT_RE = re.compile(r"[^A-Za-z0-9_]")


def make_ident(name, fallback):
    base = name.strip() or fallback
    ident = IDENT_RE.sub("_", base).strip("_").upper()
    if not ident or ident[0].isdigit():
        ident = "INS_" + ident
    return ident


def emit_block(ins, name):
    ad_hi, ad_lo = ins["ad"] >> 4, ins["ad"] & 0xF
    sr_hi, sr_lo = ins["sr"] >> 4, ins["sr"] & 0xF
    gate_off = ins["gatetimer"] & 0x3F
    firstwave_bits = translate_tone_bits(ins["firstwave"], ins["path"], "firstwave") if (ins["firstwave"] & SID_TONE_MASK) else 0

    wave_rows, wave_loop = ins["wave_decoded"]
    pulse_init, pulse_rows, pulse_loop = ins["pulse_decoded"]
    filt = ins["filter_decoded"]

    uses_pulse = bool(firstwave_bits & WAVE_BITS["pulse"]) or any(w == WAVE_BITS["pulse"] for w, _, _ in wave_rows)

    lines = [f"instrument {name}"]
    lines.append(f"    ad {ad_hi} {ad_lo}")
    lines.append(f"    sr {sr_hi} {sr_lo}")
    if firstwave_bits:
        lines.append(f"    first_wave {fmt_wave_bits(firstwave_bits)}")
    if gate_off:
        lines.append(f"    gate_off {gate_off}")
    if ins["gatetimer"] & 0xC0:
        lines.append(f"    # NOTE: source gatetimer 0x{ins['gatetimer']:02x} set retrigger-suppression bits (0x40/0x80) -- not modeled, every note retriggers AD here")

    if uses_pulse or pulse_rows or pulse_init is not None:
        value = pulse_init if pulse_init is not None else 2048
        lines.append(f"    pulse_init {value}")
        if pulse_init is None:
            lines.append("    # NOTE: source pulse table had no absolute-set row -- defaulted pulse_init to 2048 to avoid the degenerate pw=0 case")

    if wave_rows:
        lines.append("    wave")
        for wave_bits, note_offset, repeat in wave_rows:
            wave_str = fmt_wave_bits(wave_bits) if wave_bits is not None else "hold"
            x = f" x{repeat}" if repeat != 1 else ""
            lines.append(f"        {wave_str} {note_offset}{x}")
        if wave_loop is not None:
            lines.append(f"    wave_loop {wave_loop}")

    if pulse_rows:
        lines.append("    pulse")
        for delta, duration in pulse_rows:
            lines.append(f"        {delta} {duration}")
        if pulse_loop is not None:
            lines.append(f"    pulse_loop {pulse_loop}")

    if filt is not None:
        lines.append(f"    filter {_filter_mode_str(filt['mode'])} cutoff={filt['cutoff']} resonance={filt['resonance']}")
        for delta, duration in filt["rows"]:
            lines.append(f"        {delta} {duration}")
        if filt["loop"] is not None:
            lines.append(f"    filter_loop {filt['loop']}")

    lines.append("end")
    return "\n".join(lines) + "\n"


def _filter_mode_str(bits):
    names = [name for name, bit in FILTER_BITS.items() if bits & bit]
    return "+".join(names)


def convert_one(path):
    ins = parse_ins(path)
    ins["wave_decoded"] = decode_wave_table(ins["tables"][0][0], ins["tables"][0][1], ins["optr"][0], path)
    ins["pulse_decoded"] = decode_pulse_table(ins["tables"][1][0], ins["tables"][1][1], ins["optr"][1], path)
    ins["filter_decoded"] = decode_filter_table(ins["tables"][2][0], ins["tables"][2][1], ins["optr"][2], path)

    fallback = os.path.splitext(os.path.basename(path))[0]
    name = make_ident(ins["name"], fallback)

    return emit_block(ins, name)


def main():
    ap = argparse.ArgumentParser(description="Convert GoatTracker .ins files to chip_instruments.txt blocks.")
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", help="write to this file instead of stdout (appends if it exists)")
    args = ap.parse_args()

    blocks = []
    failed = False
    for path in args.files:
        try:
            blocks.append(convert_one(path))
        except ConvertError as e:
            print(f"ins2chip.py: {e}", file=sys.stderr)
            failed = True

    if blocks:
        text = "\n".join(blocks)
        if args.out:
            mode = "a" if os.path.exists(args.out) else "w"
            with open(args.out, mode) as f:
                f.write(text)
            print(f"ins2chip.py: {len(blocks)} instrument(s) -> {args.out}", file=sys.stderr)
        else:
            sys.stdout.write(text)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
