#!/usr/bin/env bash
# Fetches Dexed's synthesis core (Source/msfa/) into ./msfa/ at a pinned commit.
#
# Deliberately NOT vendored into the repo (msfa/ is gitignored). Dexed as a
# whole is GPL-3; the msfa files themselves carry Apache-2.0 headers (Google
# Inc. 2012-2013, music-synthesizer-for-android) with later GPL-era
# modifications by Pascal Gauthier. Fetching at a pinned SHA keeps that
# licensing question out of this repo entirely while staying byte-reproducible
# --- which is all the F0 reference rig needs. Nothing here is ever linked into
# the device firmware; it is host-only development tooling.
#
# See history_fm.md §3.1 (F0-a).
set -euo pipefail

# Pinned so a reference render is reproducible. Bump deliberately, never
# casually: changing this changes every baseline number in the scorecard.
DEXED_SHA=2e182b3db85c09083ab13c8b9b00565ce7d9ff85

cd "$(dirname "$0")"
BASE="https://raw.githubusercontent.com/asb2m10/dexed/${DEXED_SHA}/Source/msfa"
DEST=msfa

# tuning.{cc,h} are deliberately NOT fetched: they pull in JUCE, the Surge
# Tunings library and libMTSClient for microtuning support the DX7 never had.
# shim/tuning.h provides the standard-12-TET TuningState that Dx7Note actually
# needs (see its own comment for the log-frequency format).
FILES=(
  aligned_buf.h controllers.h dx7note.cc dx7note.h env.cc env.h exp2.cc exp2.h
  fm_core.cc fm_core.h fm_op_kernel.cc fm_op_kernel.h freqlut.cc freqlut.h
  lfo.cc lfo.h module.h pitchenv.cc pitchenv.h porta.cpp porta.h sin.cc sin.h
  synth.h
)

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
  printf '  %s\n' "$f"
  curl -sSf -o "$DEST/$f" "$BASE/$f"
done

# env.cc and controllers.h include "../Dexed.h" (the JUCE plugin's own header)
# but reference nothing from it -- verified by grep: in both files the only
# occurrence of "Dexed" is the #include line itself. Stripping it is what lets
# the synthesis core build standalone; the alternative (a stub Dexed.h in this
# directory) would be a confusingly-named empty file for the same effect.
for f in env.cc controllers.h; do
  sed -i 's|^#include "../Dexed.h"|/* ../Dexed.h include removed by fetch_dexed.sh -- unused, see script comment */|' "$DEST/$f"
done

printf '\nDexed msfa @ %s -> %s/\n' "$DEXED_SHA" "$DEST"
