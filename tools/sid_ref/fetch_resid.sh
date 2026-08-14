#!/usr/bin/env bash
# Fetches reSID's SID emulation core into ./resid/ at a pinned commit, and
# generates the siddefs.h that autoconf would otherwise produce.
#
# Deliberately NOT vendored into the repo (resid/ is gitignored). reSID is
# GPL-2-or-later (Dag Lem); t00t is not, and nothing here is ever linked into
# the device firmware -- this is host-only development tooling whose only job
# is to produce ground-truth WAVs and control-plane trajectories to diff
# against. Fetching at a pinned SHA keeps the licensing question out of this
# repo entirely while staying byte-reproducible, which is all a reference rig
# needs.
#
# See module_chip.md §11.1 (CHIP_STRICT) and §14 item 1 (P0/F0).
set -euo pipefail

# Pinned so a reference render is reproducible. Bump deliberately, never
# casually: changing this changes every baseline number in the scorecard, and
# reSID's 6581 filter and combined-waveform tables are exactly the things this
# module calibrates against.
#
# ef7873f is reSID 1.0-pre1 ("Corrected samples for 6581 pulse + sawtooth",
# 2023-05-20), the tip of daglem/reSID master. 1.0's filter model is the
# newer one (dac.h + spline.h + the measured 6581 distortion model), not
# 0.16's -- module_chip.md §5.1's cutoff LUT is sampled from this exact commit.
RESID_SHA=ef7873fc8c8379dc14cef8d9ccf9b3d34d0cc439

cd "$(dirname "$0")"
BASE="https://raw.githubusercontent.com/daglem/reSID/${RESID_SHA}/src"
DEST=resid

# The synthesis core, plus pot.{cc,h}: the paddle A/D converters are not in
# the audio path at all, but sid.h has POT members by value, so the class
# definition is needed to compile anything that instantiates a SID.
# Left out on purpose:
#   *.dat           the .h combined-waveform tables are generated from these
#                   by samp2src.pl upstream; we take the generated .h
#   Makefile.am, configure.ac, INSTALL, ...  autotools scaffolding we replace
#                   with our own small Makefile
FILES=(
  dac.h envelope.cc envelope.h extfilt.cc extfilt.h filter.cc filter.h
  pot.cc pot.h sid.cc sid.h siddefs.h.in spline.h version.cc voice.cc voice.h
  wave.cc wave.h
  wave6581_PST.h wave6581_PS_.h wave6581_P_T.h wave6581__ST.h
  wave8580_PST.h wave8580_PS_.h wave8580_P_T.h wave8580__ST.h
)

mkdir -p "$DEST"
for f in "${FILES[@]}"; do
  printf '  %s\n' "$f"
  curl -sSf -o "$DEST/$f" "$BASE/$f"
done

# dac.h's DAC<bits>() no-arg constructor (a "temporary workaround" per its own
# FIXME) names the constructor with its own template-id repeating <bits> --
# accepted as GCC/Clang leniency historically, a hard parse error ("expected
# unqualified-id") on GCC 13 in C++20 mode. -Wno-template-id-cdtor (below) is
# what used to suppress this as a *warning*; on GCC 13 it isn't one, so the
# flag is a no-op and the build fails outright without this. Standard-
# conformant fix: drop the redundant <bits>, which inside the class body
# already refers to itself via the injected class name.
sed -i 's/DAC<bits>()/DAC()/' "$DEST/dac.h"

# siddefs.h is normally produced by `configure` substituting into
# siddefs.h.in. Doing it here rather than requiring autotools keeps the rig to
# "curl + make". The values match configure.ac's defaults for a modern g++:
# inlining on, branch hints on, FPGA code off, and the C++20 spellings of the
# constant-evaluation keywords (see configure.ac lines 30-129). RESID_INLINE
# must be `inline`, not empty: the .cc files include their own inline bodies
# from the headers, so without it every header body is emitted in every
# translation unit and the link fails on duplicate symbols.
sed -e 's/@RESID_INLINING@/1/'            \
    -e 's/@RESID_INLINE@/inline/'         \
    -e 's/@RESID_BRANCH_HINTS@/1/'        \
    -e 's/@RESID_FPGA_CODE@/0/'           \
    -e 's/@RESID_CONSTEVAL@/consteval/'   \
    -e 's/@RESID_CONSTEXPR@/static constexpr/' \
    -e 's/@RESID_CONSTINIT@/constinit/'   \
    -e 's/@HAVE_BUILTIN_EXPECT@/1/'       \
    "$DEST/siddefs.h.in" > "$DEST/siddefs.h"

printf '\nreSID @ %s -> %s/\n' "$RESID_SHA" "$DEST"
printf 'siddefs.h generated (C++20; build with -std=c++20).\n'
