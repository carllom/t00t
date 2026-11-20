// DX7 .syx import: a 32-voice bulk dump or a single-voice dump as stored FM
// Patches, byte for byte as tools/syx2patch.py converts them for the FM
// factory banks (test/dx7.test.js checks both against the same fixtures).
// Pure functions over byte arrays.
//
// A classic script (see sysex.js), loaded after sysex.js: publishes
// globalThis.t00t.dx7.

(function (t00t) {
"use strict";

const sx = t00t.sysex;
const MODULE_FM = 3;         // ModuleId::FM, src/patch/module_id.h
const FM_SCHEMA_VERSION = 1; // FmModule::kSchemaVersion

const VOICE_SIZE = 155;      // one unpacked voice, as a single-voice dump carries it
const PACKED_SIZE = 128;     // one voice in a bulk dump
const BANK_VOICES = 32;
const BANK_DUMP_SIZE = 6 + BANK_VOICES * PACKED_SIZE + 2;
const VOICE_DUMP_SIZE = 6 + VOICE_SIZE + 2;

// The unpacked voice's fields with the largest value each may hold: per
// operator (OP6 first), then the voice. EG bytes and fine frequency may use
// the whole 7-bit byte, as real factory banks do; the rest keep to the
// DX7's own ranges.
const OP_FIELDS = [
    ["R1", 127], ["R2", 127], ["R3", 127], ["R4", 127], ["L1", 127], ["L2", 127], ["L3", 127], ["L4", 127],
    ["break point", 99], ["left depth", 99], ["right depth", 99], ["left curve", 3], ["right curve", 3],
    ["rate scaling", 7], ["AMS", 3], ["velocity", 7], ["level", 99], ["mode", 1], ["coarse", 31],
    ["fine", 127], ["detune", 14],
];
const VOICE_FIELDS = [
    ["pitch EG R1", 99], ["pitch EG R2", 99], ["pitch EG R3", 99], ["pitch EG R4", 99],
    ["pitch EG L1", 99], ["pitch EG L2", 99], ["pitch EG L3", 99], ["pitch EG L4", 99],
    ["algorithm", 31], ["feedback", 7], ["oscillator sync", 1], ["LFO speed", 99], ["LFO delay", 99],
    ["LFO PMD", 99], ["LFO AMD", 99], ["LFO sync", 1], ["LFO wave", 5], ["LFO PMS", 7], ["transpose", 48],
];
const OP_SIZE = OP_FIELDS.length;
const NAME_AT = 6 * OP_SIZE + VOICE_FIELDS.length;

// A packed bulk-dump voice in the unpacked layout (the DX7 MIDI Data
// Format Sheet; tools/syx2patch.py vced_from_packed()).
function unpackVoice(b) {
    const v = [];
    for (let j = 0; j < 6; j++) {
        const p = j * 17;
        const lrCurve = b[p + 11] & 0x0f, detRs = b[p + 12], kvsAms = b[p + 13] & 0x1f, coarseMode = b[p + 15] & 0x3f;
        v.push(...b.subarray(p, p + 11),
               lrCurve & 3, (lrCurve >> 2) & 3, detRs & 7, kvsAms & 3, (kvsAms >> 2) & 7, b[p + 14],
               coarseMode & 1, (coarseMode >> 1) & 0x1f, b[p + 16], (detRs >> 3) & 0x0f);
    }
    const oksFb = b[111] & 0x0f, lfo = b[116];
    v.push(...b.subarray(102, 110), b[110] & 0x1f, oksFb & 7, (oksFb >> 3) & 1, ...b.subarray(112, 116),
           lfo & 1, (lfo >> 1) & 7, (lfo >> 4) & 7, b[117], ...b.subarray(118, 128));
    return Uint8Array.from(v);
}

// The first field of an unpacked voice out of range, in words, or null.
function badField(v) {
    for (let j = 0; j < 6; j++) {
        const at = OP_FIELDS.findIndex(([, max], i) => v[j * OP_SIZE + i] > max);
        if (at >= 0) return `OP${6 - j} ${OP_FIELDS[at][0]} is ${v[j * OP_SIZE + at]} (at most ${OP_FIELDS[at][1]})`;
    }
    const at = VOICE_FIELDS.findIndex(([, max], i) => v[6 * OP_SIZE + i] > max);
    return at < 0 ? null : `${VOICE_FIELDS[at][0]} is ${v[6 * OP_SIZE + at]} (at most ${VOICE_FIELDS[at][1]})`;
}

// An unpacked voice as { name, stored }: FmModule's parameters in ID order
// (src/engines/fm/fm_module.h), OP1's block first, each operator enabled,
// then the voice's, one byte each.
function convertVoice(v) {
    const values = [];
    for (let n = 1; n <= 6; n++) values.push(...v.subarray((6 - n) * OP_SIZE, (7 - n) * OP_SIZE), 1);
    values.push(...v.subarray(6 * OP_SIZE, NAME_AT));
    // Trailing whitespace dropped, as Python's str.rstrip() drops it.
    const name = String.fromCharCode(...v.subarray(NAME_AT, NAME_AT + 10)).replace(/[\t-\r\x1c-\x20]+$/, "");
    const nameBytes = Array.from(name, (c) => c.charCodeAt(0));
    while (nameBytes.length < sx.NAME_LEN) nameBytes.push(0);
    const stored = Uint8Array.from([...sx.PATCH_MAGIC, MODULE_FM, FM_SCHEMA_VERSION, ...nameBytes,
                                    values.length & 0xff, values.length >> 8, ...values]);
    return { name, stored };
}

const checksum = (data) => -data.reduce((a, b) => a + b, 0) & 0x7f;

// The `count` data bytes of a dump that should be `size` bytes long, after
// its length, byte count, end and checksum checks, or { error } naming the
// dump as `kind`.
function unwrap(syx, size, count, kind) {
    if (syx.length !== size) return { error: `A DX7 ${kind} dump is ${size} bytes; this file is ${syx.length}.` };
    if (((syx[4] << 7) | syx[5]) !== count) {
        return { error: `The ${kind} dump's byte count is ${(syx[4] << 7) | syx[5]}, not ${count}.` };
    }
    if (syx[size - 1] !== 0xf7) return { error: `The ${kind} dump doesn't end with F7.` };
    const data = syx.subarray(6, 6 + count);
    if (checksum(data) !== syx[size - 2]) return { error: `The ${kind} dump's checksum is wrong: corrupted or truncated.` };
    return { data: data.map((b) => b & 0x7f) };
}

// A .syx file (a DX7 32-voice bulk dump or single-voice dump) as
// { voices: [{ name, stored }] }, the stored Patches for the FM module, or
// { error } saying why it can't be imported.
function parseSyx(syx) {
    if (syx.length < 6 || syx[0] !== 0xf0 || syx[1] !== 0x43 || (syx[2] >> 4) & 7) {
        return { error: "This is not a DX7 voice or bank dump (.syx)." };
    }
    let voices;
    if (syx[3] === 9) {
        const { data, error } = unwrap(syx, BANK_DUMP_SIZE, BANK_VOICES * PACKED_SIZE, "32-voice");
        if (error) return { error };
        voices = Array.from({ length: BANK_VOICES },
                            (_, i) => unpackVoice(data.subarray(i * PACKED_SIZE, (i + 1) * PACKED_SIZE)));
    } else if (syx[3] === 0) {
        const { data, error } = unwrap(syx, VOICE_DUMP_SIZE, VOICE_SIZE, "single-voice");
        if (error) return { error };
        voices = [data];
    } else {
        return { error: `This is a DX7 dump of format ${syx[3]}; only voice (0) and 32-voice (9) dumps import.` };
    }
    for (let i = 0; i < voices.length; i++) {
        const bad = badField(voices[i]);
        if (bad) return { error: `${voices.length > 1 ? `Voice ${i + 1}: ` : ""}${bad}. Not imported: corrupted data?` };
    }
    return { voices: voices.map(convertVoice) };
}

t00t.dx7 = { parseSyx, MODULE_FM };
})(globalThis.t00t ??= {});
