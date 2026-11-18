#!/usr/bin/env python3
"""Write the golden t00t SysEx byte vectors for the subtractive module.

    sysex_vectors.py <factory_bank.h> <out.txt>

An implementation of the SysEx framing and 7-bit packing that is
independent of the firmware's (src/patch/sysex.h), so a test comparing
against the file this writes (tools/host_render/test_sysex.cpp) doesn't
check the codec against itself.

Message layout: F0 7D 54 <command> <body> F7. Strings are a length byte
then 7-bit ASCII. 8-bit data (stored Patches)
is packed 7 bytes per 8: a byte holding the seven high bits (bit i = byte
i), then the seven bytes with their high bit cleared. Integers are 21-bit
two's complement, 7 bits per byte, least significant first.
"""

import re
import sys

MANUFACTURER = 0x7D
SUB_ID = 0x54
PROTOCOL_VERSION = 2

IDENTIFY_REQUEST = 0x01
IDENTIFY_REPLY = 0x02
PARAM_CHANGE = 0x10
MULTI_PARAM_CHANGE = 0x11
MULTI_EDIT_BUFFER_REQUEST = 0x22
MULTI_EDIT_BUFFER_DUMP = 0x23
EDIT_BUFFER_REQUEST = 0x20
EDIT_BUFFER_DUMP = 0x21
STORE_REQUEST = 0x30
STORE_REPLY = 0x31
MULTI_STORE_REQUEST = 0x32
MULTI_STORE_REPLY = 0x33
MULTI_LOAD_REQUEST = 0x34
MULTI_LOAD_REPLY = 0x35
DESCRIPTOR_DUMP_REQUEST = 0x40
DESCRIPTOR_TABLE_BEGIN = 0x41
DESCRIPTOR = 0x42
DESCRIPTOR_LABELS = 0x43
DESCRIPTOR_DUMP_END = 0x44

# Store results (src/patch/user_bank.h).
STORE_OK = 0
STORE_NO_SUCH_SLOT = 1
STORE_FLASH_FAILED = 3

TABLE_MODULE = 0
TABLE_COMMON = 1
TABLE_PART = 2
MULTI_ITSELF = 0x7F  # Part byte of a Multi parameter change for the Multi's own settings

# Multi load results (PatchLoadResult, src/patch/stored_format.h).
LOAD_OK = 0
LOAD_NO_SUCH_PATCH = 4
NO_CC = 0x7F

# Descriptor field codes (src/patch/descriptor.h).
UNIT = {"NONE": 0, "ENUM": 1, "HZ": 2, "CENTI_HZ": 3, "PERMILLE": 4}
CURVE = {"LINEAR": 0, "EXPONENTIAL": 1}
CLASS = {"LIVE": 0, "NOTE_ON": 1, "DATA_EXCHANGE": 2}

# The tables as an editor should see them: (name, group, min, max, default,
# unit, curve, class, quick-edit, default CC or None, labels or None).
SUBTRACTIVE_DESCRIPTORS = [
    ("WAVE", "OSC", 0, 7, 6, "ENUM", "LINEAR", "NOTE_ON", True, None,
     ["SINE", "SQUARE", "TRI", "SAW", "NOISE", "SQR-BLEP", "SAW-BLEP", "SAMPLE"]),
    ("SAMPLE", "OSC", 0, 8, 0, "ENUM", "LINEAR", "NOTE_ON", False, None,
     ["SARARR1", "MARIMBA", "LOWSTR5", "AH2", "ARR", "RRR", "SITAR2", "ZITHER2", "ELECPIAN"]),
    ("DUTY", "OSC", 0, 1023, 512, "NONE", "LINEAR", "LIVE", False, None, None),
    ("LFO RATE", "LFO", 0, 2000, 0, "CENTI_HZ", "EXPONENTIAL", "LIVE", True, 12, None),
    ("LFO>AMP", "LFO", 0, 1000, 0, "PERMILLE", "LINEAR", "LIVE", False, None, None),
    ("LFO>PTCH", "LFO", 0, 1000, 0, "PERMILLE", "LINEAR", "LIVE", False, None, None),
    ("LFO>PWM", "LFO", 0, 1000, 0, "PERMILLE", "LINEAR", "LIVE", False, None, None),
    ("LFO>CUT", "LFO", -18000, 18000, 0, "HZ", "LINEAR", "LIVE", False, None, None),
    ("FILTER", "FILTER", 0, 4, 0, "ENUM", "LINEAR", "NOTE_ON", False, None,
     ["OFF", "LP", "BP", "HP", "NOTCH"]),
    ("CUTOFF", "FILTER", 20, 18000, 8000, "HZ", "EXPONENTIAL", "LIVE", True, 74, None),
    ("RESO", "FILTER", 0, 32767, 0, "NONE", "LINEAR", "LIVE", True, 71, None),
    ("ENV>CUT", "FILTER", -18000, 18000, 0, "HZ", "LINEAR", "LIVE", True, 79, None),
]

