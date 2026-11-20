// An editing session with one t00t over SysEx: identify, descriptor dump,
// Edit buffer sync, parameter streaming, Store, Patch import/export,
// storing imported voices, and user-bank backup and restore. Transport-free: messages
// go out through `send` and come in through receive(), so the Web MIDI
// page and the tests drive the same code.
//
// A classic script (see sysex.js), loaded after sysex.js and files.js: publishes
// globalThis.t00t.Session.

(function (t00t) {
"use strict";

const sx = t00t.sysex;
const files = t00t.files;
const { PATCH, MULTI } = sx.SlotKind;

class Session {
    constructor({ send, onChange = () => {} }) {
        this.send = send;
        this.onChange = onChange;
        this.state = "idle";
        this.device = null;
        this.tables = null;
        this.part = 0;
        this.patch = null;  // the editor's own copy of the Part's Patch
        this.dump = [];     // descriptor dump messages so far
        this.lastStore = null;  // { part, slot, result }: result null until the reply
        this.problem = null;    // what is wrong, in words, or null
        // A bank backup, bank restore or store of imported voices: { kind:
        // "backup" | "restore" | "import", done, error, count (slots so far),
        // total (slots in all, once known), file (a finished backup's bank
        // file), first (an import's first slot) }, or null.
        this.transfer = null;
    }

    // Start (or restart) talking to the device.
    connect() {
        this.device = null;
        this.state = "identifying";
        this.send(sx.identifyRequest());
    }

    // Edit parameter `id` of the copy and stream it to the device. The value
    // is clamped to the descriptor's range; an unchanged value isn't sent.
    setParam(id, value) {
        const d = this.tables?.module[id];
        if (!this.editable() || !d) return;
        const v = Math.min(Math.max(Math.round(value), d.min), d.max);
        if (this.patch.values[id] === v) return;
        this.patch.values[id] = v;
        this.send(sx.paramChange(this.part, id, v));
        this.onChange(this);
    }

    // Edit Part `part` instead, starting from its Edit buffer on the device.
    selectPart(part) {
        if (!["ready", "syncing"].includes(this.state) || this.writingSlots() || part < 0 ||
            part >= this.device.partCount) return;
        this.part = part;
        this.sync();
        this.onChange(this);
    }

    // Send the copy into the Part's Edit buffer on the device. False if
    // there is nothing to send.
    push() {
        if (!this.editable()) return false;
        this.send(sx.editBufferDump(this.part, sx.encodePatch(this.patch, this.tables.module)));
        return true;
    }

    // Store the copy into user slot `slot`, renamed to `name` if given:
    // pushed first, so the device stores what the editor shows. The result
    // arrives in lastStore. False if nothing was sent.
    store(slot, name) {
        if (!this.editable()) return false;
        if (name !== undefined) this.patch.name = Array.from(name).slice(0, sx.NAME_LEN).join("");
        this.push();
        this.send(sx.storeRequest(this.part, slot));
        this.lastStore = { part: this.part, slot, result: null };
        this.onChange(this);
        return true;
    }

    // The copy as a stored Patch (a Patch file), or null if there is none.
    exportPatch() {
        if (!this.patch || !this.tables) return null;
        return sx.encodePatch(this.patch, this.tables.module);
    }

    // Make the stored Patch `stored` (a Patch file) the copy and push it into
    // the Part's Edit buffer. A Patch from an older or newer schema of the
    // device's module is read the way the device reads one (missing
    // parameters take their default). Returns null, or why it was refused.
    importPatch(stored) {
        if (this.writingSlots()) return this.waitText();
        if (this.state !== "ready") return "Not connected to a device ready to edit.";
        const patch = sx.decodePatch(stored, this.tables.module);
        if (patch.error) return patch.error === "truncated" ? "This file is not a Patch (truncated)." : "This file is not a Patch.";
        if (patch.module !== this.device.module) return this.wrongModuleText(patch.module);
        this.patch = { ...patch, schema: this.device.schema };
        this.push();
        this.onChange(this);
        return null;
    }

    // Back up the user bank: ask for the bank dump; the bank file arrives in
    // transfer.file. False if nothing was sent.
    backup() {
        if (this.state !== "ready" || this.transferring()) return false;
        this.transfer = { kind: "backup", done: false, error: null, count: 0, total: null, file: null,
                          slots: { [PATCH]: [], [MULTI]: [] } };
        this.send(sx.bankDumpRequest());
        this.onChange(this);
        return true;
    }

    // Restore the bank file `file` into the user bank, one slot at a time:
    // each slot goes out only once the device has answered the one before,
    // so nothing arrives over DIN while flash is being written. Stops at the
    // first slot the device refuses. False if nothing was sent (the reason
    // is in transfer.error).
    restore(file) {
        if (this.state !== "ready" || this.transferring()) return false;
        const bank = files.decodeBank(file);
        let error = bank.error ? `That is not a bank file this editor reads (${bank.error}).` : null;
        if (!error && bank.module !== this.device.module) {
            error = `The bank file is for ${moduleText({ module: bank.module })}; the device is ` +
                    `${moduleText({ module: this.device.module })}. Nothing was sent.`;
        }
        if (error) {
            this.transfer = { kind: "restore", done: false, error, count: 0, total: null, file: null };
            this.onChange(this);
            return false;
        }
        const queue = [...bank.patches.map((stored, slot) => ({ kind: PATCH, slot, stored })),
                       ...bank.multis.map((stored, slot) => ({ kind: MULTI, slot, stored }))];
        this.transfer = { kind: "restore", done: queue.length === 0, error: null, count: 0, total: queue.length,
                          file: null, queue };
        this.sendNextSlot();
        this.onChange(this);
        return true;
    }

    // Store the stored Patches `patches` (imported DX7 voices) into the user
    // Patch slots from `first` on, one slot at a time as a restore sends
    // them, stopping at the first slot the device refuses. Progress is in
    // transfer. Returns null, or why nothing was sent.
    storeVoices(patches, first) {
        if (this.transferring()) return this.waitText();
        if (this.state !== "ready") return "Not connected to a device ready to edit.";
        const decoded = patches.map((stored) => sx.decodePatch(stored, this.tables.module));
        if (decoded.some((p) => p.error)) return "That is not a Patch.";
        const other = decoded.find((p) => p.module !== this.device.module);
        if (other) return this.wrongModuleText(other.module);
        const badSlots = slotsProblem(patches.length, first);
        if (badSlots) return badSlots;
        const queue = patches.map((stored, i) => ({ kind: PATCH, slot: first + i, stored }));
        this.transfer = { kind: "import", done: queue.length === 0, error: null, count: 0, total: queue.length,
                          file: null, first, queue };
        this.sendNextSlot();
        this.onChange(this);
        return null;
    }

    // Whether a backup, restore or store of imported voices is under way.
    transferring() {
        return !!this.transfer && !this.transfer.done && !this.transfer.error;
    }

    // Whether a restore or a store of imported voices is under way. Nothing
    // else is sent meanwhile: each slot may have the device writing flash,
    // when DIN input is lost.
    writingSlots() {
        return this.transferring() && this.transfer.kind !== "backup";
    }

    waitText() {
        return { backup: "Wait for the bank backup to finish.", restore: "Wait for the bank restore to finish.",
                 import: "Wait for the voices to be stored." }[this.transfer.kind];
    }

    wrongModuleText(module) {
        return `This Patch is for ${moduleText({ module })}; the device is ${moduleText(this.device)}.`;
    }

    // Whether the copy can be edited, pushed or stored now.
    editable() {
        return this.state === "ready" && !this.writingSlots();
    }

    // Give up on the backup, restore or store under way, for `reason`.
    abortTransfer(reason) {
        if (!this.transferring()) return;
        this.transfer = { ...this.transfer, error: reason };
        this.onChange(this);
    }

    sendNextSlot() {
        const t = this.transfer;
        if (t.count === t.total) return;
        const { kind, slot, stored } = t.queue[t.count];
        this.send(sx.bankSlot(kind, slot, stored));
    }

    // A bank dump slot from the device, during a backup.
    backupSlot({ kind, slot, stored }) {
        const t = this.transfer;
        const slots = t.slots[kind];
        if ((kind === PATCH && t.slots[MULTI].length > 0) || slot !== slots.length) {
            this.transfer = { ...t, error: `The bank dump skipped ${slotText(kind, slots.length)}. Try again.` };
            return;
        }
        slots.push(stored);
        t.count++;
    }

    backupEnd({ patchSlots, multiSlots }) {
        const t = this.transfer;
        if (t.slots[PATCH].length !== patchSlots || t.slots[MULTI].length !== multiSlots) {
            this.transfer = { ...t, error: "The bank dump ended early. Try again." };
            return;
        }
        this.transfer = { ...t, done: true, total: t.count, slots: null,
                          file: files.encodeBank({ module: this.device.module, patches: t.slots[PATCH],
                                                   multis: t.slots[MULTI] }) };
    }

    // The device's answer for one slot of a restore or of a store of
    // imported voices.
    slotWritten({ kind, slot, result }) {
        const t = this.transfer;
        const expected = t.queue[t.count];
        if (kind !== expected.kind || slot !== expected.slot) return;
        if (result !== sx.STORE_OK) {
            const verb = t.kind === "restore" ? "restored" : "stored";
            this.transfer = { ...t, error: `${slotText(kind, slot)} was not ${verb}: ${sx.storeResultText(result)}. ` +
                                           "The slots before it were." };
            return;
        }
        t.count++;
        if (t.count === t.total) t.done = true;
        else this.sendNextSlot();
    }

    // One whole message from the device, F0 ... F7.
    receive(bytes) {
        const m = sx.parse(bytes);
        if (!m) return;
        switch (m.type) {
            case "identify":
                this.identified(m);
                break;
            case "tableBegin":
            case "descriptor":
            case "labels":
                if (this.state === "describing") this.dump.push(m);
                break;
            case "dumpEnd":
                if (this.state === "describing") this.described();
                break;
            case "editBuffer":
                if (this.state === "syncing" && m.part === this.part) this.synced(m.stored);
                break;
            case "bankSlot":
                if (this.transferring() && this.transfer.kind === "backup") this.backupSlot(m);
                break;
            case "bankDumpEnd":
                if (this.transferring() && this.transfer.kind === "backup") this.backupEnd(m);
                break;
            case "bankSlotReply":
                if (this.writingSlots()) this.slotWritten(m);
                break;
            case "storeReply":
                if (m.part === this.lastStore?.part && m.slot === this.lastStore.slot) {
                    this.lastStore = { ...this.lastStore, result: m.result };
                }
                break;
        }
        this.onChange(this);
    }

    identified({ type, ...device }) {
        this.device = device;
        this.problem = null;
        if (device.protocol !== sx.PROTOCOL_VERSION) {
            this.state = "unsupported";
            this.problem = `The device speaks t00t SysEx protocol ${device.protocol}; this editor speaks ` +
                           `protocol ${sx.PROTOCOL_VERSION}. Update the editor or the firmware.`;
            return;
        }
        if (this.patch && (this.patch.module !== device.module || this.patch.schema !== device.schema)) {
            this.state = "mismatch";
            this.problem = `The editor holds a Patch for ${moduleText(this.patch)}, but the device is ` +
                           `${moduleText(device)}. Nothing is sent to it until you discard the editor's copy.`;
            return;
        }
        if (this.patch) {
            this.state = "ready";
            return;
        }
        this.describe();
    }

    describe() {
        this.dump = [];
        this.state = "describing";
        this.send(sx.descriptorDumpRequest());
    }

    described() {
        this.tables = sx.assembleDump(this.dump);
        this.dump = [];
        if (!this.tables || this.tables.module.length !== this.device.paramCount) {
            this.state = "mismatch";
            this.problem = this.tables
                ? `The device describes ${this.tables.module.length} parameters but said it has ${this.device.paramCount}.`
                : "The descriptor dump was incomplete. Reconnect to try again.";
            this.tables = null;
            return;
        }
        this.sync();
    }

    // Ask for the Part's Edit buffer; synced() takes it.
    sync() {
        this.state = "syncing";
        this.send(sx.editBufferRequest(this.part));
    }

    synced(stored) {
        const patch = sx.decodePatch(stored, this.tables.module);
        if (patch.error) {
            this.problem = `The device sent an unreadable Edit buffer (${patch.error}).`;
            return;
        }
        if (patch.module !== this.device.module || patch.schema !== this.device.schema) {
            this.state = "mismatch";
            this.problem = `The device sent a Patch for ${moduleText(patch)}, but is ${moduleText(this.device)}.`;
            return;
        }
        this.patch = patch;
        this.problem = null;
        this.state = "ready";
    }

    // Drop the copy (and the descriptors it was made with) and start over
    // from the device's own descriptors and Edit buffer.
    discard() {
        if (!this.device || this.state === "unsupported") return;
        this.patch = null;
        this.tables = null;
        this.problem = null;
        this.describe();
        this.onChange(this);
    }

    // Replace the copy with the device's Edit buffer for the Part.
    reload() {
        if (!this.editable()) return;
        this.sync();
        this.onChange(this);
    }
}

// Why `count` voices can't go into the user Patch slots from `first` on,
// or null if they can.
function slotsProblem(count, first) {
    if (!Number.isInteger(first)) return `The first user slot must be a whole number, not ${first}.`;
    const last = first + count - 1;
    if (first < 0 || last >= sx.USER_PATCH_SLOTS) {
        return `${count} voices need user slots ${first} to ${last}; the user slots are 0 to ` +
               `${sx.USER_PATCH_SLOTS - 1}. Nothing was stored.`;
    }
    return null;
}

function moduleText({ module, schema }) {
    const name = sx.MODULE_NAMES[module];
    return `module ${module}${name ? ` (${name})` : ""}${schema === undefined ? "" : `, schema ${schema}`}`;
}

function slotText(kind, slot) {
    return `${kind === PATCH ? "Patch" : "Multi"} slot ${slot}`;
}

t00t.Session = Session;
t00t.Session.slotsProblem = slotsProblem;
})(globalThis.t00t ??= {});
