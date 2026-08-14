#!/usr/bin/env python3
"""chipgen.py -- chip instrument text format -> instruments.h (module_chip.md §6, §11.2).

    chipgen.py gen <chip_instruments.txt> <instruments.h>

The device ships a generated table; a typo in the source text fails this
script with a line number instead of turning into a wrong instrument
discovered by ear, or silently reading garbage past the end of a table.

See tools/chip_instruments.txt's own header comment for the text format's
grammar. This script is the authority on it -- change one without the other
and the next `chipgen.py gen` run will produce a header that disagrees with
the comment describing it.
"""

import argparse
import re
import sys

WAVE_BITS = {"tri": 0x1, "saw": 0x2, "pulse": 0x4, "noise": 0x8}
FILTER_BITS = {"lp": 0x1, "bp": 0x2, "hp": 0x4}
WAVE_HOLD = 0xFF

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class GenError(Exception):
    def __init__(self, path, lineno, msg):
        super().__init__(f"{path}:{lineno}: {msg}")


def parse_bits(token, table, path, lineno, what):
    val = 0
    for name in token.split("+"):
        if name not in table:
            raise GenError(path, lineno, f"unknown {what} '{name}' (known: {', '.join(sorted(table))})")
        val |= table[name]
    return val


def parse_int(token, path, lineno, lo=None, hi=None, what="value"):
    try:
        v = int(token, 0)
    except ValueError:
        raise GenError(path, lineno, f"'{token}' is not an integer ({what})")
    if lo is not None and v < lo or hi is not None and v > hi:
        raise GenError(path, lineno, f"{what} {v} out of range [{lo}, {hi}]")
    return v


