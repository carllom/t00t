#!/usr/bin/env bash
# Fetches the eight DX7 factory ROM sysex banks into ./banks/ (gitignored).
#
# Downloaded rather than committed for the same reason msfa/ is (fetch_dexed.sh):
# it keeps a redistribution question out of this repo. The banks are the DX7's
# own factory presets, mirrored by yamahablackboxes.com.
#
# Each file is a 4104-byte sysex bulk dump (F0 43 09 20 00 <4096> <cksum> F7)
# holding 32 voices; both dexed_render and tools/syx2patch.py accept that form
# directly.
#
# See fm2.md §3.3.
set -euo pipefail

cd "$(dirname "$0")"
BASE="https://yamahablackboxes.com/patches/dx7/factory"
DEST=banks

mkdir -p "$DEST"
for b in rom1a rom1b rom2a rom2b rom3a rom3b rom4a rom4b; do
  printf '  %s.syx' "$b"
  curl -sSf -o "$DEST/$b.syx" "$BASE/$b.syx"
  printf ' (%s bytes)\n' "$(stat -c%s "$DEST/$b.syx")"
done

printf '\nDX7 factory banks -> %s/\n' "$DEST"
