// The editor session at its seam: SysEx bytes from the device in, SysEx
// bytes to the device and session state out. Device replies are the
// firmware's golden vectors.

import { test, eq, ok } from "./harness.js";
import { golden } from "./golden.js";
const { Session } = globalThis.t00t;
const { decodePatch, parse, splitSysEx } = globalThis.t00t.sysex;

// A session whose outgoing messages land in `sent`.
function newSession() {
    const sent = [];
    const session = new Session({ send: (bytes) => sent.push(Array.from(bytes)) });
    return { session, sent };
}

test("session: connecting identifies the device, then asks for its descriptors", async () => {
    const g = await golden();
    const { session, sent } = newSession();
    session.connect();
    eq(sent, [Array.from(g.identify_request)], "identify request first");
    session.receive(g.identify_reply);
    eq(session.device, { module: 1, schema: 1, protocol: 2, partCount: 16, paramCount: 12 }, "device identified");
    eq(sent.at(-1), Array.from(g.descriptor_dump_request), "then the descriptor dump request");
});

// Feed every message of a concatenated golden vector to the session.
function feed(session, bytes) {
    for (const m of splitSysEx(bytes)) session.receive(m);
}

test("session: after the descriptors, it syncs Part 0's Edit buffer into its own copy", async () => {
    const g = await golden();
    const { session, sent } = newSession();
    session.connect();
    session.receive(g.identify_reply);
    feed(session, g.descriptor_dump);
    eq(session.tables.module.map((d) => d.name).slice(8, 10), ["FILTER", "CUTOFF"], "the module's descriptors");
    eq(sent.at(-1), Array.from(g.edit_buffer_request_part0), "Part 0's Edit buffer requested");
    eq(session.state, "syncing", "waiting for the Edit buffer");
    session.receive(g.edit_buffer_dump_fairlite_part0);
    eq(session.state, "ready", "ready to edit");
    eq(session.part, 0, "editing Part 0");
    eq(session.patch, { module: 1, schema: 1, name: "Fairlite", values: [7, 0, 512, 0, 0, 0, 0, 0, 1, 400, 16000, 8000] },
       "the editor's copy of the Patch");
});

// A session connected to the golden subtractive device, Part 0 synced to Fairlite.
async function readySession() {
    const g = await golden();
    const s = newSession();
    s.session.connect();
    s.session.receive(g.identify_reply);
    feed(s.session, g.descriptor_dump);
    s.session.receive(g.edit_buffer_dump_fairlite_part0);
    s.sent.length = 0;
    return s;
}

test("session: moving a control streams a parameter change and updates the copy", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    session.setParam(9, 1234);
    eq(sent, [Array.from(g.param_change_cutoff_1234_part0)], "CUTOFF = 1234 to Part 0");
    eq(session.patch.values[9], 1234, "the copy holds it");
    session.setParam(9, 1234);
    eq(sent.length, 1, "an unchanged value is not sent again");
    session.setParam(9, 99999);
    eq(session.patch.values[9], 18000, "a value past the range is clamped to max");
});

test("session: selecting another Part syncs its Edit buffer, and edits go to it", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    session.selectPart(1);
    eq(sent, [Array.from(g.edit_buffer_request_part1)], "Part 1's Edit buffer requested");
    session.receive(g.edit_buffer_dump_fairlite_part0);
    eq(session.patch.name, "Fairlite", "a dump for another Part is ignored");
    session.receive(g.edit_buffer_dump_saw_flt_part1);
    eq([session.part, session.state, session.patch.name], [1, "ready", "Saw-Flt"], "Part 1 holds Saw-Flt");
    session.setParam(11, -500);
    eq(sent.at(-1), [0xf0, 0x7d, 0x54, 0x10, 1, 11, 0, 0x0c, 0x7c, 0x7f, 0xf7], "ENV>CUT = -500 to Part 1");
});

test("session: pushing sends the editor's copy back into the Part's Edit buffer", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    session.receive(g.edit_buffer_dump_saw_flt_part1);  // not ours: Part 1
    ok(session.push(), "pushed");
    eq(sent, [Array.from(g.edit_buffer_dump_fairlite_part0)], "the Fairlite copy goes back to Part 0");
});

test("session: Store pushes the copy, then stores it into the user slot", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    ok(session.store(10), "store sent");
    eq(sent, [Array.from(g.edit_buffer_dump_fairlite_part0), Array.from(g.store_request_part0_slot10)],
       "the copy, then the store request");
    eq(session.lastStore, { part: 0, slot: 10, result: null }, "waiting for the reply");
    session.receive(g.store_reply_part0_slot10_ok);
    eq(session.lastStore, { part: 0, slot: 10, result: 0 }, "stored");
    session.store(200);
    session.receive(g.store_reply_part0_slot200_no_such_slot);
    eq(session.lastStore, { part: 0, slot: 200, result: 1 }, "slot 200 doesn't exist");
});

