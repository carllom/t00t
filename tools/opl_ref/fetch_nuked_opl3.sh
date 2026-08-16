#!/usr/bin/env bash
# Fetches Nuked-OPL3 (nukeykt/Nuked-OPL3) into ./nuked/ at a pinned commit.
#
# Deliberately NOT vendored into the repo (nuked/ is gitignored) -- it's
# LGPL-2.1, a copyleft license, not something this repo wants to commit
# third-party source under. Fetching at a pinned SHA keeps that licensing
# question out of this repo while staying byte-reproducible. Nothing here is
# ever linked into the device firmware -- host-only development tooling.
set -euo pipefail

# Pinned so a reference render is reproducible. Bump deliberately, never
# casually: changing this changes every baseline number a future diff scores
# against.
NUKED_SHA=cfedb09efc03f1d7b5fc1f04dd449d77d8c49d50

cd "$(dirname "$0")"
BASE="https://raw.githubusercontent.com/nukeykt/Nuked-OPL3/${NUKED_SHA}"
DEST=nuked

# Nuked-OPL3 is two files plus its license, with no external dependencies
# beyond <inttypes.h> -- no shim headers are needed.
FILES=(opl3.c opl3.h LICENSE)

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
  printf '  %s\n' "$f"
  curl -sSf -o "$DEST/$f" "$BASE/$f"
done

printf '\nNuked-OPL3 @ %s -> %s/\n' "$NUKED_SHA" "$DEST"
