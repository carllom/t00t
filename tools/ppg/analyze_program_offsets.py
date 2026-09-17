#!/usr/bin/env python3
"""
Statistical fingerprinting of the PPG Wave cassette "Programs" block, used to
hypothesize byte offsets of individual Program-record parameters.

Context: see docs/research/ppg-wave-23-sound-architecture-and-data-formats.md
("Preset/patch record structure" and the byte-offset addendum below it).

This script does NOT ship any decoded ROM/cassette data (that's gitignored
third-party content, see tools/ppg/README.md) -- it only operates on files
you decode yourself. Reproduce like this:

  1. Fetch and build ppgwavecass-0.5.4 (SourceForge, GPLv2) with gcc, unmodified:
       https://sourceforge.net/projects/ppgwavecass/files/ppgwavecass-0.5.4.tar.gz
     (the SourceForge "download" link serves an HTML interstitial to curl; grab
     the signed `downloads.sourceforge.net/...?ts=...` URL out of that page's
     own HTML and curl *that* instead.)
     `make` in the extracted directory.

  2. Extract the WAVs from tools/ppg/cassettes/{wave20,wave22,wave23}_fact.zip.
     wave23_fact.wav is a 22.05kHz capture and will NOT decode with the stock
     tool (it's tuned for 44.1kHz) -- linearly upsample it 2x to 44100Hz first
     (see the doc's "Preset/patch record structure" section for why this
     works: the tool counts whole samples per FSK half-cycle).

  3. Decode with the built tool:
       ppgwave22cassdecode w22_fact.wav w22_fact.bin
       ppgwave22cassdecode w23_fact_2x.wav w23_fact.bin
       ppgwave2cassdecode  w20_fact.wav w20_fact.bin
     Each output is [2-byte start addr][2-byte end addr][payload]['checksum
     and end-byte' trailer]. This script only reads the first 4+5120 bytes.

  4. Point DECODED_FILES below at your local decoded/*.bin paths (keep them
     out of the repo -- tools/ppg/ is entirely gitignored already) and run
     this script.

Usage:
    python3 analyze_program_offsets.py fingerprint [wave22|wave23|wave20]
    python3 analyze_program_offsets.py autocorr [wave22|wave23|wave20]
    python3 analyze_program_offsets.py presets [wave22|wave23]
"""
import sys
from collections import Counter

# Edit these paths to point at your own locally-decoded files (not committed).
DECODED_FILES = {
    "wave22": ("decoded/w22_fact.bin", 51, 100),
    "wave23": ("decoded/w23_fact.bin", 51, 100),
    "wave20": ("decoded/w20_fact.bin", 50, 102),
}

