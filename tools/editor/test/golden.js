// The firmware's golden SysEx vectors, fetched once.

import { parseGolden } from "./harness.js";

let cached;

export function golden() {
    cached ??= fetch("../../host_render/golden/sysex_subtractive.txt")
        .then((r) => r.text())
        .then(parseGolden);
    return cached;
}
