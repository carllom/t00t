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
2. Each descriptor group becomes a panel; ENUM parameters with labels get a list, BITS
   parameters (a set of named flags in one value) a checkbox per flag, the rest a slider
   (exponential where the descriptor says so) and a box for exact values. Every change is
   sent to the device as you make it. Parameters marked "applies from the next
   note" don't change notes already sounding.
3. **Part** switches to another Part and loads its Edit buffer.
4. The editor keeps its own copy of the Patch. If the Patch on the device changes under it
   (a Program Change, an edit on the device), **Push to device** sends the copy back, and
   **Reload from device** replaces the copy with the device's Edit buffer.
5. **Store** sends the copy, then stores it into the user slot given, under the name given.
   The device's answer (stored, no such slot, flash write failed) shows beside it.
6. **Export Patch** saves the copy as a Patch file (`.t0p`, the stored Patch as it is);
   **Import Patch…** loads one into the copy and pushes it to the device. A Patch for
   another module is refused; one from an older schema takes defaults for what it lacks.
7. **Back up bank** saves the device's whole user bank (every Patch and Multi slot) as a
   bank file (`.t0b`). **Restore bank…** writes a bank file back, after asking: every slot
   the file holds is replaced and its empty slots emptied, one slot at a time, each sent
   once the device has answered the last (so it is safe over DIN too). A file for another
   module is refused; a slot the device refuses stops the restore and is named. Restoring
   a backup to the same module gives back exactly what was backed up. Editing, Store and
   Part switching wait while a restore runs, since the device may be writing flash. Formats:
   `docs/platform.md`.
8. On an FM device, **Import DX7 .syx…** loads a DX7 32-voice bank or single-voice dump.
   Its voices are converted to FM Patches exactly as the factory banks are
   (`tools/syx2patch.py`) and listed; the voice chosen becomes the editor's copy and plays
   on the device, ready to tweak and **Store** like any other. **Store all 32 voices**
   (or **Store the voice**) writes every voice of the file into the user slots from the
   one given, in order, after asking, one slot at a time as a restore does; a slot the
   device refuses stops it and is named.

If the device goes away and comes back, the editor reconnects and keeps its copy. If the
device is then a different module or schema than the copy was made for, the editor says
so and sends nothing to it until you **Discard editor copy**. A device speaking another
SysEx protocol version is reported and left alone.

## Files

- `index.html` — the page and its styles; loads the four scripts below in order.
- `app.js` — Web MIDI ports, the generic panels, rendering.
- `session.js` — the editing session: identify, descriptor dump, Edit buffer sync,
  parameter streaming, push, Store, Patch import/export, bank backup and restore,
  mismatch handling. Takes and returns SysEx bytes; knows nothing of Web MIDI or the page.
- `files.js` — the bank file format.
- `dx7.js` — DX7 `.syx` dumps to stored FM Patches.
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
replay the device's golden replies through `session.js` and check what it sends back; the
bank tests check backup, restore and the bank file the same way. The DX7 tests check
`dx7.js` against `tools/host_render/golden/dx7_syx.txt`: synthetic DX7 dumps and the stored
Patches `tools/syx2patch.py` makes of them, written by `tools/dx7_vectors.py` and checked
current by `tools/test_syx2patch.py`, so the editor's import and the factory-bank converter
can't drift apart.
