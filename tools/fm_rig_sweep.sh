#!/usr/bin/env bash
# Builds every voice-count/lever combination for the FM engine's benchmark
# procedure, one UF2 per combination, so the only thing left at the bench is
# reflash + read GPIO 22 for each file in order. Run from anywhere; paths are
# resolved relative to this script's location.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="/mnt/c/Users/carl/Documents/repos/t00t"

cd "$REPO_DIR"

declare -a NAMES
declare -a FLAGS

add_case() { NAMES+=("$1"); FLAGS+=("$2"); }

# 1. Voice-count sweep (defaults otherwise) -- per-voice/per-operator cost.
add_case "voices1"    "FM_RIG_VOICES=1"
add_case "voices2"    "FM_RIG_VOICES=2"
add_case "voices4"    "FM_RIG_VOICES=4"
add_case "voices8"    "FM_RIG_VOICES=8"
add_case "voices16"   "FM_RIG_VOICES=16"
add_case "voices24"   "FM_RIG_VOICES=24"

# 2. Lever bake-off, each vs. the voices16 baseline above.
add_case "interleave" "FM_RIG_VOICES=16 FM_RIG_INTERLEAVE=1"
add_case "notinflash" "FM_RIG_VOICES=16 FM_RIG_NOT_IN_FLASH=1"
add_case "block8"     "FM_RIG_VOICES=16 FM_RIG_BLOCK=8"
add_case "block32"    "FM_RIG_VOICES=16 FM_RIG_BLOCK=32"
add_case "table1024"  "FM_RIG_VOICES=16 FM_RIG_TABLE_BITS=10"
add_case "smulwb"     "FM_RIG_VOICES=16 FM_RIG_SMULWB=1"
add_case "fb_off"     "FM_RIG_VOICES=16 FM_RIG_FB=0"

mkdir -p "$OUT_DIR"
MANIFEST="$OUT_DIR/t00t_fm_rig_manifest.txt"
: > "$MANIFEST"

total=${#NAMES[@]}
for i in "${!NAMES[@]}"; do
    n=$((i + 1))
    name="${NAMES[$i]}"
    flags="${FLAGS[$i]}"
    padded=$(printf "%02d" "$n")

    echo "=== [$padded/$total] $name  (make ENGINE=fm FM_PROFILE=1 $flags) ==="
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
echo "All $total builds done."
echo "Manifest (build number -> config): $MANIFEST"
cat "$MANIFEST"
echo
echo "Flash each t00t_fm_rig_<NN>.uf2 in order (Pico in BOOTSEL mode, drag onto"
echo "RPI-RP2), read the GPIO 22 duty cycle, and record it against that number."
