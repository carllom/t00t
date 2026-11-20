// The editor page: Web MIDI ports in and out of a Session, and a generic UI
// built from the device's descriptors -- one panel per group, one control
// per parameter.
//
// A classic script (see sysex.js), loaded after sysex.js, files.js, dx7.js and session.js.

(function (t00t) {
"use strict";

const { Session } = t00t;
const { MODULE_NAMES, STORE_OK, Unit, Curve, ParamClass, storeResultText } = t00t.sysex;

const $ = (id) => document.getElementById(id);

const ANSWER_TIMEOUT_MS = 2000;
// A backup or restore with no progress for this long has stalled. Each
// restored slot may rewrite a flash sector on the device first.
const TRANSFER_TIMEOUT_MS = 5000;

let midi = null;
let output = null;
let input = null;
let portName = null;    // the device being edited, to reconnect to when it returns
let appProblem = null;  // a problem outside the session (browser, ports, no answer)
let builtFor = null;    // the descriptor tables the panels were built from
const controls = [];    // per module parameter: { update(value) }
let fileMessage = null; // { text, ok } from the last import, or null
let dx7Import = null;   // the imported DX7 .syx: { name, voices: [{ name, stored }] }, or null
let transferTimer = null;
let transferSeen = null; // the transfer and its count when the timer was last armed

const session = new Session({
    send: (bytes) => output?.send(bytes),
    onChange: render,
});

// --- Web MIDI ---------------------------------------------------------------

async function start() {
    if (!navigator.requestMIDIAccess) {
        appProblem = "This browser has no Web MIDI. Use a Chromium browser (Chrome, Edge, Brave, Opera); " +
                     "Firefox needs a site permission add-on, and Safari has no Web MIDI.";
        render();
        return;
    }
    try {
        midi = await navigator.requestMIDIAccess({ sysex: true });
    } catch (e) {
        appProblem = `MIDI access was refused (${e.name}). The editor needs MIDI with SysEx: allow it for this ` +
                     "page in the browser's site settings, then reload.";
        render();
        return;
    }
    midi.onstatechange = portsChanged;
    listPorts();
    render();
}

// Outputs that have an input of the same name: a device we can talk to both ways.
function devicePorts() {
    const inputs = [...midi.inputs.values()];
    return [...midi.outputs.values()]
        .filter((o) => o.state === "connected")
        .map((o) => ({ output: o, input: inputs.find((i) => i.name === o.name && i.state === "connected") }))
        .filter((p) => p.input);
}

function listPorts() {
    const select = $("port");
    const ports = devicePorts();
    const previous = select.value || portName;
    select.replaceChildren(...ports.map((p) => new Option(p.output.name, p.output.name)));
    if (ports.length === 0) select.append(new Option("No MIDI devices", ""));
    const preferred = ports.find((p) => p.output.name === previous) ?? ports.find((p) => /t00t/i.test(p.output.name));
    if (preferred) select.value = preferred.output.name;
}

function connect() {
    const port = devicePorts().find((p) => p.output.name === $("port").value);
    if (!port) return;
    if (input) input.onmidimessage = null;
    ({ output, input } = port);
    portName = output.name;
    input.onmidimessage = (e) => {
        if (e.data[0] === 0xf0) session.receive(e.data);
    };
    appProblem = null;
    session.connect();
    setTimeout(() => {
        if (session.state === "identifying" && output === port.output) {
            appProblem = `No answer from ${portName}. Is it a t00t running a patch-platform module?`;
            render();
        }
    }, ANSWER_TIMEOUT_MS);
}

// A port came or went: follow the edited device out and back in.
function portsChanged() {
    listPorts();
    const edited = devicePorts().find((p) => p.output.name === portName);
    if (output && !edited) {
        output = null;
        input = null;
        appProblem = `${portName} disconnected. The editor keeps its copy; it reconnects when the device returns.`;
        render();
    } else if (!output && edited && portName) {
        $("port").value = portName;
        connect();
    }
}

// --- Values -----------------------------------------------------------------

// Values in a physical unit as text, as the device shows them (param_format(),
// src/patch/descriptor.h).
const sign = (v) => (v < 0 ? "-" : "");
const UNIT_TEXT = {
    [Unit.HZ]: (v, a) => (a < 10000 ? `${v}Hz` : `${sign(v)}${Math.floor(a / 1000)}.${Math.floor((a % 1000) / 100)}k`),
    [Unit.CENTI_HZ]: (v, a) => `${sign(v)}${Math.floor(a / 100)}.${String(a % 100).padStart(2, "0")}Hz`,
    [Unit.PERMILLE]: (v, a) => `${sign(v)}${Math.floor(a / 10)}.${a % 10}%`,
};

// Slider positions: the value itself for a short linear range, else 0..1000
// mapped linearly or exponentially onto the range.
const SLIDER_STEPS = 1000;

function sliderMapping(d) {
    const range = d.max - d.min;
    if (d.curve !== Curve.EXPONENTIAL && range <= SLIDER_STEPS) {
        return { min: d.min, max: d.max, toValue: (p) => p, toPosition: (v) => v };
    }
    if (d.curve === Curve.EXPONENTIAL) {
        const k = Math.log(range + 1);
        return {
            min: 0, max: SLIDER_STEPS,
            toValue: (p) => d.min + Math.round(Math.exp((k * p) / SLIDER_STEPS) - 1),
            toPosition: (v) => Math.round((Math.log(v - d.min + 1) / k) * SLIDER_STEPS),
        };
    }
    return {
        min: 0, max: SLIDER_STEPS,
        toValue: (p) => d.min + Math.round((range * p) / SLIDER_STEPS),
        toPosition: (v) => Math.round(((v - d.min) / range) * SLIDER_STEPS),
    };
}

// --- Panels -----------------------------------------------------------------

function hintFor(d) {
    const hints = [];
    if (d.cls === ParamClass.NOTE_ON) hints.push("applies from the next note");
    if (d.cc !== null) hints.push(`CC ${d.cc}`);
    if (d.quickEdit) hints.push("on the Quick-edit Page");
    return hints.join(" · ");
}

function paramControl(d) {
    const row = document.createElement("div");
    row.className = "param";
    const id = `param-${d.id}`;
    const label = document.createElement("label");
    label.htmlFor = id;
    label.textContent = d.name;
    label.title = `${d.name} (parameter ${d.id})`;
    row.append(label);

    let update;
    if (d.cls === ParamClass.DATA_EXCHANGE) {
        const note = document.createElement("span");
        note.textContent = "bulk data, not editable here";
        row.append(note);
        update = () => {};
    } else if (d.unit === Unit.ENUM && d.labels) {
        const select = document.createElement("select");
        select.id = id;
        d.labels.forEach((text, i) => select.append(new Option(text, String(d.min + i))));
        select.onchange = () => session.setParam(d.id, Number(select.value));
        row.append(select);
        update = (v) => { select.value = String(v); };
    } else {
        const map = sliderMapping(d);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.id = id;
        slider.min = map.min;
        slider.max = map.max;
        slider.oninput = () => session.setParam(d.id, map.toValue(Number(slider.value)));
        const number = document.createElement("input");
        number.type = "number";
        number.min = d.min;
        number.max = d.max;
        number.title = d.name;
        number.onchange = () => {
            if (number.value === "" || !Number.isFinite(Number(number.value))) number.value = session.patch.values[d.id];
            else session.setParam(d.id, Number(number.value));
        };
        row.append(slider, number);
        update = (v) => {
            if (map.toValue(Number(slider.value)) !== v) slider.value = map.toPosition(v);
            if (document.activeElement !== number) number.value = v;
        };
    }
    // The hint line: the value in its unit (where it has one), then the hints.
    const unitText = UNIT_TEXT[d.unit];
    const hint = hintFor(d);
    if (hint || unitText) {
        const h = document.createElement("div");
        h.className = "hint";
        h.textContent = hint;
        row.append(h);
        if (unitText) {
            const inner = update;
            update = (v) => {
                inner(v);
                h.textContent = [unitText(v, Math.abs(v)), hint].filter(Boolean).join(" · ");
            };
        }
    }
    return { row, update };
}

function buildPanels(tables) {
    const panels = new Map();
    controls.length = 0;
    for (const d of tables.module) {
        if (!panels.has(d.group)) {
            const fs = document.createElement("fieldset");
            fs.className = "panel";
            const legend = document.createElement("legend");
            legend.textContent = d.group;
            fs.append(legend);
            panels.set(d.group, fs);
        }
        const c = paramControl(d);
        panels.get(d.group).append(c.row);
        controls[d.id] = c;
    }
    $("panels").replaceChildren(...panels.values());
    builtFor = tables;
}

// --- Rendering ----------------------------------------------------------------

function deviceText(device) {
    const name = MODULE_NAMES[device.module] ?? `module ${device.module}`;
    return `${portName}: ${name}, schema ${device.schema}, ${device.paramCount} parameters, ${device.partCount} Parts`;
}

const STATUS = {
    idle: () => (midi ? "Not connected" : appProblem ? "No MIDI" : "Starting…"),
    identifying: () => `Asking ${portName} who it is…`,
    describing: () => "Reading the descriptors…",
    syncing: () => `Loading Part ${session.part + 1}'s Edit buffer…`,
    ready: () => deviceText(session.device),
    mismatch: () => deviceText(session.device),
    unsupported: () => `${portName}: unsupported`,
};

function render() {
    $("connect").disabled = !midi || devicePorts().length === 0;
    $("status").textContent = output || session.state === "idle" ? STATUS[session.state]() : "Disconnected";

    const problem = appProblem ?? session.problem;
    $("problem").classList.toggle("hidden", !problem);
    $("problem-text").textContent = problem ?? "";
    $("discard").classList.toggle("hidden", !(session.state === "mismatch" && session.patch && output));

    const ready = session.editable() && !!output;
    const tables = session.tables;
    $("toolbar").classList.toggle("hidden", !tables || !session.patch);
    for (const id of ["reload", "push", "store", "part"]) $(id).disabled = !ready;

    if (session.device && $("part").options.length !== session.device.partCount) {
        $("part").replaceChildren(...Array.from({ length: session.device.partCount },
                                                (_, i) => new Option(String(i + 1), String(i))));
    }
    $("part").value = String(session.part);

    if (tables && tables !== builtFor) buildPanels(tables);
    if (!tables) {
        $("panels").replaceChildren();
        builtFor = null;
    }
    if (session.patch && tables === builtFor) {
        session.patch.values.forEach((v, i) => controls[i]?.update(v));
        $("patch-name").textContent = session.patch.name;
        if (document.activeElement !== $("name") && !$("name").dataset.edited) $("name").value = session.patch.name;
    }
    $("panels").classList.toggle("hidden", !session.patch);
    $("panels").inert = !ready;

    renderFiles(ready);
    renderDx7(ready);

    const s = session.lastStore;
    const result = $("store-result");
    result.className = "store-result";
    if (!s) result.textContent = "";
    else if (s.result === null) result.textContent = "Storing…";
    else if (s.result === STORE_OK) {
        result.textContent = `Stored in user slot ${s.slot}`;
        result.classList.add("ok");
    } else {
        result.textContent = `Store failed: ${storeResultText(s.result)}`;
        result.classList.add("failed");
    }
}

// --- Files and banks ---------------------------------------------------------

function download(bytes, name) {
    const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
}

const safeName = (s) => s.trim().replace(/[^A-Za-z0-9._-]+/g, "_") || "patch";

function renderFiles(ready) {
    const t = session.transfer;
    const busy = session.transferring();
    $("files").classList.toggle("hidden", !session.tables || !session.patch);
    for (const id of ["export-patch", "import-patch", "import-dx7", "backup", "restore"]) $(id).disabled = !ready || busy;
    $("import-dx7").classList.toggle("hidden", session.device?.module !== t00t.dx7.MODULE_FM);

    const result = $("file-result");
    result.className = "store-result";
    if (t && (busy || t.error || t.done)) {
        if (t.error) {
            result.textContent = t.error;
            result.classList.add("failed");
        } else if (busy) {
            const ofTotal = t.total === null ? "" : ` of ${t.total}`;
            const doing = { backup: "Backing up", restore: "Restoring", import: "Storing voices" }[t.kind];
            result.textContent = `${doing}: slot ${t.count}${ofTotal}…`;
        } else {
            result.textContent = {
                backup: `Backed up ${t.total} slots`,
                restore: `Restored ${t.total} slots`,
                import: t.total ? `Stored ${t.total} voices in user slots ${t.first} to ${t.first + t.total - 1}` : "",
            }[t.kind];
            result.classList.add("ok");
        }
    } else if (fileMessage) {
        result.textContent = fileMessage.text;
        if (fileMessage.ok !== null) result.classList.add(fileMessage.ok ? "ok" : "failed");
    } else {
        result.textContent = "";
    }

    // A finished backup is saved once; a stalled transfer is given up.
    if (t?.kind === "backup" && t.done && t.file) {
        const module = MODULE_NAMES[session.device.module] ?? `module${session.device.module}`;
        download(t.file, `t00t-${module}-bank-${new Date().toISOString().slice(0, 10)}.t0b`);
        t.file = null;
    }
    if (busy && (transferSeen?.t !== t || transferSeen.count !== t.count)) {
        clearTimeout(transferTimer);
        transferSeen = { t, count: t.count };
        transferTimer = setTimeout(() => session.abortTransfer(
            `The device stopped answering at slot ${t.count}. Check the connection and try again.`),
            TRANSFER_TIMEOUT_MS);
    } else if (!busy) {
        clearTimeout(transferTimer);
        transferSeen = null;
    }
}

// The imported DX7 file's bar: its voices, and storing them all.
function renderDx7(ready) {
    const shown = !!dx7Import && session.device?.module === t00t.dx7.MODULE_FM && !!session.patch;
    $("dx7").classList.toggle("hidden", !shown);
    if (!shown) return;
    const n = dx7Import.voices.length;
    $("dx7-voice").disabled = $("dx7-store").disabled = !ready || session.transferring();
    $("dx7-store").textContent = n === 1 ? "Store the voice" : `Store all ${n} voices`;
    $("dx7-slot").max = String(t00t.sysex.USER_PATCH_SLOTS - n);
}

// Make imported voice `i` the copy and play it on the device.
function audition(i) {
    followName();
    const error = session.importPatch(dx7Import.voices[i].stored);
    fileMessage = error ? { text: error, ok: false } : null;
    render();
}

async function readFile(inputEl) {
    const file = inputEl.files[0];
    inputEl.value = "";
    return file ? { name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) } : null;
}

// --- Wiring -------------------------------------------------------------------

// The Name box follows the copy's name until the user types in it, and
// again after anything that sets the name.
const followName = () => { delete $("name").dataset.edited; };

$("connect").onclick = connect;
$("discard").onclick = () => session.discard();
$("part").onchange = () => {
    followName();
    session.selectPart(Number($("part").value));
};
$("reload").onclick = () => {
    followName();
    session.reload();
};
$("push").onclick = () => session.push();
$("name").oninput = () => { $("name").dataset.edited = "1"; };
$("store").onclick = () => {
    followName();
    session.store(Number($("slot").value), $("name").value);
};

$("export-patch").onclick = () => {
    const bytes = session.exportPatch();
    if (bytes) download(bytes, `${safeName(session.patch.name)}.t0p`);
};
$("import-patch").onclick = () => $("patch-file").click();
$("patch-file").onchange = async () => {
    const file = await readFile($("patch-file"));
    if (!file) return;
    session.transfer = null;
    followName();
    const error = session.importPatch(file.bytes);
    fileMessage = error ? { text: error, ok: false } : { text: `Imported ${file.name}`, ok: true };
    render();
};
$("import-dx7").onclick = () => $("dx7-file").click();
$("dx7-file").onchange = async () => {
    const file = await readFile($("dx7-file"));
    if (!file) return;
    session.transfer = null;
    const result = t00t.dx7.parseSyx(file.bytes);
    if (result.error) {
        fileMessage = { text: `${file.name}: ${result.error}`, ok: false };
        render();
        return;
    }
    dx7Import = { name: file.name, voices: result.voices };
    $("dx7-name").textContent = file.name;
    $("dx7-voice").replaceChildren(...result.voices.map((v, i) => new Option(`${i + 1} ${v.name}`, String(i))));
    audition(0);
};
$("dx7-voice").onchange = () => audition(Number($("dx7-voice").value));
$("dx7-store").onclick = () => {
    const first = $("dx7-slot").value === "" ? NaN : Number($("dx7-slot").value);
    const n = dx7Import.voices.length;
    const refused = Session.slotsProblem(n, first);
    if (refused) {
        fileMessage = { text: refused, ok: false };
        render();
        return;
    }
    const where = n === 1 ? `user slot ${first}` : `user slots ${first} to ${first + n - 1}`;
    if (!confirm(`Store ${n === 1 ? "the voice" : `the ${n} voices`} of ${dx7Import.name} into ${where}? ` +
                 "What those slots hold is replaced.")) return;
    const error = session.storeVoices(dx7Import.voices.map((v) => v.stored), first);
    fileMessage = error ? { text: error, ok: false } : null;
    render();
};
$("dx7-close").onclick = () => {
    dx7Import = null;
    render();
};
$("backup").onclick = () => {
    fileMessage = null;
    session.backup();
};
$("restore").onclick = () => $("bank-file").click();
$("bank-file").onchange = async () => {
    const file = await readFile($("bank-file"));
    if (!file) return;
    fileMessage = null;
    if (!confirm(`Restore ${file.name} into the device's user bank? Every Patch and Multi slot it holds is ` +
                 "replaced, empty ones emptied.")) return;
    session.restore(file.bytes);
};

start();
})(globalThis.t00t ??= {});
