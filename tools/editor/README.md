# t00t editor

A static Web MIDI page for editing a t00t module on the patch platform (`docs/platform.md`)
with live preview from the device. It knows no module: it builds its controls from the
descriptor dump the device sends, so any module on the platform is editable without
changing the editor.

## Running it

Open `index.html` in a Chromium browser (Chrome, Edge, Brave, Opera), straight from the
file system; no server is needed. The scripts are classic scripts, not ES modules, since
Chromium won't load module scripts from `file://`.

The browser asks to allow MIDI with SysEx; the editor needs it. Firefox needs its Web MIDI
site-permission add-on and is not tested; Safari has no Web MIDI. A browser without Web
MIDI gets a message saying so.

## Using it

1. Pick the t00t in the device list (a port named T00T is chosen by default) and
   **Connect**. The editor identifies the device (module, schema, protocol), reads its
   descriptors and loads Part 1's Edit buffer.
2. Each descriptor group becomes a panel; ENUM parameters with labels get a list, the rest
   a slider (exponential where the descriptor says so) and a box for exact values. Every
   change is sent to the device as you make it. Parameters marked "applies from the next
   note" don't change notes already sounding.
3. **Part** switches to another Part and loads its Edit buffer.
4. The editor keeps its own copy of the Patch. If the Patch on the device changes under it
   (a Program Change, an edit on the device), **Push to device** sends the copy back, and
   **Reload from device** replaces the copy with the device's Edit buffer.
5. **Store** sends the copy, then stores it into the user slot given, under the name given.
   The device's answer (stored, no such slot, flash write failed) shows beside it.

If the device goes away and comes back, the editor reconnects and keeps its copy. If the
device is then a different module or schema than the copy was made for, the editor says
so and sends nothing to it until you **Discard editor copy**. A device speaking another
SysEx protocol version is reported and left alone.

## Files

- `index.html` — the page and its styles; loads the three scripts below in order.
- `app.js` — Web MIDI ports, the generic panels, rendering.
- `session.js` — the editing session: identify, descriptor dump, Edit buffer sync,
  parameter streaming, push, Store, mismatch handling. Takes and returns SysEx bytes;
  knows nothing of Web MIDI or the page.
- `sysex.js` — the SysEx codec, the stored Patch format and the descriptor codes.
- `test/` — the tests, run in the browser; `run_tests.py` runs them headless.

## Tests

```bash
python3 tools/editor/run_tests.py [chromium binary]
```

Serves the repository on a local port (the tests fetch the golden vectors, which a
`file://` page can't), loads `test/index.html` in headless Chromium and prints one line per
test. The codec tests check `sysex.js` against
`tools/host_render/golden/sysex_subtractive.txt`, the same golden byte vectors the
firmware's `test_sysex` uses, so editor and firmware can't drift apart. The session tests
replay the device's golden replies through `session.js` and check what it sends back.
