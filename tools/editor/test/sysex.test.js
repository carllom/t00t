// The editor's SysEx codec against the firmware's golden byte vectors
// (tools/host_render/golden/sysex_subtractive.txt, written by
// tools/sysex_vectors.py), so editor and firmware can't drift apart.

import { test, eq, ok } from "./harness.js";
import { golden } from "./golden.js";
const sx = globalThis.t00t.sysex;

test("codec: identify request", async () => {
    const g = await golden();
    eq(sx.identifyRequest(), g.identify_request, "identify request bytes");
});

test("codec: parameter change, positive and negative values", async () => {
    const g = await golden();
    eq(sx.paramChange(0, 9, 1234), g.param_change_cutoff_1234_part0, "Part 0 CUTOFF = 1234");
    eq(sx.paramChange(3, 11, -500), g.param_change_env_cutoff_minus500_part3, "Part 3 ENV>CUT = -500");
});

test("codec: edit-buffer request", async () => {
    const g = await golden();
    eq(sx.editBufferRequest(0), g.edit_buffer_request_part0, "Part 0");
    eq(sx.editBufferRequest(1), g.edit_buffer_request_part1, "Part 1");
});

test("codec: store request, slot past 127", async () => {
    const g = await golden();
    eq(sx.storeRequest(0, 10), g.store_request_part0_slot10, "Part 0 into slot 10");
    eq(sx.storeRequest(0, 200), g.store_request_part0_slot200, "Part 0 into slot 200");
});

test("codec: identify reply", async () => {
    const g = await golden();
    eq(sx.parse(g.identify_reply),
       { type: "identify", module: 1, schema: 1, protocol: 2, partCount: 16, paramCount: 12 },
       "subtractive, schema 1, protocol 2, 16 Parts, 12 parameters");
});

test("codec: other manufacturers' SysEx and non-SysEx are not t00t messages", () => {
    eq(sx.parse(Uint8Array.from([0xf0, 0x43, 0x10, 0x01, 0xf7])), null, "Yamaha SysEx");
    eq(sx.parse(Uint8Array.from([0x90, 60, 100])), null, "a note on");
    eq(sx.parse(Uint8Array.from([0xf0, 0x7d, 0x55, 0x02, 0xf7])), null, "another sub-ID");
});

test("codec: store replies", async () => {
    const g = await golden();
    eq(sx.parse(g.store_reply_part0_slot10_ok), { type: "storeReply", part: 0, slot: 10, result: 0 }, "OK");
    eq(sx.parse(g.store_reply_part0_slot10_flash_failed), { type: "storeReply", part: 0, slot: 10, result: 3 },
       "flash write failed");
    eq(sx.parse(g.store_reply_part0_slot200_no_such_slot), { type: "storeReply", part: 0, slot: 200, result: 1 },
       "no such slot");
    eq(sx.storeResultText(1), "no such slot", "result 1 in words");
});

test("codec: descriptor dump messages", async () => {
    const g = await golden();
    const msgs = sx.splitSysEx(g.descriptor_dump).map(sx.parse);
    ok(msgs.every((m) => m !== null), "every message of the dump decodes");
    eq(msgs[0], { type: "tableBegin", table: 0, count: 12 }, "module table begins, 12 descriptors");
    eq(msgs.filter((m) => m.type === "tableBegin").map((m) => [m.table, m.count]), [[0, 12], [1, 5], [2, 6]],
       "module, common and Part tables");
    eq(msgs.find((m) => m.type === "descriptor" && m.table === 0 && m.id === 9),
       { type: "descriptor", table: 0, id: 9, min: 20, max: 18000, def: 8000, unit: 2, curve: 1, cls: 0,
         quickEdit: true, cc: 74, name: "CUTOFF", group: "FILTER" },
       "CUTOFF");
    eq(msgs.find((m) => m.type === "descriptor" && m.table === 0 && m.id === 7),
       { type: "descriptor", table: 0, id: 7, min: -18000, max: 18000, def: 0, unit: 2, curve: 0, cls: 0,
         quickEdit: false, cc: null, name: "LFO>CUT", group: "LFO" },
       "LFO>CUT: negative min, no default CC");
    eq(msgs.find((m) => m.type === "labels" && m.table === 0 && m.id === 8),
       { type: "labels", table: 0, id: 8, labels: ["OFF", "LP", "BP", "HP", "NOTCH"] }, "FILTER labels");
    eq(msgs.find((m) => m.type === "labels" && m.table === 2 && m.id === 0).labels.at(-1), "OFF",
       "the Part CHANNEL labels end with OFF");
    eq(msgs.at(-1), { type: "dumpEnd" }, "dump end last");
});

test("codec: descriptor dump request", async () => {
    const g = await golden();
    eq(sx.descriptorDumpRequest(), g.descriptor_dump_request, "request bytes");
});

