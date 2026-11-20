// Banks and files: the bank messages against the firmware's golden vectors,
// the bank file format, and the session's backup, restore and Patch
// import/export, with the device's replies taken from the golden vectors.

import { test, eq, ok } from "./harness.js";
import { golden } from "./golden.js";
const { Session } = globalThis.t00t;
const sx = globalThis.t00t.sysex;
const files = globalThis.t00t.files;

const { PATCH, MULTI } = sx.SlotKind;

// The golden bank dump, parsed: { patches, multis } of stored bytes or null.
async function goldenBank() {
    const g = await golden();
    const msgs = sx.splitSysEx(g.bank_dump).map(sx.parse);
    const patches = msgs.filter((m) => m.type === "bankSlot" && m.kind === PATCH).map((m) => m.stored);
    const multis = msgs.filter((m) => m.type === "bankSlot" && m.kind === MULTI).map((m) => m.stored);
    return { patches, multis };
}

async function storedPatch(name) {
    return sx.parse((await golden())[name]).stored;
}

// --- Codec ------------------------------------------------------------------

test("bank codec: bank dump request", async () => {
    const g = await golden();
    eq(sx.bankDumpRequest(), g.bank_dump_request, "request bytes");
});

test("bank codec: a bank dump parses into every slot, then the end", async () => {
    const g = await golden();
    const msgs = sx.splitSysEx(g.bank_dump).map(sx.parse);
    eq(msgs.length, 128 + 32 + 1, "128 Patch slots, 32 Multi slots, the end");
    eq([msgs[0].type, msgs[0].kind, msgs[0].slot], ["bankSlot", PATCH, 0], "Patch slot 0 first");
    eq(msgs[0].stored, await storedPatch("edit_buffer_dump_fairlite_part0"), "holding Fairlite's stored bytes");
    eq(msgs[1], { type: "bankSlot", kind: PATCH, slot: 1, stored: null }, "an empty slot has no data");
    eq([msgs[128 + 3].kind, msgs[128 + 3].slot, String.fromCharCode(...msgs[128 + 3].stored.subarray(0, 4))],
       [MULTI, 3, "T0MT"], "Multi slot 3 holds a stored Multi");
    eq(msgs.at(-1), { type: "bankDumpEnd", patchSlots: 128, multiSlots: 32 }, "the end carries both slot counts");
});

test("bank codec: bank slot messages, with and without data", async () => {
    const g = await golden();
    const bank = await goldenBank();
    eq(sx.bankSlot(PATCH, 10, await storedPatch("edit_buffer_dump_saw_flt_part1")), g.bank_slot_patch10_saw_flt,
       "Saw-Flt into Patch slot 10");
    eq(sx.bankSlot(PATCH, 0, null), g.bank_slot_patch0_empty, "Patch slot 0 emptied");
    eq(sx.bankSlot(MULTI, 3, bank.multis[3]), g.bank_slot_multi3_default, "the default Multi into Multi slot 3");
    eq(sx.bankSlot(PATCH, 200, await storedPatch("edit_buffer_dump_saw_flt_part1")), g.bank_slot_patch200_saw_flt,
       "slot past 127");
});

test("bank codec: bank slot replies", async () => {
    const g = await golden();
    eq(sx.parse(g.bank_slot_reply_patch10_ok), { type: "bankSlotReply", kind: PATCH, slot: 10, result: 0 }, "OK");
    eq(sx.parse(g.bank_slot_reply_patch10_bad_patch), { type: "bankSlotReply", kind: PATCH, slot: 10, result: 2 },
       "bad Patch");
    eq(sx.parse(g.bank_slot_reply_multi3_flash_failed),
       { type: "bankSlotReply", kind: MULTI, slot: 3, result: 3 }, "flash write failed");
});

// --- Bank files -------------------------------------------------------------

test("bank file: header, then each slot's length and stored bytes", async () => {
    const bank = await goldenBank();
    const file = files.encodeBank({ module: 1, ...bank });
    eq(Array.from(file.subarray(0, 10)), [0x54, 0x30, 0x42, 0x4b, 1, 1, 128, 0, 32, 0],
       "T0BK, format 1, module 1, 128 Patch slots, 32 Multi slots");
    const fairlite = await storedPatch("edit_buffer_dump_fairlite_part0");
    eq(Array.from(file.subarray(10, 12 + fairlite.length)), [fairlite.length, 0, ...fairlite],
       "Patch slot 0: its length, then Fairlite's stored bytes");
    eq(Array.from(file.subarray(12 + fairlite.length, 14 + fairlite.length)), [0, 0], "Patch slot 1: empty");
    eq(files.decodeBank(file), { module: 1, ...bank }, "and it reads back");
});

test("bank file: what isn't a bank file is refused", async () => {
    const file = files.encodeBank({ module: 1, ...(await goldenBank()) });
    eq(files.decodeBank(await storedPatch("edit_buffer_dump_fairlite_part0")).error, "not a t00t bank file",
       "a Patch file");
    eq(files.decodeBank(file.subarray(0, file.length - 1)).error, "truncated", "one byte short");
    const v2 = Uint8Array.from(file);
    v2[4] = 2;
    ok(/format 2/.test(files.decodeBank(v2).error), "a newer format version");
    eq(files.decodeBank(Uint8Array.from([...file, 0])).error, "trailing bytes", "one byte too many");
});

// --- Session ----------------------------------------------------------------

function feed(session, bytes) {
    for (const m of sx.splitSysEx(bytes)) session.receive(m);
}

async function readySession() {
    const g = await golden();
    const sent = [];
    const session = new Session({ send: (bytes) => sent.push(Array.from(bytes)) });
    session.connect();
    session.receive(g.identify_reply);
    feed(session, g.descriptor_dump);
    session.receive(g.edit_buffer_dump_fairlite_part0);
    sent.length = 0;
    return { session, sent };
}