# Transcribed from "Original-presets for the PPG WAVE 2.2, September 1982"
# (PPG Wave 2.2 Owner's Manual, hermannseib.com w22omeng.pdf, last 2 pages --
# a factory-preset appendix not previously used in this project's research).
# Columns: PROG -> (KEYB, WAVET, INT "X"/"-", COMMENT). Blank manual rows
# (19,48,58,59,66,68,69) and reserved 80-89 (WAVE-TERM demo) are omitted.
PRESETS = {
    0: (0, 0, "X", "Polysynthesizer"),
    1: (0, 30, "-", "perc. Polysynth./ Vibra."),
    2: (0, 8, "-", "perc. polysynth./ Pianet"),
    3: (0, 8, "-", "perc. Polysynth./ Pianet"),
    4: (0, 0, "-", "Vibraphone with Tremolo"),
    5: (0, 0, "X", "Polysynth. short /nasal"),
    6: (0, 8, "-", "D 6 / perc. with Quinte"),
    7: (0, 8, "-", "Polysynth. short, detuned"),
    8: (0, 0, "-", "Resonance-bass"),
    9: (1, 6, "-", "Poly with wave-modulation"),
    10: (0, 8, "-", "A=E.Piano B=Click-Piano"),
    11: (1, 24, "-", "Organ with Clickattack"),
    12: (0, 24, "-", "A=E.Fender-Piano B=sustained, Tremolo"),
    13: (1, 12, "-", "Poly ,strong Filtersweep with emphasis"),
    14: (0, 28, "-", 'perc. "sync-sound"'),
    15: (0, 13, "-", "E.Piano ,Spinett."),
    16: (0, 13, "-", "percussiv DS"),
    17: (0, 8, "-", "Piano"),
    18: (0, 13, "-", "E. piano"),
    20: (0, 0, "X", "Synth"),
    21: (0, 28, "-", "Synth. with Pulswidthmod."),
    22: (0, 14, "X", "Synth / Hammond"),
    23: (0, 24, "X", "Synth percussiv u. contracussiv DS"),
    24: (0, 28, "-", "Synth long with Pulswidthmod."),
    25: (0, 27, "-", "Synth long with wavesweep"),
    26: (0, 9, "-", "percussiv with Resonance"),
    27: (1, 13, "-", "percussiv abstract"),
    28: (0, 2, "-", "percussiv bell"),
    29: (0, 4, "-", "percussiv hard, bell"),
    30: (0, 4, "-", "percussiv clear"),
    31: (0, 24, "-", "Space-bell long"),
    32: (1, 4, "-", "bell"),
    33: (0, 0, "-", "percussiv calm"),
    34: (0, 2, "-", "Xylophon"),
    35: (1, 2, "-", "Marimbaphon"),
    36: (0, 2, "-", "Vibraphon"),
    37: (0, 9, "-", "Poly / Poly with wavesweep"),
    38: (3, 26, "-", "Akkord Brass"),
    39: (0, 0, "-", "percussiv glass-like"),
    40: (1, 26, "X", "Brass 1"),
    41: (0, 26, "X", "Brass 2"),
    42: (1, 26, "-", "Trumpet"),
    43: (0, 18, "-", "perc. / DS long"),
    44: (1, 26, "X", "Brass 3 dark"),
    45: (0, 26, "X", "Brass long Attack"),
    46: (3, 26, "X", "Brass Akkord"),
    47: (1, 26, "X", "Brass mixed"),
    49: (0, 3, "-", "A u.B Flute"),
    50: (1, 16, "X", "Strings 1"),
    51: (1, 29, "X", "Strings 2"),
    52: (1, 29, "X", "Strings 3"),
    53: (1, 29, "-", "Strings 4"),
    54: (0, 29, "-", "Strings abstract"),
    55: (1, 29, "-", "Strings abstract"),
    56: (1, 28, "-", "Poly with sync-Effect"),
    57: (0, 15, "-", "Mouthorgan"),
    60: (0, 14, "-", "A=Organ B=Hammond with Percus."),
    61: (0, 14, "-", "Organ with Clickattack"),
    62: (0, 14, "-", "Hammond with Leslie"),
    63: (0, 14, "-", "Chirchorgan"),
    64: (0, 14, "-", "Organ"),
    65: (0, 13, "-", "percussiv bright"),
    67: (0, 14, "-", "Hammond"),
    70: (0, 27, "-", "Choir"),
    71: (1, 27, "-", "mixed Choir"),
    72: (1, 21, "-", "Choir abstract"),
    73: (1, 23, "-", "Choir abstract"),
    74: (0, 14, "-", "Filter Modulation"),
    75: (0, 17, "-", "Spacepoly / with wavesweep"),
    76: (0, 25, "-", "Effect"),
    77: (1, 16, "-", "Spacepoly"),
    78: (1, 20, "-", "Effectpoly"),
    79: (0, 26, "-", "Spacestrings"),
    90: (0, 17, "-", "Effektpoly"),
    91: (0, 24, "-", "Polysynth / Echosynth"),
    92: (0, 0, "-", "Poly soft"),
    93: (0, 0, "-", "Poly soft"),
    94: (0, 29, "-", "Effektpoly"),
    95: (0, 25, "-", "Echo"),
    96: (0, 18, "-", "sound-sweep"),
    97: (3, 13, "-", "Effekt"),
    98: (3, 13, "-", "Effekt"),
    99: (0, 0, "-", "percussive"),
}


