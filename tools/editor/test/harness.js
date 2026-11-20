// A minimal test harness for the editor: named checks in the style of the
// host tests ("ok   ..." / "FAIL ..."), run in the browser by test/index.html.

const tests = [];

export function test(name, fn) {
    tests.push({ name, fn });
}

export class CheckFailed extends Error {}

function show(v) {
    if (v instanceof Uint8Array || (Array.isArray(v) && v.every((b) => Number.isInteger(b)))) {
        return Array.from(v, (b) => b.toString(16).toUpperCase().padStart(2, "0")).join(" ");
    }
    return JSON.stringify(v);
}

function same(a, b) {
    if (a instanceof Uint8Array) a = Array.from(a);
    if (b instanceof Uint8Array) b = Array.from(b);
    return JSON.stringify(a) === JSON.stringify(b);
}

export function eq(actual, expected, what) {
    if (!same(actual, expected)) {
        throw new CheckFailed(`${what}\n       expected ${show(expected)}\n       actual   ${show(actual)}`);
    }
}

export function ok(cond, what) {
    if (!cond) throw new CheckFailed(what);
}

// The golden SysEx vectors (tools/host_render/golden/sysex_subtractive.txt)
// as { name: Uint8Array }.
export function parseGolden(text) {
    const out = {};
    for (const line of text.split("\n")) {
        if (!line || line.startsWith("#")) continue;
        const [name, ...hex] = line.trim().split(/\s+/);
        out[name] = Uint8Array.from(hex, (h) => parseInt(h, 16));
    }
    return out;
}

// Runs every test, one line per test; returns whether all passed.
export async function run(log) {
    let allOk = true;
    for (const { name, fn } of tests) {
        try {
            await fn();
            log(`ok   ${name}`);
        } catch (e) {
            allOk = false;
            log(`FAIL ${name}${e instanceof CheckFailed ? "\n       " + e.message : "\n       " + (e.stack || e)}`);
        }
    }
    return allOk;
}