test("codec: edit-buffer dump carries the stored Patch, unpacked", async () => {
    const g = await golden();
    const m = sx.parse(g.edit_buffer_dump_fairlite_part0);
    eq(m.type, "editBuffer", "type");
    eq(m.part, 0, "Part 0");
    eq(Array.from(m.stored.subarray(0, 6)), [0x54, 0x30, 0x50, 0x54, 1, 1], "header: T0PT, subtractive, schema 1");
    eq(String.fromCharCode(...m.stored.subarray(6, 14)), "Fairlite", "name");
    eq(m.stored.length, 24 + 21, "24-byte header plus subtractive's 21 payload bytes");
    eq(sx.parse(g.edit_buffer_dump_malformed_part1), null, "a high-bits byte with no data is malformed");
});

async function subtractiveTables() {
    const g = await golden();
    return sx.assembleDump(sx.splitSysEx(g.descriptor_dump).map(sx.parse));
}

test("codec: a descriptor dump assembles into the module, common and Part tables", async () => {
    const t = await subtractiveTables();
    eq([t.module.length, t.common.length, t.part.length], [12, 5, 6], "table sizes");
    eq(t.module[0].labels.slice(0, 2), ["SINE", "SQUARE"], "WAVE carries its labels");
    eq(t.module[2].labels, null, "DUTY has no labels");
    eq(t.common[4].name, "MASTER", "common table");
    eq(sx.assembleDump([{ type: "tableBegin", table: 0, count: 2 },
                        { type: "descriptor", table: 0, id: 0, name: "X" }, { type: "dumpEnd" }]),
       null, "a table missing a descriptor its begin counted is incomplete");
});

test("codec: stored Patch decodes into values in descriptor order", async () => {
    const g = await golden();
    const t = await subtractiveTables();
    eq(sx.decodePatch(sx.parse(g.edit_buffer_dump_fairlite_part0).stored, t.module),
       { module: 1, schema: 1, name: "Fairlite", values: [7, 0, 512, 0, 0, 0, 0, 0, 1, 400, 16000, 8000] },
       "Fairlite: SAMPLE wave, LP 400 Hz, reso 16000, env 8000");
    eq(sx.decodePatch(sx.parse(g.edit_buffer_dump_saw_flt_part1).stored, t.module).values,
       [6, 0, 512, 200, 0, 0, 0, 2000, 1, 200, 24000, 0], "Saw-Flt: LFO 2 Hz onto cutoff by 2000 Hz");
    eq(sx.decodePatch(sx.parse(g.edit_buffer_dump_fm_module_part1).stored, t.module).module, 3,
       "an FM Patch decodes, saying it is for module 3");
    eq(sx.decodePatch(sx.parse(g.edit_buffer_dump_truncated_part1).stored, t.module), { error: "truncated" },
       "shorter than its payload length");
    eq(sx.decodePatch(Uint8Array.from("T0MT".split("").map((c) => c.charCodeAt(0)).concat(Array(30).fill(0))),
                      t.module), { error: "not a Patch" }, "a stored Multi is not a Patch");
});

test("codec: a Patch from an older schema takes defaults for the parameters it lacks", async () => {
    const g = await golden();
    const t = await subtractiveTables();
    const stored = sx.parse(g.edit_buffer_dump_fairlite_part0).stored.slice(0, 24 + 19);  // up to RESO's LSB
    stored[22] = 19;
    eq(sx.decodePatch(stored, t.module).values.slice(9), [400, 16000, 0], "ENV>CUT takes its default, 0");
});

test("codec: values encode into the stored Patch and an edit-buffer dump", async () => {
    const g = await golden();
    const t = await subtractiveTables();
    const patch = { module: 1, schema: 1, name: "Saw-Flt", values: [6, 0, 512, 200, 0, 0, 0, 2000, 1, 200, 24000, 0] };
    eq(sx.editBufferDump(1, sx.encodePatch(patch, t.module)), g.edit_buffer_dump_saw_flt_part1,
       "Saw-Flt to Part 1");
});

test("codec: a payload cut inside a parameter gives that one and every later one its default", () => {
    const table = [{ min: 0, max: 1000, def: 5 }, { min: 0, max: 10, def: 3 }];  // 2 bytes, then 1
    const header = [0x54, 0x30, 0x50, 0x54, 1, 1, ...Array(16).fill(0)];
    const stored = Uint8Array.from([...header, 1, 0, 0x07]);  // payload length 1: half of the first value
    eq(sx.decodePatch(stored, table).values, [5, 3], "both take their defaults, the second isn't read from byte 1");
});

test("codec: a name is stored as 7-bit ASCII, other characters as ?", async () => {
    const t = await subtractiveTables();
    const patch = { module: 1, schema: 1, name: "Pad é x", values: [7, 0, 512, 0, 0, 0, 0, 0, 1, 400, 16000, 8000] };
    eq(sx.decodePatch(sx.encodePatch(patch, t.module), t.module).name, "Pad ??x", "é and a no-break space");
});