def load_programs_block(path):
    with open(path, "rb") as f:
        data = f.read()
    startaddr = int.from_bytes(data[0:2], "big")
    endaddr = int.from_bytes(data[2:4], "big")
    assert (startaddr, endaddr) == (0x0400, 0x17FF), (hex(startaddr), hex(endaddr))
    payload = data[4:4 + 5120]
    assert len(payload) == 5120, len(payload)
    return payload


def split_records(payload, reclen, nrecs):
    recs = [payload[i * reclen:(i + 1) * reclen] for i in range(nrecs)]
    trailer = payload[nrecs * reclen:5120]
    return recs, trailer


def cmd_autocorr(name):
    path, reclen, nrecs = DECODED_FILES[name]
    payload = load_programs_block(path)
    n = len(payload)
    results = []
    for stride in range(2, 200):
        matches = sum(1 for i in range(n - stride) if payload[i] == payload[i + stride])
        results.append((matches / (n - stride), stride))
    results.sort(reverse=True)
    print(f"{name}: top byte-match-rate strides (period {reclen} expected):")
    for rate, stride in results[:10]:
        print(f"  stride {stride:4d}  rate {rate:.4f}")


def cmd_fingerprint(name):
    path, reclen, nrecs = DECODED_FILES[name]
    payload = load_programs_block(path)
    recs, trailer = split_records(payload, reclen, nrecs)
    print(f"{name}: {nrecs} records x {reclen} bytes, trailer ({len(trailer)}B) = {trailer.hex()}")
    print(f"{'off':>4} {'min':>4} {'max':>4} {'ndist':>6}  mode(freq)  distinct(<=20)")
    for off in range(reclen):
        vals = [r[off] for r in recs]
        c = Counter(vals)
        mv, mf = c.most_common(1)[0]
        dv = sorted(c)
        dv_str = ",".join(map(str, dv[:20])) + (",..." if len(dv) > 20 else "")
        print(f"{off:>4} {min(vals):>4} {max(vals):>4} {len(c):>6}  {mv:>3}({mf:>3})   {dv_str}")
    # Negative-result check flagged in the research doc: look for an 8-byte
    # run that stays within a narrow range anywhere in the record (candidate
    # SEMIT anchor / any raw 0-63-style dial value).
    maxes = [max(r[off] for r in recs) for off in range(reclen)]
    for thresh in (20, 40, 63):
        hits = [s for s in range(reclen - 7) if all(mx <= thresh for mx in maxes[s:s + 8])]
        print(f"8-consecutive-offset runs with max<={thresh}: {hits}")
    single = [off for off in range(reclen) if maxes[off] <= 63]
    print(f"single offsets with max<=63 (any field?): {single}")


def cmd_presets(name):
    path, reclen, nrecs = DECODED_FILES[name]
    payload = load_programs_block(path)
    recs, trailer = split_records(payload, reclen, nrecs)
    progs = sorted(PRESETS)
    print(f"{name}: cross-referencing {len(progs)} known (PROG,WAVET) pairs from the")
    print("PPG Wave 2.2 Owner's Manual's factory-preset appendix against every byte offset.")
    best = []
    for off in range(reclen):
        raw = sum(1 for p in progs if p < nrecs and recs[p][off] == PRESETS[p][1])
        masked = sum(1 for p in progs if p < nrecs and (recs[p][off] & 0x1F) == PRESETS[p][1])
        best.append((masked, raw, off))
    best.sort(reverse=True)
    print(f"{'offset':>6} {'masked(&0x1F)':>14} {'raw exact':>10}")
    for masked, raw, off in best[:10]:
        print(f"{off:>6} {masked:>10}/{len(progs)}   {raw:>6}/{len(progs)}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    names = sys.argv[2:] or list(DECODED_FILES)
    for name in names:
        if cmd == "autocorr":
            cmd_autocorr(name)
        elif cmd == "fingerprint":
            cmd_fingerprint(name)
        elif cmd == "presets":
            cmd_presets(name)
        else:
            print(f"unknown command {cmd!r}")
            sys.exit(1)