test("session: Store under a new name renames the copy", async () => {
    const { session, sent } = await readySession();
    session.store(10, "My Fairlite Pad!!");
    eq(session.patch.name, "My Fairlite Pad!", "the name, cut to 16 characters");
    const pushed = parse(Uint8Array.from(sent[0]));
    eq(decodePatch(pushed.stored, session.tables.module).name, "My Fairlite Pad!", "the pushed Patch has the new name");
});

// The golden identify reply with some fields replaced.
async function identifyReply({ module = 1, schema = 1, protocol = 2 } = {}) {
    const g = await golden();
    const m = Uint8Array.from(g.identify_reply);
    m[4] = module;
    m[5] = schema;
    m[6] = protocol;
    return m;
}

test("session: an Edit buffer holding another module's Patch is reported, not taken", async () => {
    const g = await golden();
    const { session, sent } = newSession();
    session.connect();
    session.receive(g.identify_reply);
    feed(session, g.descriptor_dump);
    session.receive(Uint8Array.from([...g.edit_buffer_dump_fm_module_part1.subarray(0, 4), 0,
                                     ...g.edit_buffer_dump_fm_module_part1.subarray(5)]));  // as Part 0
    eq([session.state, session.patch], ["mismatch", null], "no copy taken");
    ok(session.problem.includes("module 3"), `the problem names the Patch's module: ${session.problem}`);
    sent.length = 0;
    eq([session.push(), session.store(1)], [false, false], "nothing to push or store");
    eq(sent, [], "nothing sent");
});

test("session: reconnecting to another module keeps the copy and refuses to send it", async () => {
    for (const other of [{ module: 3 }, { schema: 2 }]) {
        const { session, sent } = await readySession();
        session.connect();
        session.receive(await identifyReply(other));
        eq(session.state, "mismatch", `mismatch for ${JSON.stringify(other)}`);
        ok(/module 1 .*schema 1/.test(session.problem), `the problem names the copy's module and schema: ${session.problem}`);
        eq(session.patch.name, "Fairlite", "the copy is kept");
        sent.length = 0;
        session.setParam(9, 1234);
        eq([session.push(), session.store(1), sent], [false, false, []], "no edits, push or store sent");
        session.selectPart(1);
        eq(sent, [], "nor a Part's Edit buffer requested");
        session.discard();
        eq(session.patch, null, "discarding drops the copy");
        eq(sent.at(-1), Array.from((await golden()).descriptor_dump_request), "and describes the new device");
    }
});

test("session: reconnecting to the same module keeps the copy, ready to push or reload", async () => {
    const g = await golden();
    const { session, sent } = await readySession();
    session.setParam(9, 1234);
    session.connect();
    session.receive(g.identify_reply);
    eq([session.state, session.patch.values[9]], ["ready", 1234], "ready, holding the edited copy");
    eq(sent.slice(1), [Array.from(g.identify_request)], "nothing sent but the identify request");
    session.reload();
    eq(sent.at(-1), Array.from(g.edit_buffer_request_part0), "reload asks for the device's Edit buffer");
    session.receive(g.edit_buffer_dump_fairlite_part0);
    eq(session.patch.values[9], 400, "the copy follows the device again");
});

test("session: a device speaking another protocol version is reported and left alone", async () => {
    const { session, sent } = newSession();
    session.connect();
    session.receive(await identifyReply({ protocol: 1 }));
    eq(session.state, "unsupported", "unsupported");
    ok(session.problem.includes("protocol 1"), `the problem names the protocol: ${session.problem}`);
    eq(sent.length, 1, "nothing sent after the identify request");
});

test("session: a store reply still lands after switching Part", async () => {
    const g = await golden();
    const { session } = await readySession();
    session.store(10);
    session.selectPart(1);
    session.receive(g.edit_buffer_dump_saw_flt_part1);
    session.receive(g.store_reply_part0_slot10_ok);
    eq(session.lastStore, { part: 0, slot: 10, result: 0 }, "Part 0's store into slot 10 succeeded");
});

test("session: an Edit buffer from another schema than the device's is reported, not taken", async () => {
    const g = await golden();
    const { session } = newSession();
    session.connect();
    session.receive(g.identify_reply);
    feed(session, g.descriptor_dump);
    const dump = Uint8Array.from(g.edit_buffer_dump_fairlite_part0);
    dump[11] = 2;  // schema byte of the stored header (packed: offset 5 of the first group, after its high-bits byte)
    session.receive(dump);
    eq([session.state, session.patch], ["mismatch", null], "no copy taken");
});