COMMON_DESCRIPTORS = [
    ("FX TYPE", "FX", 0, 7, 1, "ENUM", "LINEAR", "LIVE", False, None,
     ["OFF", "DELAY", "REVERB", "PHASER", "FLANGER", "CHORUS", "CRUSH", "DRIVE"]),
    ("FX MIX", "FX", 0, 127, 0, "NONE", "LINEAR", "LIVE", False, None, None),
    ("FX P1", "FX", 0, 127, 55, "NONE", "LINEAR", "LIVE", False, None, None),
    ("FX P2", "FX", 0, 127, 36, "NONE", "LINEAR", "LIVE", False, None, None),
    ("MASTER", "MASTER", 0, 127, 127, "NONE", "LINEAR", "LIVE", False, None, None),
]

PART_DESCRIPTORS = [
    ("CHANNEL", "PART", 0, 16, 0, "ENUM", "LINEAR", "LIVE", False, None,
     [str(c) for c in range(1, 17)] + ["OFF"]),
    ("BANK", "PART", 0, 127, 0, "NONE", "LINEAR", "NOTE_ON", False, None, None),
    ("PROGRAM", "PART", 0, 127, 0, "NONE", "LINEAR", "NOTE_ON", False, None, None),
    ("VOLUME", "PART", 0, 127, 127, "NONE", "LINEAR", "LIVE", False, 7, None),
    ("PAN", "PART", 0, 127, 64, "NONE", "LINEAR", "LIVE", False, 10, None),
    ("FX SEND", "PART", 0, 127, 127, "NONE", "LINEAR", "LIVE", False, 91, None),
]
COMMON_FX_MIX = 1
PART_VOLUME = 3
PART_CHANNEL_OFF = 16
MULTI_SCHEMA_VERSION = 1

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


def text7(s):
    return [len(s), *s.encode("ascii")]


def id14(i):
    return [i & 0x7F, i >> 7]


def descriptor_dump():
    out = []
    for table, descs in ((TABLE_MODULE, SUBTRACTIVE_DESCRIPTORS), (TABLE_COMMON, COMMON_DESCRIPTORS),
                         (TABLE_PART, PART_DESCRIPTORS)):
        out += frame(DESCRIPTOR_TABLE_BEGIN, [table, *id14(len(descs))])
        for i, (name, group, lo, hi, default, unit, curve, cls, quick, cc, labels) in enumerate(descs):
            out += frame(DESCRIPTOR, [table, *id14(i), *int21(lo), *int21(hi), *int21(default),
                                      UNIT[unit], CURVE[curve], CLASS[cls], 1 if quick else 0,
                                      NO_CC if cc is None else cc, *text7(name), *text7(group)])
            if labels:
                out += frame(DESCRIPTOR_LABELS, [table, *id14(i), len(labels),
                                                 *[b for label in labels for b in text7(label)]])
    out += frame(DESCRIPTOR_DUMP_END)
    return out


def stored_multi(name, common, parts):
    """A stored Multi (src/patch/multi.h): the Patch header layout under magic
    T0MT, then common byte count, common values, Part count, Part byte
    count, Part values -- every value 0..127 here, one byte each."""
    payload = [len(common), *common, len(parts), len(parts[0]), *[v for part in parts for v in part]]
    name_bytes = list(name.encode("ascii"))[:16]
    header = [ord("T"), ord("0"), ord("M"), ord("T"), MODULE_SUBTRACTIVE, MULTI_SCHEMA_VERSION,
              *name_bytes, *([0] * (16 - len(name_bytes))), len(payload) & 0xFF, len(payload) >> 8]
    return header + payload