def parse_file(path):
    with open(path) as f:
        raw_lines = f.readlines()

    instruments = []
    cur = None
    section = None  # None | "wave" | "pulse" | "filter"

    for lineno, raw in enumerate(raw_lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        kw = parts[0]

        if kw == "instrument":
            if cur is not None:
                raise GenError(path, lineno, "nested 'instrument' -- missing 'end'?")
            if len(parts) != 2:
                raise GenError(path, lineno, "usage: instrument NAME")
            name = parts[1]
            if not IDENT_RE.match(name):
                raise GenError(path, lineno, f"'{name}' is not a valid identifier")
            if any(i["name"] == name for i in instruments):
                raise GenError(path, lineno, f"duplicate instrument '{name}'")
            cur = {
                "name": name, "line": lineno,
                "ad": (0, 0), "sr": (0, 0), "first_wave": 0,
                "gate_off": 0, "vibrato": (0, 0, 0), "pulse_init": 0,
                "wave_rows": [], "wave_loop": None,
                "pulse_rows": [], "pulse_loop": None,
                "uses_filter": False, "filter_mode": 0,
                "filter_cutoff": 0, "filter_resonance": 0,
                "filter_rows": [], "filter_loop": None,
            }
            section = None
            continue

        if cur is None:
            raise GenError(path, lineno, f"'{kw}' outside any 'instrument' block")

        if kw == "end":
            instruments.append(cur)
            cur = None
            section = None
            continue

        if kw == "ad":
            cur["ad"] = (parse_int(parts[1], path, lineno, 0, 15, "attack"),
                         parse_int(parts[2], path, lineno, 0, 15, "decay"))
            section = None
        elif kw == "sr":
            cur["sr"] = (parse_int(parts[1], path, lineno, 0, 15, "sustain"),
                         parse_int(parts[2], path, lineno, 0, 15, "release"))
            section = None
        elif kw == "first_wave":
            cur["first_wave"] = parse_bits(parts[1], WAVE_BITS, path, lineno, "waveform")
            section = None
        elif kw == "gate_off":
            cur["gate_off"] = parse_int(parts[1], path, lineno, 0, 255, "gate_off")
            section = None
        elif kw == "vibrato":
            cur["vibrato"] = tuple(parse_int(parts[k], path, lineno, 0, 255, "vibrato")
                                    for k in (1, 2, 3))
            section = None
        elif kw == "pulse_init":
            cur["pulse_init"] = parse_int(parts[1], path, lineno, 0, 4095, "pulse_init")
            section = None
        elif kw == "wave" and len(parts) == 1:
            section = "wave"
        elif kw == "wave_loop":
            cur["wave_loop"] = parse_int(parts[1], path, lineno, 0, 255, "wave_loop")
            section = None
        elif kw == "pulse" and len(parts) == 1:
            # The len(parts) == 1 guard matters: "pulse" alone opens the
            # pulse-sweep section, but "pulse" is also a valid WAVE_BITS
            # name, so a wave row using it (e.g. "pulse 15") has the same
            # parts[0] -- without the guard this branch would wrongly steal
            # it as a bare section header instead of falling through to the
            # `section == "wave"` row parser below.
            section = "pulse"
        elif kw == "pulse_loop":
            cur["pulse_loop"] = parse_int(parts[1], path, lineno, 0, 255, "pulse_loop")
            section = None
        elif kw == "filter":
            if len(parts) < 2:
                raise GenError(path, lineno, "usage: filter MODE cutoff=N resonance=N")
            cur["uses_filter"] = True
            cur["filter_mode"] = parse_bits(parts[1], FILTER_BITS, path, lineno, "filter mode")
            for kv in parts[2:]:
                if "=" not in kv:
                    raise GenError(path, lineno, f"expected key=value, got '{kv}'")
                key, val = kv.split("=", 1)
                if key == "cutoff":
                    cur["filter_cutoff"] = parse_int(val, path, lineno, 0, 2047, "cutoff")
                elif key == "resonance":
                    cur["filter_resonance"] = parse_int(val, path, lineno, 0, 15, "resonance")
                else:
                    raise GenError(path, lineno, f"unknown filter parameter '{key}'")
            section = "filter"
        elif kw == "filter_loop":
            cur["filter_loop"] = parse_int(parts[1], path, lineno, 0, 255, "filter_loop")
            section = None
        elif section == "wave":
            if len(parts) not in (2, 3):
                raise GenError(path, lineno, "wave row: WAVE NOTE_OFFSET [xN]")
            wave = WAVE_HOLD if parts[0] == "hold" else parse_bits(parts[0], WAVE_BITS, path, lineno, "waveform")
            note = parse_int(parts[1], path, lineno, -128, 127, "note offset")
            repeat = 1
            if len(parts) == 3:
                if not parts[2].startswith("x"):
                    raise GenError(path, lineno, f"expected 'xN' repeat count, got '{parts[2]}'")
                repeat = parse_int(parts[2][1:], path, lineno, 1, 255, "repeat count")
            for _ in range(repeat):
                cur["wave_rows"].append((wave, note))
        elif section in ("pulse", "filter"):
            if len(parts) != 2:
                raise GenError(path, lineno, f"{section} row: DELTA DURATION")
            delta = parse_int(parts[0], path, lineno, -32768, 32767, "delta")
            duration = parse_int(parts[1], path, lineno, 0, 255, "duration")
            cur[f"{section}_rows"].append((delta, duration))
        else:
            raise GenError(path, lineno, f"unknown directive '{kw}'")

    if cur is not None:
        raise GenError(path, cur["line"], f"instrument '{cur['name']}' missing 'end'")
    if not instruments:
        raise GenError(path, 1, "no instruments defined")

    for ins in instruments:
        for tbl in ("wave", "pulse", "filter"):
            rows = ins[f"{tbl}_rows"]
            loop = ins[f"{tbl}_loop"]
            if len(rows) > 255:
                raise GenError(path, ins["line"], f"'{ins['name']}': {tbl} table has {len(rows)} rows, max 255")
            if loop is not None and rows and loop >= len(rows):
                raise GenError(path, ins["line"],
                                f"'{ins['name']}': {tbl}_loop {loop} is out of range for a {len(rows)}-row table")
            if loop is not None and not rows:
                raise GenError(path, ins["line"], f"'{ins['name']}': {tbl}_loop set but table is empty")

    return instruments


def wave_row_str(w, n):
    wf = "WAVE_HOLD" if w == WAVE_HOLD else f"0x{w:x}"
    return f"{{ {wf}, 0, {n} }}"


def sweep_row_str(delta, duration):
    return f"{{ {delta}, {duration} }}"


def emit(instruments, out_path, src_path):
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "instrument.h"')
    lines.append('#include "chip/sid_filter.h"   // SID_FILT_LP/BP/HP')
    lines.append("")
    lines.append("// GENERATED FILE -- do not edit by hand.")
    lines.append(f"// Source: tools/{src_path.split('/')[-1] if '/' in src_path else src_path}")
    lines.append("// Regenerate: tools/chipgen.py gen <source.txt> <this file>")
    lines.append("// (module_chip.md §6, §11.2)")
    lines.append("")

    for ins in instruments:
        name = ins["name"]
        if ins["wave_rows"]:
            lines.append(f"static const WaveRow {name}_WAVE[] = {{")
            for w, n in ins["wave_rows"]:
                lines.append(f"    {wave_row_str(w, n)},")
            lines.append("};")
        if ins["pulse_rows"]:
            lines.append(f"static const SweepRow {name}_PULSE[] = {{")
            for d, du in ins["pulse_rows"]:
                lines.append(f"    {sweep_row_str(d, du)},")
            lines.append("};")
        if ins["filter_rows"]:
            lines.append(f"static const SweepRow {name}_FILTER[] = {{")
            for d, du in ins["filter_rows"]:
                lines.append(f"    {sweep_row_str(d, du)},")
            lines.append("};")
        lines.append("")

    lines.append("enum ChipInstrumentId : uint8_t {")
    for ins in instruments:
        lines.append(f"    INS_{ins['name']},")
    lines.append("    INSTRUMENT_COUNT,")
    lines.append("};")
    lines.append("")
    lines.append("static const char *const INSTRUMENT_NAMES[INSTRUMENT_COUNT] = {")
    for ins in instruments:
        lines.append(f"    \"{ins['name']}\",")
    lines.append("};")
    lines.append("")
    lines.append("static const Instrument INSTRUMENTS[INSTRUMENT_COUNT] = {")
    for ins in instruments:
        lines.append(f"    // INS_{ins['name']}")
        lines.append("    {")
        ad = (ins["ad"][0] << 4) | ins["ad"][1]
        sr = (ins["sr"][0] << 4) | ins["sr"][1]
        vd, vs, vdelay = ins["vibrato"]
        lines.append(f"        0x{ad:02x}, 0x{sr:02x}, 0,  {vd}, {vs}, {vdelay},  "
                      f"{ins['gate_off']},  0x{ins['first_wave']:x},")
        if ins["wave_rows"]:
            loop = ins["wave_loop"] if ins["wave_loop"] is not None else len(ins["wave_rows"]) - 1
            lines.append(f"        {{ {ins['name']}_WAVE, {len(ins['wave_rows'])}, {loop} }},")
        else:
            lines.append("        { nullptr, 0, 0 },")
        if ins["pulse_rows"]:
            loop = ins["pulse_loop"] if ins["pulse_loop"] is not None else len(ins["pulse_rows"]) - 1
            lines.append(f"        {{ {ins['name']}_PULSE, {len(ins['pulse_rows'])}, {loop} }}, {ins['pulse_init']},")
        else:
            lines.append(f"        {{ nullptr, 0, 0 }}, {ins['pulse_init']},")
        if ins["filter_rows"]:
            loop = ins["filter_loop"] if ins["filter_loop"] is not None else len(ins["filter_rows"]) - 1
            filter_table = f"{{ {ins['name']}_FILTER, {len(ins['filter_rows'])}, {loop} }}"
        else:
            filter_table = "{ nullptr, 0, 0 }"
        uses = "true" if ins["uses_filter"] else "false"
        lines.append(f"        {uses}, {ins['filter_cutoff']}, {ins['filter_resonance']}, "
                      f"0x{ins['filter_mode']:x}, {filter_table},")
        lines.append("    },")
    lines.append("};")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("gen")
    gen.add_argument("source")
    gen.add_argument("out")
    args = ap.parse_args()

    if args.cmd == "gen":
        try:
            instruments = parse_file(args.source)
        except GenError as e:
            print(f"chipgen.py: {e}", file=sys.stderr)
            return 1
        emit(instruments, args.out, args.source)
        print(f"chipgen.py: {len(instruments)} instruments -> {args.out}")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
