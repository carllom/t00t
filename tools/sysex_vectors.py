#!/usr/bin/env python3
"""Write the golden t00t SysEx byte vectors for the subtractive module.

    sysex_vectors.py <factory_bank.h> <out.txt>

An implementation of the SysEx framing and 7-bit packing that is
independent of the firmware's (src/patch/sysex.h), so a test comparing
against the file this writes (tools/host_render/test_sysex.cpp) doesn't
check the codec against itself.

Message layout: F0 7D 54 <command> <body> F7. 8-bit data (stored Patches)
is packed 7 bytes per 8: a byte holding the seven high bits (bit i = byte
i), then the seven bytes with their high bit cleared. Integers are 21-bit
two's complement, 7 bits per byte, least significant first.
"""

import re
import sys

MANUFACTURER = 0x7D
SUB_ID = 0x54
PROTOCOL_VERSION = 1

IDENTIFY_REQUEST = 0x01
IDENTIFY_REPLY = 0x02
PARAM_CHANGE = 0x10
EDIT_BUFFER_REQUEST = 0x20
EDIT_BUFFER_DUMP = 0x21

# Subtractive module facts (src/engines/subtractive/patch.h).
MODULE_SUBTRACTIVE = 1
MODULE_FM = 3
STORED_MODULE_OFFSET = 4  # module ID byte in a stored Patch header (src/patch/stored_format.h)
SCHEMA_VERSION = 1
PART_COUNT = 16
PARAM_COUNT = 12
P_CUTOFF = 9
P_ENV_CUTOFF = 11


def frame(command, body=()):
    return [0xF0, MANUFACTURER, SUB_ID, command, *body, 0xF7]


def pack7(data):
    out = []
    for i in range(0, len(data), 7):
        chunk = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(chunk)))
        out.extend(b & 0x7F for b in chunk)
    return out


def int21(value):
    v = value & ((1 << 21) - 1)
    return [v & 0x7F, (v >> 7) & 0x7F, (v >> 14) & 0x7F]


def factory_patches(header_path):
    text = open(header_path).read()
    patches = {}
    for m in re.finditer(r"kSubFactoryPatch(\d+)\[\] = \{(.*?)\};", text, re.S):
        patches[int(m.group(1))] = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2))]
    return patches


def vectors(patches):
    fairlite, saw_flt = patches[0], patches[2]
    foreign = list(saw_flt)
    foreign[STORED_MODULE_OFFSET] = MODULE_FM
    return [
        ("identify_request", frame(IDENTIFY_REQUEST)),
        ("identify_reply", frame(IDENTIFY_REPLY, [MODULE_SUBTRACTIVE, SCHEMA_VERSION, PROTOCOL_VERSION,
                                                  PART_COUNT, PARAM_COUNT & 0x7F, PARAM_COUNT >> 7])),
        # Part 0, CUTOFF = 1234 Hz
        ("param_change_cutoff_1234_part0", frame(PARAM_CHANGE, [0, P_CUTOFF & 0x7F, P_CUTOFF >> 7, *int21(1234)])),
        # Part 3, ENV>CUT = -500 Hz
        ("param_change_env_cutoff_minus500_part3",
         frame(PARAM_CHANGE, [3, P_ENV_CUTOFF & 0x7F, P_ENV_CUTOFF >> 7, *int21(-500)])),
        ("edit_buffer_request_part0", frame(EDIT_BUFFER_REQUEST, [0])),
        ("edit_buffer_request_part1", frame(EDIT_BUFFER_REQUEST, [1])),
        # What the device sends for Part 0 holding factory Patch 0 (Fairlite).
        ("edit_buffer_dump_fairlite_part0", frame(EDIT_BUFFER_DUMP, [0, *pack7(fairlite)])),
        # Factory Patch 2 (Saw-Flt) sent to Part 1.
        ("edit_buffer_dump_saw_flt_part1", frame(EDIT_BUFFER_DUMP, [1, *pack7(saw_flt)])),
        # The same Patch claiming to be an FM Patch: must be rejected.
        ("edit_buffer_dump_fm_module_part1", frame(EDIT_BUFFER_DUMP, [1, *pack7(foreign)])),
        # Saw-Flt with its last two payload bytes cut off: shorter than its
        # header's payload length, must be rejected.
        ("edit_buffer_dump_truncated_part1", frame(EDIT_BUFFER_DUMP, [1, *pack7(saw_flt[:-2])])),
        # A high-bits byte with no data after it: malformed, must be ignored.
        ("edit_buffer_dump_malformed_part1", frame(EDIT_BUFFER_DUMP, [1, 0x00])),
    ]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[2], "w") as f:
        f.write("# t00t SysEx golden vectors, subtractive module -- written by tools/sysex_vectors.py\n")
        f.write("# <name> <bytes in hex, F0 ... F7>\n")
        for name, data in vectors(factory_patches(sys.argv[1])):
            f.write(name + " " + " ".join(f"{b:02X}" for b in data) + "\n")


if __name__ == "__main__":
    main()
