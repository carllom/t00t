// DX7 .syx import: the editor's conversion against the fixtures the host
// converter wrote (tools/host_render/golden/dx7_syx.txt, by
// tools/dx7_vectors.py), so editor and tools/syx2patch.py can't drift apart.

import { test, eq, ok, parseGolden } from "./harness.js";
const dx7 = globalThis.t00t.dx7;

let cached;
function fixtures() {
    cached ??= fetch("../../host_render/golden/dx7_syx.txt").then((r) => r.text()).then(parseGolden);
    return cached;
}

// A copy of `syx` with byte `at` set to `value` and the checksum redone.
function withByte(syx, at, value) {
    const out = Uint8Array.from(syx);
    out[at] = value;
    const data = out.subarray(6, out.length - 2);
    out[out.length - 2] = -data.reduce((a, b) => a + b, 0) & 0x7f;
    return out;
}

// --- Conversion ---------------------------------------------------------------

test("dx7 import: a 32-voice bank converts to the host converter's stored Patches", async () => {
    const f = await fixtures();
    const result = dx7.parseSyx(f.bank_syx);
    eq(result.error, undefined, "no error");
    eq(result.voices.length, 32, "32 voices");
    result.voices.forEach((v, i) => eq(v.stored, f[`bank_patch_${String(i).padStart(2, "0")}`], `voice ${i}`));
    eq(result.voices.slice(0, 9).map((v) => v.name),
       ["INIT VOICE", "E.PIANO 1", " LEAD", "BRASS", "A", "STRINGS  2", "BASS-1~", "TUB BELLS", ""],
       "names, trailing spaces dropped");
});

test("dx7 import: a single-voice dump converts to the host converter's stored Patch", async () => {
    const f = await fixtures();
    const result = dx7.parseSyx(f.voice_syx);
    eq(result.voices.length, 1, "one voice");
    eq(result.voices[0].stored, f.voice_patch, "the converter's bytes");
    eq(result.voices[0].stored, f.bank_patch_07, "the same as that voice in a bank");
    eq(result.voices[0].name, "TUB BELLS", "named");
});

// --- Refusals -----------------------------------------------------------------

test("dx7 import: what isn't a DX7 voice or bank dump is refused", async () => {
    const f = await fixtures();
    const refused = (bytes, pattern, what) => {
        const r = dx7.parseSyx(bytes);
        ok(r.error && pattern.test(r.error) && !r.voices, `${what}: ${r.error}`);
    };
    refused(Uint8Array.from([0x54, 0x30, 0x50, 0x54, 3, 1]), /not a DX7/, "a Patch file");
    refused(f.bank_syx.subarray(0, 4000), /4104 bytes/, "a truncated bank");
    refused(f.voice_syx.subarray(0, 100), /163 bytes/, "a truncated voice");
    refused(f.bank_syx.map((b, i) => (i === 3 ? 5 : b)), /format 5/, "another format");
    refused(f.bank_syx.map((b, i) => (i === 100 ? b ^ 1 : b)), /checksum/, "a bank with a bad checksum");
    refused(f.voice_syx.map((b, i) => (i === 100 ? b ^ 1 : b)), /checksum/, "a voice with a bad checksum");
    refused(f.voice_syx.map((b, i) => (i === 5 ? 0x1c : b)), /byte count/, "a voice with the wrong byte count");
});

test("dx7 import: a voice with a field out of range is refused, naming it", async () => {
    const f = await fixtures();
    const r1 = dx7.parseSyx(withByte(f.voice_syx, 6 + 5 * 21 + 20, 15));  // OP1 detune
    ok(/OP1 detune/.test(r1.error), `single voice: ${r1.error}`);
    const r2 = dx7.parseSyx(withByte(f.bank_syx, 6 + 3 * 128 + 117, 49));  // voice 3 transpose
    ok(/Voice 4: transpose/.test(r2.error), `bank: ${r2.error}`);
    const r3 = dx7.parseSyx(withByte(f.bank_syx, 6 + 128 + 8, 100));  // voice 1, OP6 break point
    ok(/Voice 2: OP6 break point/.test(r3.error), `bank: ${r3.error}`);
});

// --- Session: audition and store on an FM device ------------------------------

const sx = globalThis.t00t.sysex;
const { Session } = globalThis.t00t;
const { PATCH } = sx.SlotKind;
const FM_PARAMS = 151;

const msg = (command, body) => Uint8Array.from([0xf0, 0x7d, 0x54, command, ...body, 0xf7]);
const int21 = (v) => [v & 0x7f, (v >> 7) & 0x7f, (v >> 14) & 0x7f];
const str = (s) => [s.length, ...Array.from(s, (c) => c.charCodeAt(0))];