def default_multi_parts():
    """The default Multi's Parts: Part n on channel n, Patch Fairlite, Sq-PWM,
    then Saw-Flt (subtractive's default_program()), unity volume, centred
    pan, full send."""
    return [[p, 0, min(p, 2), 127, 64, 127] for p in range(PART_COUNT)]


def factory_patches(header_path):
    text = open(header_path).read()
    patches = {}
    for m in re.finditer(r"kSubFactoryPatch(\d+)\[\] = \{(.*?)\};", text, re.S):
        patches[int(m.group(1))] = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2))]
    return patches


def vectors(patches):
    fairlite, saw_flt = patches[0], patches[2]
    default_multi = stored_multi("INIT", [1, 0, 55, 36, 127], default_multi_parts())
    layer_parts = default_multi_parts()
    layer_parts[1][0] = 0
    layer_multi = stored_multi("LAYER", [2, 40, 55, 36, 127], layer_parts)
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
        # Store Part 0's Edit buffer into user slot 10, and the device's answers.
        ("store_request_part0_slot10", frame(STORE_REQUEST, [0, *id14(10)])),
        ("store_reply_part0_slot10_ok", frame(STORE_REPLY, [0, *id14(10), STORE_OK])),
        ("store_reply_part0_slot10_flash_failed", frame(STORE_REPLY, [0, *id14(10), STORE_FLASH_FAILED])),
        # Slot 200: past the 128-slot user bank.
        ("store_request_part0_slot200", frame(STORE_REQUEST, [0, *id14(200)])),
        ("store_reply_part0_slot200_no_such_slot", frame(STORE_REPLY, [0, *id14(200), STORE_NO_SUCH_SLOT])),
        # Part 16: the module has Parts 0-15 only, so no reply.
        ("store_request_part16_slot0", frame(STORE_REQUEST, [16, *id14(0)])),
        # Multi: FX MIX = 100, Part 2 VOLUME = 64.
        ("multi_param_change_fx_mix_100", frame(MULTI_PARAM_CHANGE, [MULTI_ITSELF, *id14(COMMON_FX_MIX), *int21(100)])),
        ("multi_param_change_part2_volume_64", frame(MULTI_PARAM_CHANGE, [2, *id14(PART_VOLUME), *int21(64)])),
        ("multi_edit_buffer_request", frame(MULTI_EDIT_BUFFER_REQUEST)),
        # The device's default Multi.
        ("multi_edit_buffer_dump_default", frame(MULTI_EDIT_BUFFER_DUMP, pack7(default_multi))),
        # "LAYER": Part 1 moved onto channel 1 with Part 0, reverb at mix 40.
        ("multi_edit_buffer_dump_layer", frame(MULTI_EDIT_BUFFER_DUMP, pack7(layer_multi))),
        ("multi_store_request_slot3", frame(MULTI_STORE_REQUEST, [*id14(3)])),
        ("multi_store_reply_slot3_ok", frame(MULTI_STORE_REPLY, [*id14(3), STORE_OK])),
        ("multi_load_request_slot3", frame(MULTI_LOAD_REQUEST, [*id14(3)])),
        ("multi_load_reply_slot3_ok", frame(MULTI_LOAD_REPLY, [*id14(3), LOAD_OK])),
        ("multi_load_request_slot5", frame(MULTI_LOAD_REQUEST, [*id14(5)])),
        ("multi_load_reply_slot5_empty", frame(MULTI_LOAD_REPLY, [*id14(5), LOAD_NO_SUCH_PATCH])),
        ("descriptor_dump_request", frame(DESCRIPTOR_DUMP_REQUEST)),
        # The device's whole answer, every message F0 ... F7 in order: for the
        # module table then the common table, table begin, then each
        # descriptor followed by its labels if it has any; then dump end.
        ("descriptor_dump", descriptor_dump()),
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
