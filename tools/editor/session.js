// An editing session with one t00t over SysEx: identify, descriptor dump,
// Edit buffer sync, parameter streaming and Store. Transport-free: messages
// go out through `send` and come in through receive(), so the Web MIDI
// page and the tests drive the same code.
//
// A classic script (see sysex.js), loaded after sysex.js: publishes
// globalThis.t00t.Session.

(function (t00t) {
"use strict";

const sx = t00t.sysex;

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
        if (this.state !== "ready" || !d) return;
        const v = Math.min(Math.max(Math.round(value), d.min), d.max);
        if (this.patch.values[id] === v) return;
        this.patch.values[id] = v;
        this.send(sx.paramChange(this.part, id, v));
        this.onChange(this);
    }

    // Edit Part `part` instead, starting from its Edit buffer on the device.
    selectPart(part) {
        if (!["ready", "syncing"].includes(this.state) || part < 0 || part >= this.device.partCount) return;
        this.part = part;
        this.sync();
        this.onChange(this);
    }

    // Send the copy into the Part's Edit buffer on the device. False if
    // there is nothing to send.
    push() {
        if (this.state !== "ready") return false;
        this.send(sx.editBufferDump(this.part, sx.encodePatch(this.patch, this.tables.module)));
        return true;
    }

    // Store the copy into user slot `slot`, renamed to `name` if given:
    // pushed first, so the device stores what the editor shows. The result
    // arrives in lastStore. False if nothing was sent.
    store(slot, name) {
        if (this.state !== "ready") return false;
        if (name !== undefined) this.patch.name = Array.from(name).slice(0, sx.NAME_LEN).join("");
        this.push();
        this.send(sx.storeRequest(this.part, slot));
        this.lastStore = { part: this.part, slot, result: null };
        this.onChange(this);
        return true;
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
        if (this.state !== "ready") return;
        this.sync();
        this.onChange(this);
    }
}

function moduleText({ module, schema }) {
    const name = sx.MODULE_NAMES[module];
    return `module ${module}${name ? ` (${name})` : ""}, schema ${schema}`;
}

t00t.Session = Session;
})(globalThis.t00t ??= {});