// An FM device's descriptor dump: 151 module parameters of 0..127, no common
// or Part parameters.
function fmDescriptorDump() {
    const out = [msg(0x41, [0, FM_PARAMS & 0x7f, FM_PARAMS >> 7])];
    for (let id = 0; id < FM_PARAMS; id++) {
        out.push(msg(0x42, [0, id & 0x7f, id >> 7, ...int21(0), ...int21(127), ...int21(0), 0, 0, 1, 0, 0x7f,
                            ...str(`P${id}`), ...str("FM")]));
    }
    out.push(msg(0x41, [1, 0, 0]), msg(0x41, [2, 0, 0]), msg(0x44, []));
    return out;
}

// A session with an FM device, ready, its Part 1 Edit buffer bank voice 0.
async function fmSession() {
    const f = await fixtures();
    const sent = [];
    const session = new Session({ send: (bytes) => sent.push(Array.from(bytes)) });
    session.connect();
    session.receive(msg(0x02, [3, 1, sx.PROTOCOL_VERSION, 16, FM_PARAMS & 0x7f, FM_PARAMS >> 7]));
    fmDescriptorDump().forEach((m) => session.receive(m));
    session.receive(sx.editBufferDump(0, f.bank_patch_00));
    ok(session.state === "ready", `ready (${session.state}: ${session.problem})`);
    sent.length = 0;
    return { session, sent, voices: dx7.parseSyx(f.bank_syx).voices };
}

const slotReply = (slot, result = 0) => msg(0x38, [PATCH, slot & 0x7f, slot >> 7, result]);

test("dx7 session: auditioning an imported voice pushes it into the Part's Edit buffer", async () => {
    const { session, sent, voices } = await fmSession();
    eq(session.importPatch(voices[5].stored), null, "imported");
    eq(session.patch.name, "STRINGS  2", "the copy is the voice");
    eq(sent, [Array.from(sx.editBufferDump(0, voices[5].stored))], "its stored bytes pushed to Part 1");
});

test("dx7 session: storing imported voices writes them into user slots, one at a time", async () => {
    const { session, sent, voices } = await fmSession();
    eq(session.storeVoices(voices.map((v) => v.stored), 96), null, "started");
    eq(sent, [Array.from(sx.bankSlot(PATCH, 96, voices[0].stored))], "voice 1 into slot 96, alone");
    session.receive(slotReply(97));
    eq(sent.length, 1, "an answer for another slot doesn't advance it");
    for (let i = 0; i < 32; i++) session.receive(slotReply(96 + i));
    eq(sent, voices.map((v, i) => Array.from(sx.bankSlot(PATCH, 96 + i, v.stored))), "each voice into its slot");
    eq([session.transfer.done, session.transfer.error, session.transfer.total], [true, null, 32], "done, 32 slots");
});

test("dx7 session: nothing else is sent while imported voices are stored", async () => {
    const { session, sent, voices } = await fmSession();
    session.storeVoices(voices.map((v) => v.stored), 0);
    sent.length = 0;
    session.setParam(3, 50);
    eq([session.push(), session.store(1), session.importPatch(voices[2].stored)],
       [false, false, "Wait for the voices to be stored."], "no push, Store or audition");
    eq(sent, [], "nothing sent: the device may be writing flash");
});

test("dx7 session: a refused slot stops the store and says which", async () => {
    const { session, sent, voices } = await fmSession();
    session.storeVoices(voices.map((v) => v.stored), 10);
    session.receive(slotReply(10));
    session.receive(slotReply(11, 3));
    ok(/Patch slot 11.*flash write failed/.test(session.transfer.error), `reported: ${session.transfer.error}`);
    eq(sent.length, 2, "nothing more is sent");
});

test("dx7 session: voices that don't fit the user slots, or a device that isn't FM, are refused", async () => {
    const { session, sent, voices } = await fmSession();
    const stored = voices.map((v) => v.stored);
    ok(/slots 100 to 131.*0 to 127/.test(session.storeVoices(stored, 100)), "past slot 127");
    ok(/slot/.test(session.storeVoices(stored, -1)), "before slot 0");
    ok(/whole number/.test(session.storeVoices(stored, 5.5)), "a slot that isn't a whole number");
    ok(/whole number/.test(session.storeVoices(stored, NaN)), "nor one that isn't a number");
    eq(session.storeVoices(stored.slice(0, 1), 127), null, "one voice into the last slot fits");
    eq(sent.length, 1, "only that one was sent");

    const g = await (await import("./golden.js")).golden();
    const sub = new Session({ send: (bytes) => sent.push(bytes) });
    sub.connect();
    sub.receive(g.identify_reply);
    sx.splitSysEx(g.descriptor_dump).forEach((m) => sub.receive(m));
    sub.receive(g.edit_buffer_dump_fairlite_part0);
    sent.length = 0;
    ok(/FM.*subtractive/.test(sub.storeVoices(stored, 0)), "a subtractive device: refused, naming both");
    ok(/FM.*subtractive/.test(sub.importPatch(stored[0])), "and no audition either");
    eq(sent, [], "nothing sent to it");
});
