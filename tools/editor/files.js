// Files the editor reads and writes: a single Patch, and a whole user bank.
// Pure functions over byte arrays.
//
// A Patch file (.t0p) is one stored Patch (src/patch/stored_format.h) as it
// is: the header, then the payload.
//
// A bank file (.t0b) holds a module's user bank as a bank dump carries it:
//
//   offset  size  field
//   0       4     magic "T0BK"
//   4       1     format version (1)
//   5       1     module ID
//   6       2     Patch slot count, little-endian
//   8       2     Multi slot count, little-endian
//   10      ...   each Patch slot, then each Multi slot: its length (2 bytes,
//                 little-endian; 0 for an empty slot), then the stored Patch
//                 or Multi, byte for byte as in the slot without its padding
//
// A classic script (see sysex.js): publishes globalThis.t00t.files.

(function (t00t) {
"use strict";

const BANK_MAGIC = [0x54, 0x30, 0x42, 0x4b];  // "T0BK"
const BANK_FORMAT = 1;
const BANK_HEADER_SIZE = 10;

const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];

// { module, patches, multis } (stored bytes or null per slot) as a bank file.
function encodeBank({ module, patches, multis }) {
    const out = [...BANK_MAGIC, BANK_FORMAT, module, ...u16(patches.length), ...u16(multis.length)];
    for (const stored of [...patches, ...multis]) {
        out.push(...u16(stored ? stored.length : 0));
        if (stored) out.push(...stored);
    }
    return Uint8Array.from(out);
}

// A bank file as { module, patches, multis }, or { error }.
function decodeBank(file) {
    if (file.length < BANK_HEADER_SIZE || BANK_MAGIC.some((b, i) => file[i] !== b)) {
        return { error: "not a t00t bank file" };
    }
    if (file[4] !== BANK_FORMAT) return { error: `bank file format ${file[4]}; this editor reads format ${BANK_FORMAT}` };
    const counts = [file[6] | (file[7] << 8), file[8] | (file[9] << 8)];
    let at = BANK_HEADER_SIZE;
    const [patches, multis] = counts.map((count) => {
        const slots = [];
        for (let i = 0; i < count; i++) {
            if (at + 2 > file.length) return null;
            const len = file[at] | (file[at + 1] << 8);
            at += 2;
            if (at + len > file.length) return null;
            slots.push(len ? file.slice(at, at + len) : null);
            at += len;
        }
        return slots;
    });
    if (!patches || !multis) return { error: "truncated" };
    if (at !== file.length) return { error: "trailing bytes" };
    return { module: file[5], patches, multis };
}

t00t.files = { encodeBank, decodeBank };
})(globalThis.t00t ??= {});