// The device's answer to a bank slot message.
function slotReply(kind, slot, result = 0) {
    return Uint8Array.from([0xf0, 0x7d, 0x54, 0x38, kind, slot & 0x7f, slot >> 7, result, 0xf7]);
}

test("session: backing up asks for the bank dump and makes a bank file of it", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    ok(session.backup(), "backup started");
    eq(sent, [Array.from(g.bank_dump_request)], "bank dump requested");
    feed(session, g.bank_dump);
    eq(session.transfer.done, true, "done once the end arrives");
    eq(files.decodeBank(session.transfer.file), { module: 1, ...(await goldenBank()) },
       "the file holds every slot of the dump, for module 1");
});

test("session: a bank dump missing a slot is reported, no file", async () => {
    const g = await golden();
    const { session } = await readySession();
    session.backup();
    const msgs = sx.splitSysEx(g.bank_dump);
    msgs.filter((_, i) => i !== 5).forEach((m) => session.receive(m));
    ok(session.transfer.error && !session.transfer.file, `reported: ${session.transfer.error}`);
});

test("session: restoring sends one slot at a time, each after the last one's answer", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    const file = files.encodeBank({ module: 1, ...(await goldenBank()) });
    ok(session.restore(file), "restore started");
    eq(sent, [Array.from(sx.splitSysEx(g.bank_dump)[0])], "Patch slot 0 first, alone");
    session.receive(slotReply(PATCH, 1));
    eq(sent.length, 1, "an answer for another slot doesn't advance it");
    session.receive(g.bank_slot_reply_patch0_ok);
    eq(sent.length, 2, "slot 0's answer sends slot 1");
    for (let i = 1; i < 128; i++) session.receive(slotReply(PATCH, i));
    for (let i = 0; i < 32; i++) session.receive(slotReply(MULTI, i));
    eq(sent, sx.splitSysEx(g.bank_dump).slice(0, -1).map((m) => Array.from(m)),
       "every slot went out as the device dumped it");
    eq([session.transfer.done, session.transfer.error], [true, null], "done");
});

test("session: nothing else is sent while a restore runs", async () => {
    const { session, sent } = await readySession();
    session.restore(files.encodeBank({ module: 1, ...(await goldenBank()) }));
    sent.length = 0;
    session.setParam(9, 1234);
    eq([session.push(), session.store(1), session.importPatch(await storedPatch("edit_buffer_dump_saw_flt_part1"))],
       [false, false, "Wait for the bank restore to finish."], "no push, Store or import");
    session.selectPart(1);
    session.reload();
    eq(sent, [], "no edit, Part switch or reload either: the device may be writing flash");
    eq(session.patch.values[9], 400, "the copy is unchanged");
});

test("session: a refused slot stops the restore and says why", async () => {
    const { session, sent } = await readySession();
    const bank = await goldenBank();
    session.restore(files.encodeBank({ module: 1, ...bank }));
    for (let i = 0; i < 10; i++) session.receive(slotReply(PATCH, i));
    session.receive((await golden()).bank_slot_reply_patch10_bad_patch);
    ok(/Patch slot 10.*bad Patch/.test(session.transfer.error), `reported: ${session.transfer.error}`);
    eq(sent.length, 11, "nothing more is sent");
});

test("session: a bank file for another module is refused, nothing sent", async () => {
    const { session, sent } = await readySession();
    eq(session.restore(files.encodeBank({ module: 3, ...(await goldenBank()) })), false, "not started");
    ok(/module 3/.test(session.transfer.error), `reported: ${session.transfer.error}`);
    eq(session.restore(Uint8Array.from([1, 2, 3])), false, "nor is a file that isn't a bank");
    eq(sent, [], "nothing sent");
});

test("session: exporting gives the copy as a stored Patch", async () => {
    const { session } = await readySession();
    eq(session.exportPatch(), await storedPatch("edit_buffer_dump_fairlite_part0"), "Fairlite's stored bytes");
});

test("session: importing a Patch makes it the copy and pushes it", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    session.selectPart(1);
    session.receive(g.edit_buffer_dump_fairlite_part0.map((b, i) => (i === 4 ? 1 : b)));  // Fairlite, as Part 1
    sent.length = 0;
    eq(session.importPatch(await storedPatch("edit_buffer_dump_saw_flt_part1")), null, "imported");
    eq(session.patch.name, "Saw-Flt", "the copy is Saw-Flt");
    eq(sent, [Array.from(g.edit_buffer_dump_saw_flt_part1)], "pushed into Part 1's Edit buffer");
});

test("session: importing a Patch for another module is refused", async () => {
    const { session, sent } = await readySession();
    ok(/module 3/.test(session.importPatch(await storedPatch("edit_buffer_dump_fm_module_part1"))),
       "the refusal names the module");
    ok(/not a Patch/.test(session.importPatch(Uint8Array.from([1, 2, 3, 4]))), "a file that isn't a Patch");
    eq([session.patch.name, sent], ["Fairlite", []], "the copy is kept, nothing sent");
});

test("session: a Patch from an older schema imports with defaults, as the device's schema", async () => {
    const { session } = await readySession();
    const stored = (await storedPatch("edit_buffer_dump_fairlite_part0")).slice(0, 24 + 19);  // ENV>CUT 8000
    stored[5] = 0;   // schema 0
    stored[22] = 19; // without ENV>CUT
    eq(session.importPatch(stored), null, "imported");
    eq([session.patch.schema, session.patch.values[11]], [1, 0], "schema 1, ENV>CUT at its default");
});
