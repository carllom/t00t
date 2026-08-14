#!/usr/bin/env bash
# rig.h's FM_RIG_NOT_IN_FLASH=1 forces op_render/op_render_first/op_render_fb
# out of flash into a real out-of-line SRAM copy via the pico-sdk's
# __no_inline_not_in_flash_func. A new =2 value is a noinline-but-still-flash
# control, so diffing =2 against =1 isolates the SRAM-vs-flash placement
# effect alone, without also mixing in the added call/return overhead.
#
# Numbered 14/15, continuing the original sweep's manifest rather than
# renumbering it.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="/mnt/c/Users/carl/Documents/repos/t00t"

cd "$REPO_DIR"

declare -a NAMES
declare -a FLAGS

add_case() { NAMES+=("$1"); FLAGS+=("$2"); }

add_case "notinflash_ctrl_flash" "FM_RIG_VOICES=16 FM_RIG_NOT_IN_FLASH=2"
add_case "notinflash_sram"       "FM_RIG_VOICES=16 FM_RIG_NOT_IN_FLASH=1"

mkdir -p "$OUT_DIR"
MANIFEST="$OUT_DIR/t00t_fm_rig_manifest.txt"

start=14
total=${#NAMES[@]}
for i in "${!NAMES[@]}"; do
    n=$((start + i))
    name="${NAMES[$i]}"
    flags="${FLAGS[$i]}"
    padded=$(printf "%02d" "$n")

    echo "=== [$padded] $name  (make ENGINE=fm FM_PROFILE=1 $flags) ==="
    rm -rf build
    # Intentionally unquoted: $flags is a set of separate VAR=val make args.
    # shellcheck disable=SC2086
    make ENGINE=fm FM_PROFILE=1 $flags

    dest="$OUT_DIR/t00t_fm_rig_${padded}.uf2"
    cp build/t00t.uf2 "$dest"
    echo "  -> $dest"
    echo "${padded}  ${name}  FM_PROFILE=1 ${flags}" >> "$MANIFEST"
done

echo
echo "$total builds done (14-15, appended to the existing manifest)."
echo "Manifest: $MANIFEST"
cat "$MANIFEST"
echo
echo "Flash 14 then 15, read GPIO 22 duty cycle for each. 15 minus 14 is the"
echo "isolated SRAM-vs-flash cost for the operator kernel."
