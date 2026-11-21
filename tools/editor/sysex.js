// The editor's side of the t00t SysEx protocol (firmware: src/patch/sysex.h)
// and the descriptor vocabulary it carries. Pure functions over byte
// arrays: no Web MIDI, no DOM.
//
// A classic script, not an ES module, so the editor runs from file:// (where
// Chromium refuses to load module scripts): it publishes its API as
// globalThis.t00t.sysex.
//
// Every message is F0 7D 54 <command> <body> F7.

(function (t00t) {
"use strict";

const MANUFACTURER = 0x7d;
const SUB_ID = 0x54;
const PROTOCOL_VERSION = 2;

// Module IDs (ModuleId, src/patch/module_id.h).
const MODULE_NAMES = { 1: "subtractive", 2: "groovebox", 3: "FM", 4: "OPL", 5: "chip", 6: "speech", 7: "tracker" };

// Descriptor field codes (src/patch/descriptor.h).
const Unit = Object.freeze({ NONE: 0, ENUM: 1, HZ: 2, CENTI_HZ: 3, PERMILLE: 4, BITS: 5 });
const Curve = Object.freeze({ LINEAR: 0, EXPONENTIAL: 1 });
const ParamClass = Object.freeze({ LIVE: 0, NOTE_ON: 1, DATA_EXCHANGE: 2 });

const Command = Object.freeze({
    IDENTIFY_REQUEST: 0x01,
    IDENTIFY_REPLY: 0x02,
    PARAM_CHANGE: 0x10,
    EDIT_BUFFER_REQUEST: 0x20,
    EDIT_BUFFER_DUMP: 0x21,
    STORE_REQUEST: 0x30,
    STORE_REPLY: 0x31,
    BANK_DUMP_REQUEST: 0x36,
    BANK_SLOT: 0x37,
    BANK_SLOT_REPLY: 0x38,
    BANK_DUMP_END: 0x39,
    DESCRIPTOR_DUMP_REQUEST: 0x40,
    DESCRIPTOR_TABLE_BEGIN: 0x41,
    DESCRIPTOR: 0x42,
    DESCRIPTOR_LABELS: 0x43,
    DESCRIPTOR_DUMP_END: 0x44,
});

function frame(command, body = []) {
    return Uint8Array.from([0xf0, MANUFACTURER, SUB_ID, command, ...body, 0xf7]);
}

// Integers: 21-bit two's complement over three 7-bit bytes, least significant first.
function int21(value) {
    const v = value & 0x1fffff;
    return [v & 0x7f, (v >> 7) & 0x7f, (v >> 14) & 0x7f];
}

function readInt21(b, at) {
    const v = b[at] | (b[at + 1] << 7) | (b[at + 2] << 14);
    return v & 0x100000 ? v - 0x200000 : v;
}

// IDs and counts: 2 x 7 bits, least significant first.
function id14(id) {
    return [id & 0x7f, (id >> 7) & 0x7f];
}

function readId14(b, at) {
    return b[at] | (b[at + 1] << 7);
}

function paramChange(part, id, value) {
    return frame(Command.PARAM_CHANGE, [part, ...id14(id), ...int21(value)]);
}

function identifyRequest() {
    return frame(Command.IDENTIFY_REQUEST);
}

function editBufferRequest(part) {
    return frame(Command.EDIT_BUFFER_REQUEST, [part]);
}

// Store Part `part`'s Edit buffer into user slot `slot`.
function storeRequest(part, slot) {
    return frame(Command.STORE_REQUEST, [part, ...id14(slot)]);
}

// User Patch slots per module (kUserPatchSlots, src/patch/user_bank.h).
const USER_PATCH_SLOTS = 128;

// The two kinds of user-bank slot (SlotKind, src/patch/user_bank.h).
const SlotKind = Object.freeze({ PATCH: 0, MULTI: 1 });

// Ask for the whole user bank: every Patch slot, then every Multi slot.
function bankDumpRequest() {
    return frame(Command.BANK_DUMP_REQUEST);
}

// Put `stored` (a stored Patch or Multi) into user-bank slot `slot` of
// `kind`, byte for byte; null empties the slot.
function bankSlot(kind, slot, stored) {
    return frame(Command.BANK_SLOT, [kind, ...id14(slot), ...(stored ? pack(Array.from(stored)) : [])]);
}

// Packed 8-bit data: groups of up to 7 bytes, each led by a byte holding
// their high bits (bit i = byte i of the group), then the bytes with their
// high bit cleared.
function pack(data) {
    const out = [];
    for (let i = 0; i < data.length; i += 7) {
        const group = data.slice(i, i + 7);
        out.push(group.reduce((high, byte, j) => high | (((byte >> 7) & 1) << j), 0));
        for (const byte of group) out.push(byte & 0x7f);
    }
    return out;
}

// Packed data back to bytes, or null if malformed (a high-bits byte with no
// data after it).
function unpack(packed) {
    const out = [];
    for (let i = 0; i < packed.length; i += 8) {
        const group = Math.min(7, packed.length - i - 1);
        if (group <= 0) return null;
        for (let j = 0; j < group; j++) out.push(packed[i + 1 + j] | (((packed[i] >> j) & 1) << 7));
    }
    return Uint8Array.from(out);
}

const NO_CC = 0x7f;

// Strings: a length byte, then 7-bit ASCII. Returns [string, next offset],
// or null if it runs past the end.
function readString(b, at) {
    if (at >= b.length || at + 1 + b[at] > b.length) return null;
    return [String.fromCharCode(...b.subarray(at + 1, at + 1 + b[at])), at + 1 + b[at]];
}

// table, ID, min, max, default, unit, curve, class, flags, default CC, name, group
function parseDescriptor(b) {
    if (b.length < 17) return null;
    const name = readString(b, 17);
    const group = name && readString(b, name[1]);
    if (!group || group[1] !== b.length) return null;
    return {
        type: "descriptor", table: b[0], id: readId14(b, 1),
        min: readInt21(b, 3), max: readInt21(b, 6), def: readInt21(b, 9),
        unit: b[12], curve: b[13], cls: b[14], quickEdit: (b[15] & 1) !== 0,
        cc: b[16] === NO_CC ? null : b[16], name: name[0], group: group[0],
    };
}

// table, ID, label count, labels
function parseLabels(b) {
    if (b.length < 4) return null;
    const labels = [];
    let at = 4;
    for (let i = 0; i < b[3]; i++) {
        const s = readString(b, at);
        if (!s) return null;
        labels.push(s[0]);
        at = s[1];
    }
    if (at !== b.length) return null;
    return { type: "labels", table: b[0], id: readId14(b, 1), labels };
}

// A BITS value as the device shows it (param_format(), src/patch/descriptor.h):
// the set bits' names joined by "+", "-" for none.
function bitsText(labels, value) {
    const set = labels.filter((_, bit) => (value >> bit) & 1);
    return set.length ? set.join("+") : "-";
}

// A byte stream holding whole SysEx messages, cut into one array per message.
function splitSysEx(bytes) {
    const out = [];
    let start = -1;
    bytes.forEach((byte, i) => {
        if (byte === 0xf0) start = i;
        else if (byte === 0xf7 && start >= 0) {
            out.push(bytes.subarray(start, i + 1));
            start = -1;
        }
    });
    return out;
}

// Store results (StoreResult, src/patch/user_bank.h).
const STORE_OK = 0;
const STORE_RESULTS = ["OK", "no such slot", "bad Patch", "flash write failed"];

function storeResultText(result) {
    return STORE_RESULTS[result] ?? `error ${result}`;
}

// A device message (one whole F0 ... F7) as an object with a `type`, or null
// if it isn't a well-formed t00t message this editor reads.
function parse(msg) {
    const n = msg.length;
    if (n < 5 || msg[0] !== 0xf0 || msg[n - 1] !== 0xf7) return null;
    if (msg[1] !== MANUFACTURER || msg[2] !== SUB_ID) return null;
    const b = msg.subarray(4, n - 1);
    switch (msg[3]) {
        case Command.IDENTIFY_REPLY:
            if (b.length !== 6) return null;
            return { type: "identify", module: b[0], schema: b[1], protocol: b[2], partCount: b[3],
                     paramCount: readId14(b, 4) };
        case Command.EDIT_BUFFER_DUMP: {
            const stored = b.length >= 2 ? unpack(b.subarray(1)) : null;
            return stored ? { type: "editBuffer", part: b[0], stored } : null;
        }
        case Command.STORE_REPLY:
            if (b.length !== 4) return null;
            return { type: "storeReply", part: b[0], slot: readId14(b, 1), result: b[3] };
        case Command.BANK_SLOT: {
            if (b.length < 3 || b[0] > SlotKind.MULTI) return null;
            const stored = b.length > 3 ? unpack(b.subarray(3)) : null;
            if (b.length > 3 && !stored) return null;
            return { type: "bankSlot", kind: b[0], slot: readId14(b, 1), stored };
        }
        case Command.BANK_SLOT_REPLY:
            if (b.length !== 4) return null;
            return { type: "bankSlotReply", kind: b[0], slot: readId14(b, 1), result: b[3] };
        case Command.BANK_DUMP_END:
            if (b.length !== 4) return null;
            return { type: "bankDumpEnd", patchSlots: readId14(b, 0), multiSlots: readId14(b, 2) };
        case Command.DESCRIPTOR_TABLE_BEGIN:
            if (b.length !== 3) return null;
            return { type: "tableBegin", table: b[0], count: readId14(b, 1) };
        case Command.DESCRIPTOR:
            return parseDescriptor(b);
        case Command.DESCRIPTOR_LABELS:
            return parseLabels(b);
        case Command.DESCRIPTOR_DUMP_END:
            return b.length === 0 ? { type: "dumpEnd" } : null;
        default:
            return null;
    }
}

function descriptorDumpRequest() {
    return frame(Command.DESCRIPTOR_DUMP_REQUEST);
}

const TABLE_KEYS = ["module", "common", "part"];

// The parsed messages of a whole descriptor dump as { module, common, part }:
// each an array of descriptors in ID order, ENUM labels merged in (`labels`,
// or null). Null if a table is missing or short of the count it began with.
function assembleDump(messages) {
    const tables = {};
    const counts = {};
    for (const m of messages) {
        const key = TABLE_KEYS[m?.table];
        if (m?.type === "tableBegin" && key) {
            counts[key] = m.count;
            tables[key] = [];
        } else if (m?.type === "descriptor" && tables[key]) {
            const { type, table, ...d } = m;
            tables[key][m.id] = { ...d, labels: null };
        } else if (m?.type === "labels" && tables[key]?.[m.id]) {
            tables[key][m.id].labels = m.labels;
        }
    }
    for (const key of TABLE_KEYS) {
        const t = tables[key];
        if (!t || t.length !== counts[key] || t.some((d) => !d)) return null;
    }
    return tables;
}

// The stored Patch format (src/patch/stored_format.h): a 24-byte header --
// magic "T0PT", module ID, schema version, 16-byte NUL-padded name, payload
// length (2 bytes, little-endian) -- then each parameter in ID order as
// (value - min) in one byte, or two little-endian if its range exceeds 255.
const PATCH_MAGIC = [0x54, 0x30, 0x50, 0x54];
const HEADER_SIZE = 24;
const NAME_LEN = 16;

function storedWidth(d) {
    return d.max - d.min <= 0xff ? 1 : 2;
}

// A stored Patch as { module, schema, name, values } for the given module
// descriptors, or { error }. Parameters past the end of an older schema's
// payload take their default, from the first one that doesn't fit whole.
function decodePatch(stored, descriptors) {
    if (stored.length < HEADER_SIZE) return { error: "truncated" };
    if (PATCH_MAGIC.some((b, i) => stored[i] !== b)) return { error: "not a Patch" };
    const payloadLen = stored[22] | (stored[23] << 8);
    if (stored.length < HEADER_SIZE + payloadLen) return { error: "truncated" };
    const nameBytes = Array.from(stored.subarray(6, 6 + NAME_LEN));
    const nul = nameBytes.indexOf(0);
    const name = String.fromCharCode(...(nul < 0 ? nameBytes : nameBytes.slice(0, nul)));
    let at = HEADER_SIZE;
    let end = HEADER_SIZE + payloadLen;
    const values = descriptors.map((d) => {
        const w = storedWidth(d);
        if (at + w > end) {
            end = at;
            return d.def;
        }
        const raw = w === 1 ? stored[at] : stored[at] | (stored[at + 1] << 8);
        at += w;
        return Math.min(d.min + raw, d.max);
    });
    return { module: stored[4], schema: stored[5], name, values };
}

// { module, schema, name, values } as a stored Patch. The name is cut to
// NAME_LEN characters; any outside printable ASCII is stored as "?".
function encodePatch(patch, descriptors) {
    const payload = [];
    descriptors.forEach((d, i) => {
        const raw = patch.values[i] - d.min;
        payload.push(raw & 0xff);
        if (storedWidth(d) === 2) payload.push((raw >> 8) & 0xff);
    });
    const name = Array.from(patch.name).slice(0, NAME_LEN).map((c) => {
        const code = c.codePointAt(0);
        return code >= 0x20 && code < 0x7f ? code : 0x3f;
    });
    while (name.length < NAME_LEN) name.push(0);
    return Uint8Array.from([...PATCH_MAGIC, patch.module, patch.schema, ...name,
                            payload.length & 0xff, payload.length >> 8, ...payload]);
}

// Send a stored Patch to Part `part`'s Edit buffer.
function editBufferDump(part, stored) {
    return frame(Command.EDIT_BUFFER_DUMP, [part, ...pack(Array.from(stored))]);
}

t00t.sysex = {
    PROTOCOL_VERSION, MODULE_NAMES, NAME_LEN, PATCH_MAGIC, Unit, Curve, ParamClass, SlotKind, STORE_OK, USER_PATCH_SLOTS,
    identifyRequest, descriptorDumpRequest, editBufferRequest, paramChange, editBufferDump, storeRequest,
    bankDumpRequest, bankSlot,
    parse, splitSysEx, assembleDump, decodePatch, encodePatch, storeResultText, bitsText,
};
})(globalThis.t00t ??= {});
