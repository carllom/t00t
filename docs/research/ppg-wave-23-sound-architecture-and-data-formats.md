# Research: PPG Wave 2.3 sound architecture and data formats

Requested directly by the project owner (not tied to a GitHub issue): map (a) the PPG
Wave 2.3 sound architecture for internal-ROM waves (not the Waveterm/sample-import
path), and (b) the waveform/wavetable and preset/patch data structures as they sit in
ROM and on the factory cassette dumps — closing the gap `module_wavetable.md`'s own
Future/TODO section flags explicitly ("the per-patch record layout gets
reverse-engineered... unexplored"). `src/engines/wavetable/` is explicitly **not** a
PPG clone project; this is background for translating PPG-style sounds into that
engine's own terms, not a spec to implement against. No code under `src/`, `tools/`,
or `docs/` was changed except this file, other than a later follow-up session adding
one new analysis script, `tools/ppg/analyze_program_offsets.py` (statistical
byte-offset fingerprinting used in the "Preset/patch record structure — byte
offsets" section below; no decoded binary/cassette-derived data committed,
consistent with `tools/ppg/` already being entirely gitignored — see that section
for details).

## Primary sources

- **`tools/ppg/PPG Wave 2.3 version 6.zip`** — 7 raw EPROM chip dumps (`w23_64.bin`,
  `w23_66.bin`, `w23_68.bin`, `w23_6a.bin`, `w23_6c.bin`, `w23_6e0.bin`,
  `w23_6e1.bin`, each 8192 bytes). Inspected directly with throwaway Python (entropy,
  mean sample-to-sample delta, 64-byte block "wrap smoothness", and a sequential
  structural parser), all scripts and extracted files kept in this session's
  scratchpad, nothing added to the repo.
- **`tools/ppg/cassettes/{wave20,wave22,wave23}_fact.zip`** — 3 factory-sound cassette
  WAV recordings. Inspected directly (WAV header parsing, amplitude envelope,
  zero-crossing frequency measurement) and, per explicit scope expansion from the
  project owner, actually **decoded** (see next item) rather than only described.
- **[`ppgwavecass`](https://sourceforge.net/projects/ppgwavecass/)** (SourceForge,
  author Klaus Michael Indlekofer, GPLv2) — the community cassette decoder the
  existing `tools/ppg/README.md` names. Fetched the full `ppgwavecass-0.5.4` source
  tarball, built it locally and unmodified with `gcc`, and **ran it against all three
  real cassette WAV files**, producing genuine decoded binary "Programs" dumps
  (checksum- and end-byte-verified, not simulated). Source files read directly:
  `ppgwavecassdecode.c`, `ppgwavedefs.h`, `ppgwave22seqsparse.c`.
- **PPG Wave 2.2 Owner's Manual** (English), fetched as a PDF from
  [hermannseib.com](https://www.hermannseib.com/documents/w22omeng.pdf) and read in
  full (31 written pages, OCR'd via image inspection). This is the manufacturer's own
  1982 manual — the primary source for the front-panel/parameter model in section (a)
  and for the "100 programs" figure used in section (b). Pages 13-14 (the DIGITAL
  Display / modulation-matrix page) were re-fetched and re-read a second time,
  prompted by the project owner questioning the `UW` field's source letter `U`
  against the manual's own 5-letter SOURCES list — see the Modulation sources and
  matrix section below for what that check found. Re-fetched a third time in a later
  session specifically to hunt for a factory-preset parameter appendix (see the new
  "Preset/patch record structure — byte offsets" section below): the PDF is 36 pages
  total (the manual's own "31 written pages" undercounts a cover page, a blank, and —
  critically — two **unpaginated pages at the very end**, past the last numbered page
  33, titled *"Original-presets for the PPG WAVE 2.2, September 1982"*, a
  `PROG./KEYB./WAVET./INT./COMMENT` table for all 100 factory programs. Not used or
  mentioned anywhere in this doc's earlier passes; directly used below as ground
  truth to check a statistical hypothesis against.
- **PPG Wave 2.3 Service Manual**, fetched as a PDF from
  [deepsonic.ch](https://www.deepsonic.ch/deep/docs_manuals/ppg_wave_2.3_service_manual_b.pdf)
  and read (first 10 of 38 pages: introduction, board list, block diagram, adjustment
  procedure). Manufacturer's own service documentation — the primary source for the
  ROM-chip identification and voice-board hardware in section (b) and part of (a).
- **[`ppg.synth.net/wave22`](https://ppg.synth.net/wave22/)** — the same secondary
  community summary `module_wavetable.md` already cites and partially distrusts;
  fetched again here and found to disagree with the byte-verified ROM extraction in
  the same places already documented, plus one new count (see the comparison table
  below).
- **Sound on Sound, ["PPG Wave 2.3 & Waveterm B"](https://www.soundonsound.com/reviews/ppg-wave-23-waveterm-b)**
  — a detailed contemporary-ish professional review, fetched and used as
  corroboration for envelope/LFO/matrix terminology (it independently uses the same
  control names as the owner's manual, e.g. "Waves-Osc"/"Waves-Sub", which is a good
  sign it was written from hands-on use of the real control panel or manual, not
  guesswork). Treated as secondary throughout; every claim from it is cross-checked
  against the manufacturer manual or flagged as uncorroborated.
- **[hermannseib.com](https://www.hermannseib.com/english/synths/ppg/) PPG pages**
  (`wave.htm`, `msysex.htm`, `docs.htm`) — fetched; found to be mostly navigational
  hubs without the technical depth needed, noted explicitly rather than padded out.
- **[presetpatch.com/articles/ppg-wavetable-list](https://www.presetpatch.com/articles/ppg-wavetable-list)**
  — fetched per the existing `tools/ppg/README.md` reference list; a fourth,
  disagreeing wavetable-count data point, no byte-level detail.
- **[Seib, *PPG Wave ROM Waveforms and Wavetables*](https://www.hermannseib.com/documents/PPGWTbl.pdf)**
  (`PPGWTbl.pdf`, dated Jan 2005) — fetched at the project owner's pointer and read in
  full. A data-derived document, not prose commentary: it programmatically renders
  every stored waveform and every wavetable's full 64-slot sweep as small graphs
  (black header = taken directly from the waveform section, red header = the
  document's own 2-point interpolation between two stored waveforms, done by the
  document's generator, not the synthesizer). The primary source for resolving the
  "well over 2000 waveforms" claim (see below) and for a new, independently observed
  fixed-tail finding (see part (b)'s waveform section).
- `docs/module_wavetable.md`, `docs/logs/history_wavetable.md`, `tools/ppg/README.md`,
  `tools/host_render/render_ppg_waves.cpp` — this project's own existing
  documentation, used as context/baseline, not re-cited as new findings except where
  a new source corrects or extends a specific claim.

**Explicit non-source, per the task brief**: `tools/ppg/extract/w23_waves.bin` and
`extract/w23_wavetables.json`, which `convert_ppg_waves.py` reads per
`module_wavetable.md`, are **not present in this checkout** (that extraction happened
on a different machine). Nothing in this document claims to have reproduced that
specific conversion; the wavetable-index structure described in part (b) below was
independently re-derived directly from the raw ROM chip dump, by a different method,
in this session.

## (a) PPG Wave 2.3 sound architecture (internal ROM waves only)

### Voice and oscillator architecture

8 voices, 2 oscillators per voice, 16 oscillators total — stated directly in the
owner's manual's opening paragraph ("an eight-voice polyphonic Synthesizer that uses
sixteen digital oscillators") and confirmed independently by the service manual's
troubleshooting section, which calls the units under test "the 8 digital dual
oscillators" and describes a diagnostic that cycles voice allocation round-robin
("oscillator No. 1 is activated as soon as a key is pressed. Releasing the key and
pressing it a second time activates oscillator No. 2. This makes it possible to step
through the 8 oscillators one after the other" — p.2 of the service manual; note the
manual's own loose use of "oscillator" for "voice" in that one spot). The service
manual's hardware list confirms this is physically 2 identical "Voice Boards," 4
voices each.

The two oscillators in a voice are **not** a fixed sync/detune pair with one set of
shared parameters — they are two independently-programmable half-voices, called
**Group A** and **Group B** throughout the owner's manual and UI: "Each voice has two
possible sound sources: one stored in bank A and the other in Bank B" (Sound on
Sound). Every parameter display in the owner's manual (pp.9-17) is edited per-Group,
selected with a front-panel `GROUP` key, and a Program stores both groups' full
parameter sets together. A single Group additionally carries its own suboscillator
with a separate **detune** parameter (`DETU 0-7`: no detuning → very soft → soft →
detuning → strong detuning → fixed intervals of a fifth / one octave / two octaves —
owner's manual p.16) — this detune relationship is internal to one Group's
oscillator+suboscillator pair, not the Group-A/Group-B relationship itself. No
oscillator hard-sync is described anywhere in the owner's manual; treated as an open
question below rather than assumed absent.

**Keyboard/voice-allocation modes** (owner's manual pp.11-12, `KEYB` codes 0-8) let a
single Program's 16 oscillators be reassigned across fewer simultaneously-playable
notes, i.e. a genuine "unison" performance capability that changes voice *count per
note* without touching a Program's own stored parameters: mode 0 is normal 8-voice
polyphonic (2 osc/note); mode 1 is 4-voice with 4 osc/note (2+2 sub); mode 2 is
2-voice with 8 osc/note; mode 3 is monophonic with all 16 oscillators stacked on one
note. Modes 4-8 add a keyboard split point and assign different voice-counts to the
upper/lower halves (e.g. mode 6: monophonic 2-voice upper part, 6-voice polyphonic
lower part). In every mode, Group A always plays the odd-numbered voices and Group B
the even-numbered ones. None of this changes how a single Program's parameters are
interpreted — only how many physical oscillators a played note consumes.

### Envelope generators — 3 per voice, not a simple 1:1:1 mapping

Confirmed by both the owner's manual (pp.3, 7, 10, 16) and Sound on Sound
independently: **2 ADSR generators (Env 1, Env 2) plus one simpler Attack/Decay-only
generator (Env 3, called "AR" on p.3)** — no separate sustain stage on Env 3.

- **Envelope 2 → Loudness (VCA)**, a standard ADSR (owner's manual p.10).
- **Envelope 1 → filter cutoff *and* oscillator wave-position, simultaneously, from
  one shared envelope shape** with independently adjustable depths — quoted directly
  from the manual (p.10): *"The envelope 1 works in the same way but is controlling
  the CUTOFF frequency of the lowpass filter and the waveform of the oscillators (for
  dynamic soundchanges during the time of one played note). Up to 64 waveforms can be
  run trough, corresponding to the position of the control ENVELOPE-WAVES."* This is
  a specific correction worth carrying forward: it is **one** envelope feeding two
  destinations at independent depths, not two separate envelopes each dedicated to
  one destination.
- **Envelope 3 → pitch** of the oscillator and/or suboscillator by default (Tuning
  Display fields `EO`/`ES` = "envelope 3 to oscillator/suboscillator (pitch)", p.16,
  with a positive/negative depth control `ENV 3 ATT` centered at a no-effect
  midpoint) — **or**, if the suboscillator's `SW` parameter is set to code `2`,
  Envelope 3 instead drives the *suboscillator's* wave-position in place of Envelope
  1 (Digital Display, p.14: *"instead of envelope 1 the envelope 3 is now controlling
  the waveforms of the suboscillator"*). Envelope 3's destination is switchable, not
  fixed.
- **Real hardware quirk worth preserving as color**: the ADSR attack knob's range is
  piecewise. From 0-31 it behaves as an ordinary ADSR; from 31-63, decay and sustain
  are disabled entirely and release begins immediately after attack, giving an
  automatic AR-only envelope for long, percussive attack times (owner's manual p.10).

### Filter

SSM2044, one per voice — now confirmed directly by the service manual's own
Voice Board parts list (not just architectural inference as `module_wavetable.md`
previously flagged it): *"4 VCF ICs, model: SSM2044"* per board × 2 boards = 8, one
per voice. The same section also names the VCA chip, not previously documented in
this project: *"2 Double VCAs, model: CEM3360"* per board — a dual-VCA chip, 2 per
board × 2 boards = 4 chips / 8 VCAs, again one per voice. Both filter and VCA receive
their control voltages from dedicated D/A converters on the I/O board (service
manual: *"Three additional D/A converters that supply the power for the ADSR
envelopes of the VCA, the VCF and the Resonance which are all on the voice
cards"*) — i.e. even manually-set resonance is quantized/digitized like every other
front-panel control (owner's manual p.7-8), though no modulation-matrix route lets
LFO/velocity/envelope modulate resonance directly (see next section) — only cutoff is
matrix-modulatable.

### LFO

One LFO. Its `WAVESHAPE` knob is a single continuous sweep, not a discrete selector:
*"triangle, positive and negative sawtooth, and rectangular wave"* across one control
(owner's manual p.16), matching Sound on Sound's description of the same knob
passing "through various sawtooth wave shapes in between" triangle and square.
Controls: `RATE`, `WAVESHAPE`, `DELAY` (onset fade-in, for vibrato-style delayed
effect). Destinations are three on/off matrix bits — `MW` (waves), `MF` (filter),
`ML` (loudness) — all scaled by one shared modulation-wheel depth; there is no
per-destination LFO depth beyond that shared wheel amount.

### Modulation sources and matrix — fully enumerated, not just "known partially"

The owner's manual's "DIGITAL Display" section (pp.13-14) shows 16 two-letter fields
in its display layout — verbatim: `UWO SWO KWO KFO KLO MWO MFO MLO` /
`BDO BIO TWO TFO TLO TMO VFO VLO` (the trailing digit in each is the field's
currently-set value, not part of its code) — but **only 12 of those 16 are actual
Source×Destination modulation switches**; the other 4 are unrelated fields that
happen to reuse the same two-letter display convention. Re-fetched and re-read the
manual's own page image directly (not just a first pass) specifically to check this,
prompted by the project owner noticing `UW`'s source letter `U` doesn't appear
anywhere in the manual's own "SOURCES" list.

The manual states its SOURCES list explicitly as exactly five letters — **K**(eyboard
position), **M**(LFO), **T**(ouch/aftertouch), **V**(elocity), **B**(ender/pitch-wheel)
— and its DESTINATION list as exactly four — **W**(ave-position), **F**(ilter
cutoff), **L**(oudness/VCA), **M**(odulation intensity/depth itself, i.e. touch can
control how deep the LFO's effect is). `U` and `S` genuinely do not appear in either
list; `UW` and `SW` are not Source×Destination pairs at all, confirmed by the
manual's own following explanatory paragraphs (p.14), not inferred:

- **`UW`** — an on/off program-level flag: "by inserting a 1, you switch from normal
  wavetable to UPPER waves" (a special shared wavetable reachable from any program,
  distinct from the Program's own selected wavetable). `U` here is "Upper," not a
  modulation source.
- **`SW`** — a 4-value mode selector (codes 0-3) for the suboscillator's wave source:
  0 follows the main oscillator's wave-position plus its own `WAVES-SUB` offset, 1 is
  driven purely by `WAVES-SUB`, 2 hands wave-position control to Envelope 3 instead of
  Envelope 1, 3 switches the suboscillator off. Not a Source×Destination pair either.

The 12 genuine switches split unevenly by source: `K` and `M` each cover all three
non-modulation-intensity destinations (`KW KF KL`, `MW MF ML`); `T` covers all four,
including modulating modulation-intensity itself (`TW TF TL TM`); `V` covers only two
— **`VF` and `VL`, with no `VW`** — velocity cannot modulate wave-position on real
hardware, confirmed directly both by the absence of `VW` in the display layout and by
the manual's own per-field explanation section not describing one. `B` (bender) does
not follow the single-letter-destination pattern at all: it gets two dedicated
selector fields instead, `BD` (destination selector: off / pitch / filter / waves /
sub-pitch / pitch+filter / pitch+waves / filter+waves) and `BI` (interval selector:
2nd / 3rd / 5th / octave, p.15) — so the bender wheel is not restricted to pitch
either, it's just switched by a different mechanism than K/M/T/V.

**Specific, citable correction to this project's existing documentation**:
`module_wavetable.md`'s PPG Architecture Reference previously listed wave-position
modulation sources as "envelope, velocity, mod wheel, aftertouch" (sourced from
`ppg.synth.net`). The corrected, complete set of wave-position modulation sources is:
keyboard-tracking (`KW`), LFO (`MW`), aftertouch (`TW`), Envelope 1 (fixed routing,
separate from this matrix entirely, via the `ENVELOPE-WAVES` depth knob), and — for
the suboscillator specifically — Envelope 3 in place of Envelope 1 (`SW=2`). Velocity
is not in this list under any field name.

### Wave-position scanning — the defining mechanism

Multiple, independently-enableable sources sum into the same 0-63 wave-position value
an oscillator reads within its current wavetable:

1. **Manual**: `WAVES-OSC` picks the wavetable (0-30), `WAVES-SUB` sets the starting
   wave position (0-63).
2. **Keyboard-tracking** (`KW`, depth 0-7): the key played offsets the position —
   *"every key higher calls up one wave backward through the wavetable"* (p.14).
3. **Envelope 1** (`ENVELOPE-WAVES` depth): sweeps up to all 64 waveforms during one
   held note, simultaneously with filter cutoff (see Envelopes above).
4. **LFO** (`MW` on/off): adds a wheel-scaled cyclic wobble.
5. **Aftertouch** (`TW` on/off): key-pressure-driven offset.
6. **Suboscillator** has its own independent routing choice (`SW` 0-3): tied to the
   main oscillator's position plus its own `WAVES-SUB` offset, driven purely by
   `WAVES-SUB` alone, driven by Envelope 3 instead of Envelope 1, or switched off.

Velocity is explicitly **not** one of these sources (see matrix correction above).
This multi-source-summed control value is the single most PPG-defining mechanism,
and conceptually matches this project's own `osc/wavetable.h` wave-position axis
(`module_wavetable.md`'s Glossary) — real hardware treats it as several simultaneous
contributing inputs, not one modulation slot.

### Where sources conflict, and which is trusted

Extending `module_wavetable.md`'s own comparison table with two more counts observed
directly in this pass:

| Claim | Our ROM extraction (trusted) | Owner's manual | Sound on Sound | ppg.synth.net | presetpatch.com |
|---|---|---|---|---|---|
| Waveform size | **64 bytes/samples** (byte-verified) | not stated in bytes | "256 bytes" | "128" | not stated |
| Wavetable count | **29** (byte-verified) | "30" | "30" | "32" | "31" |
| Programs stored | **100** (see part b, cross-checked two ways) | **"100"** (explicit) | "up to 87" | not stated | not stated |

Waveform size and wavetable count: continue trusting the byte-verified ROM
extraction, per the precedent `module_wavetable.md` already set (every secondary
source disagrees with every other secondary source too, which is itself evidence
none of them re-derived these numbers from the ROM). Program count is the one place
this pass adds real independent corroboration rather than just another citation: the
owner's manual's plain statement ("all 100 sound programs are dumped onto cassette,"
p.29) and this session's own from-scratch autocorrelation analysis of the actually
decoded cassette data (part b, below) arrive at the same number, **100**, by two
unrelated methods. Sound on Sound's "87" is not corroborated by either and is treated
as either model-specific or an imprecise recollection.

**Resolving the "well over 2000 waveforms" marketing figure** (raised directly by the
project owner: could this instead mean some waveforms are *primitively synthesized* —
plain sine/saw/triangle/pulse generated by formula — rather than sampled, and could
that explain the 244-vs-thousands count gap?): no. Seib's own more detailed technical
document is explicit about the mechanism, and it is not primitive-waveform synthesis:
*"Since the PPG Wave only has a set of about 250 waveforms, most of the wavetables'
contents are interpolated between 2 of the waveforms. This interpolation, done for
all wavetables, gives the marketing-hype-number of 'over 1800 waveforms!' available in
the instrument."* (`PPGWTbl.pdf`, p.1). That is exactly the same two-keyframe
interpolation mechanism already confirmed in this project's own ROM extraction
(sparse authored keyframes blended across a 0-63 wave-position range) and already
implemented, unmodified, by `osc/wavetable.h` — not a distinct population of
algorithmically-generated basic waveforms. The `ppg.synth.net` page's "well over 2000"
figure is the same claim with a rounder, slightly different table-count assumption
(32×64=2048) plugged into the same "sweep every position of every table" arithmetic —
both writers appear to be the same author (Hermann Seib) restating one figure two
ways, not two independent counts.

That said, direct visual inspection of `PPGWTbl.pdf`'s per-table renderings (not
mentioned in its own prose) does surface something adjacent to the project owner's
instinct — see part (b)'s "Universal fixed-tail waveforms" finding below: the last
four slots of every rendered wavetable are always the same four *primitive-looking*
shapes (triangle, two pulse/square variants, sawtooth), and they are marked as
directly-stored data, not interpolated. That is a real, if narrower, sense in which
recognizable "basic" waveforms are part of the ROM data — just not the explanation
for the 244-vs-2000 count gap, which is fully accounted for by interpolation alone.

## (b) Waveform and preset/patch data structures

### ROM chip identification (new — not previously documented in this repo)

The 7 raw EPROM dumps (`w23_64.bin`, `w23_66.bin`, `w23_68.bin`, `w23_6a.bin`,
`w23_6c.bin`, `w23_6e0.bin`, `w23_6e1.bin`, each 8192 bytes — a 2764-class 8K×8
EPROM) were analyzed byte-by-byte (entropy, mean sample-to-sample delta, and a
64-byte-block "wrap smoothness" test meant to separate PCM audio from 6809 machine
code). `w23_64.bin` and `w23_66.bin` stood out sharply from the other five — entropy
≈7.5 and mean delta ≈10-14 vs. ≈50-99 for the rest — consistent with either PCM audio
or small lookup tables rather than opcodes.

This matches, exactly, the PPG Wave 2.3 Service Manual's own component list (fetched
directly, not inferred): *"the machine program... is stored in four 2764 E-PROMS.
They are plugged into sockets on the I/O BOARD and labeled 8, A, C, E... Next to them
you find the 2 E-PROMS labeled 4 and 6 which contains the WAVEFORM DATA"* (p.2), with
a photo of the I/O board captioned "E-Proms 4 and 6 = Wave tables" / "E-Proms 8,A,C,E
= Operating Instructions." The dump filenames' `_64`/`_66`/`_68`/`_6a`/`_6c`/`_6e`
suffixes are exactly these single-character manual labels (`4,6,8,A,C,E`) with a
shared naming prefix: EPROM `4` → `w23_64.bin`, EPROM `6` → `w23_66.bin` (wave data —
matches the entropy/smoothness result independently), and `8,A,C,E` → the remaining
four (firmware). `w23_6e0.bin`/`w23_6e1.bin` are two dumps of the single EPROM
labeled `E`: they differ in only 3 of 8192 bytes, all three flips clustered at file
offset `0x0FFA-0x0FFC` — roughly the middle of the chip, not the 6809
reset/interrupt-vector region that would sit at the top of a memory-mapped 8K page —
consistent with a marginal-bit read error on one of the two dump passes rather than
two genuinely different ROM revisions.

Going beyond what the manual documents (it names which EPROMs hold wave data but not
their internal layout), sequentially parsing `w23_64.bin` from offset 0 as a stream
of `[table_number: 1 byte]` followed by repeated `[waveform_id: 1 byte][slot: 1
byte]` pairs, terminating each table when `slot == 0x3C`, decodes **tables 0
through 16 cleanly and self-consistently**: table numbers increment 0,1,2,...,16;
every table's first pair has `slot=0x00` and its last has `slot=0x3C`; keyframe
counts range 4-31, exactly matching `history_wavetable.md`'s previously-documented
"4-31 per table" range (table 16 has the full 31, at every even slot 0-60). Table 13
decodes as `[(241,0),(242,8),(243,16),(244,24),(245,32),(241,40),(242,48),(243,60)]`
— an independent, byte-for-byte reproduction, recovered here from the raw ROM with no
reference to the existing doc while decoding, of `history_wavetable.md`'s previously
documented "wavetable 13 references waveforms 244 and 245, which don't exist"
anomaly. Waveform 101 (`0x65`) recurs as the slot-0 keyframe of tables 0, 2, 3, 6, 9,
12, and 14 in this parse, independently corroborating the existing doc's "waveform
101 appears in several tables" note. The naive sequential parser desynchronizes after
table 16 (subsequent "table numbers" stop looking like small monotonic values, and
runs of pairs like `(128,128)` — characteristic of genuine 8-bit-PCM digital silence
— start appearing), most likely marking the point where the index-table region ends
and raw waveform samples begin later in the same EPROM; **tables 17-28 were not
recovered in this pass** (see Open Questions). Separately, `w23_66.bin`'s 64-byte
blocks were confirmed by eye to look like genuine single-cycle 8-bit unsigned-PCM
waveforms — smooth sample-to-sample deltas, small wrap-around discontinuity between
each block's first and last byte, asymmetric non-sinusoidal shapes — rather than
metadata or code.

### Universal fixed-tail waveforms (new finding, prompted by a follow-up question)

Prompted by the project owner questioning whether "calculated" waveforms
(`ppg.synth.net`/`PPGWTbl.pdf`, see part (a)'s "Resolving the 'over 2000 waveforms'"
note) could mean primitive geometric waveforms rather than interpolation: they don't
(that gap is fully explained by interpolation alone), but direct visual inspection of
`PPGWTbl.pdf`'s per-wavetable renderings surfaced a separate, genuine fact not stated
in that document's own prose. Every wavetable page checked in this pass — 0, 1, 2, 3,
13 (the buggy one), 19, 20, 21, 22, 23, 24 — shows the **exact same four shapes at
slots 60-63 (0x3C-0x3F)**: a triangle wave, a ~50%-duty pulse/square, a
narrower-duty pulse, and a rising sawtooth ramp — every one marked with a **black**
header, i.e. the document's own convention for "taken directly from the waveform
section," not interpolated. This holds even in wavetable 13, whose slots 0-59 render
as visibly corrupted/noisy waveforms (consistent with `history_wavetable.md`'s
"references waveforms 244/245, which don't exist" anomaly) — the 60-63 tail is
unaffected.

This sits awkwardly next to this project's own ROM-index parse just above, which
found each table's authored-keyframe list terminating with an entry at `slot=0x3C`
(60) and no further entries read past it. Two explanations are both consistent with
what was actually observed, and this pass could not distinguish between them:

1. Real PPG hardware's index format has additional real entries after the `0x3C`
   terminator (e.g. exactly three more, at 0x3D/0x3E/0x3F) referencing a shared set of
   "standard" waveforms, which this session's sequential parser — which stops at
   `slot==0x3C` by design — never read.
2. `PPGWTbl.pdf`'s own rendering tool fills the tail of every table's display with a
   fixed set of reference waveforms for readability, independent of what the real
   synthesizer's index actually contains at those slots.

Not resolved in this pass (see Open Questions) — but worth recording either way: *if*
(1) holds, real PPG wavetables always resolve to a recognizable triangle/pulse/
pulse/saw at the extreme top of their wave-position range, a deliberate "always land
somewhere familiar" design choice worth knowing about when authoring new tables for
this project's own engine (out of scope to act on here, per the task brief).

### Preset/patch record structure — cassette decode (primary new result)

Per explicit direction to run the tool rather than only read it: `ppgwavecass-0.5.4`
was fetched from SourceForge and built unmodified with `gcc` (clean build, no
warnings beyond stock `-Wall -Wextra`). It was then run against all three factory
cassette recordings:

- `wave22_fact.zip → w22_fact.wav` (44.1kHz/8-bit mono): decoded cleanly with
  `ppgwave22cassdecode` — checksum OK, end-byte OK, address range auto-identified as
  `0x0400-0x17FF` ("Programs," matching `ppgwavedefs.h`'s
  `PPGWAVE_ALLPROGS_ADDR_MIN/MAX`).
- `wave20_fact.zip → w20_fact.wav` (44.1kHz/8-bit mono): decoded cleanly with the
  Wave-2-specific `ppgwave2cassdecode` variant (different carrier frequencies), same
  address range.
- `wave23_fact.zip → w23_fact.wav` (22.05kHz/16-bit mono): **failed to decode at
  all** on the first attempt (`ppgwave22cassdecode` found no sync tone anywhere in
  the 99-second file). Diagnosed as an insufficient-sample-rate problem: the
  decoder's FSK/PDM pulse classification counts whole samples per half-cycle, and at
  22050Hz the ~4213Hz "frequency B" tone has only ~2.6 samples per half-cycle — too
  coarse, and the tool was evidently written/tested against 44.1kHz captures (as the
  other two files here are). A throwaway Python script (scratchpad only, not part of
  the decoder) linearly upsampled the file 2× to 44100Hz; the same decoder then
  decoded it cleanly on the very next attempt — checksum OK, end-byte OK, no leftover
  bytes. A 2-second zero-crossing measurement directly on the raw (pre-upsample)
  audio gave ≈4210Hz, matching the decoder's compiled-in Wave 2.2/2.3 "frequency B"
  constant (4213.5Hz) to within measurement error — direct confirmation the audio
  genuinely carries this exact FSK+PDM tone pair (`PPGWAVECASSFREQRATIOMA=712`,
  `PPGWAVECASSFREQRATIOBA=2`, base clock 1.5MHz for 2.2/2.3 vs. 1MHz for Wave 2, per
  `ppgwavedefs.h`), not some other scheme.

All three decodes independently confirm the Programs region is exactly
`0x0400`-`0x17FF` (5120 bytes) for all three PPG generations tested.

**`ppgwavecass`'s source does not parse individual Program fields — confirmed by
reading it, not inferred.** `ppgwavecassdecode.c` identifies a decoded block's type
purely by matching its address-range header against four known ranges (RAM /
Programs / Sequences / Programs+Sequences) and writes the block out verbatim (plus
optional SysEx wrapping); no struct or field table for Program contents exists
anywhere in the source. By contrast, `ppgwave22seqsparse.c` *does* fully parse
Sequence data field-by-field (`Begin`/`Begin2`/`NoteOn`/`NoteOff` event structures
with named fields like `PROG=`, `KEY0=`, `SP=`, per-channel bytes) — the tool's
author reverse-engineered the Sequence format but evidently never did the same for
Programs. This directly confirms the exact gap `module_wavetable.md`'s Future/TODO
already flagged; this primary source doesn't close it either.

Given that, autocorrelation was run directly against the three actually-decoded
5120-byte Program blocks (a real measurement on real decoded data, not a
hypothesis): for both Wave 2.2 and Wave 2.3, byte-match rate peaks sharply at a
**51-byte stride** (with harmonics at 102 and 153 also elevated, as expected for a
true period), and **exactly 100 full 51-byte records fill the first 5100 of the 5120
bytes**, leaving a 20-byte remainder whose structure was not decoded. This
**100-program, 51-byte record** result is independently corroborated by the owner's
manual's cassette chapter, which states plainly: *"all 100 sound programs are dumped
onto cassette"* (p.29) — an exact match between two unrelated methods. Wave 2 instead
peaks at a **50-byte stride**, with **102 full 50-byte records** filling the same
first 5100 bytes (`50×102 = 51×100 = 5100`, landing on an identical total by
coincidence or design) — a one-byte-narrower record and two more slots, plausibly a
smaller Wave-2-era parameter set (e.g. no `UW`/upper-wavetable flag, which is
described in the 2.2 manual as newer), though this specific explanation is
speculation, not confirmed. Wave 2's dump also shows long runs of a single repeated
filler byte (`0xBD`), especially in later records — consistent with several trailing
Program slots on that particular factory cassette being empty/uninitialized rather
than holding distinct patches.

No printable-ASCII name field was found anywhere in any of the three 5120-byte
blocks, and the owner's manual's worked examples always reference programs purely by
number (`PROG:31`, `PROG:99`) with no text-entry mechanism described anywhere in the
manual — both observations are consistent with PPG Wave 2/2.2/2.3 Programs having
**no stored name field**, only a bank-position number.

**The exact byte offsets of individual parameters within the 50/51-byte record could
not be established** from any primary source available in this session — neither the
cassette tool, the owner's manual, nor the service manual documents that level of
detail, and no further differential reverse-engineering was attempted here. What the
record almost certainly *contains*, per the owner's manual's own parameter
enumeration (section a, above), for each of the two Groups: wavetable select
(0-30) and starting wave-position (`WAVES-OSC`/`WAVES-SUB`), the wave-position/
filter/loudness modulation-matrix bits (`KW/MW/TW`, `KF/MF/TF/VF`, `KL/ML/TL/VL`),
filter cutoff/emphasis and Envelope-1 depths (`ENVELOPE-VCF`, `ENVELOPE-WAVES`), the
Envelope-2 ADSR (attack/decay/sustain/release), Envelope-3's attack/decay/amount and
its oscillator-vs-suboscillator pitch-or-wave routing (`EO`/`ES`/`SW=2`), the
suboscillator mode (`SW` 0-3) and detune amount (`DETU` 0-7), 8 individual per-voice
tuning offsets (`SEMIT`), bender destination/interval (`BD`/`BI`), plus
Program-level fields `WAVETABLE`, `KEYB` mode, `KB-SPLIT` point, and the `UW`
(upper-wavetable) flag. This is a parameter *list*, per the task's explicit fallback
instruction, not a byte layout — the layout itself remains unresolved.

### Preset/patch record structure — byte offsets (this session's addition)

Follow-up pass targeting the one item the section above left open: exact byte
offsets within the 50/51-byte Program record. `ppgwavecass-0.5.4` was fetched fresh
from SourceForge again in this session (the "download" link serves an HTML
interstitial to `curl`, not the tarball itself — the actual file is a signed
`downloads.sourceforge.net/project/ppgwavecass/ppgwavecass-0.5.4.tar.gz?ts=...` URL
embedded in that page's own HTML) and rebuilt unmodified with `gcc` (clean, no
warnings). Analysis script kept at `tools/ppg/analyze_program_offsets.py` (script
only — no decoded binaries or extracted records committed, per the task's explicit
constraint and consistent with `tools/ppg/` already being entirely gitignored).

**Re-decoded all three cassettes, including Wave 2 this time.** `wave22_fact.zip`
and the 2×-upsampled `wave23_fact.zip` (same workaround as the earlier pass, same
result: checksum OK, end-byte OK) decoded as expected. `wave20_fact.zip` — lower
priority per the task brief, attempted only after the other two succeeded — in fact
decoded cleanly on the **first** attempt with `ppgwave2cassdecode`, checksum and
end-byte both OK; the earlier pass's caution about it needing extra work turned out
unnecessary. All three again independently reproduce the `0x0400`-`0x17FF` Programs
address range.

**Independently re-derived the record period from scratch** (not re-used from the
earlier pass's numbers): a fresh byte-match-rate autocorrelation over each decoded
5120-byte Programs block, all strides 2-199, confirms stride 51 as the sharp peak for
both Wave 2.2 (match rate 0.378) and Wave 2.3 (0.316), with harmonics at 102 and 153
also elevated, exactly as the earlier pass reported; Wave 2 peaks at stride 50 (0.272,
harmonic at 100). Same conclusion, independently reproduced.

**Statistical fingerprinting of every byte offset (min/max/distinct-count/histogram,
run via `analyze_program_offsets.py fingerprint`) against the known field list is
mostly a negative result, and a real one, not a stalled attempt:**

- No 8-consecutive-byte run stays within *any* tested narrow band (max ≤ 20, ≤ 40, or
  even ≤ 63) anywhere in the 51-byte (Wave 2.2/2.3) or 50-byte (Wave 2) record. This
  was the search built specifically to catch the "8 individual per-voice `SEMIT`
  tuning offsets" anchor described as the strongest expected signature going in — it
  was not found. In the Wave 2.3 record, not even a *single* byte offset stays within
  the manual's own documented 0-63 front-panel display range across all 100 records
  (Wave 2.2 has exactly one, offset 39, and it does not hold up under cross-checking
  below). This directly contradicts the naive "1 byte = 1 raw 0-63/0-30/0-7 dial
  value" hypothesis this pass started with — whatever most of the record's bytes
  hold, it is not that.
- Nibble-level and bit-level (per-bit-column "fraction of records with bit=1")
  re-analysis of the same data found no clean 2-byte / ~12-independent-near-boolean
  signature for the modulation-matrix switches either. The closest thing found was an
  unexplained *structural* regularity, not a field: in the Wave 2.3 record, offsets
  20-24 and again 44-49 each show a repeating ~3-byte value pattern with high but
  non-universal frequency (mode value present in 58-72% of the 100 records at each of
  those offsets, vs. single-digit-percent elsewhere) — the same sub-pattern recurring
  at two different positions in the record. This is flagged as a real, reproducible
  observation (re-run it and it's still there), not explained, and not force-fit to
  any field on the known list.

**One offset resolved with real confidence, cross-checked against a primary source,
not just statistics.** The newly-found preset-table appendix (see Primary Sources
above) gives independently-known `(PROG, KEYB, WAVETABLE)` triples for 83 of the 100
factory programs (17 rows are blank or "reserved for WAVE-TERM demo" in the manual
itself and were excluded). Cross-referencing this against every byte offset of the
decoded Wave 2.2 cassette (`ppgwavecass22` output, i.e. the exact cassette this
manual's own preset table describes) via
`analyze_program_offsets.py presets wave22`:

- **Byte offset 0's low 5 bits equal the manual's printed `WAVETABLE` value in 57 of
  83 known programs (68.7%)** — the next-best offset anywhere in the record manages
  only 11/83 (13.3%), and the chance rate for a 5-bit field matching one of ~24
  distinct observed table values at random is roughly 3%. No other offset comes
  close.
- **The same byte's top 3 bits equal the manual's printed `KEYB` value in 69 of 83
  (83.1%)**, and the full byte matches *both* fields simultaneously (`byte0 =
  wavetable | (keyb << 5)`) in 54 of 83 (65.1%) — again far above the ~0.3-match
  chance expectation for two independent fields matching simultaneously by luck.
  Across the full 100-record set (not just the 83 cross-checkable ones), the masked
  low-5-bit values stay entirely within `0-31` and the top-3-bit values stay within
  `{0,1,3,4}` — a subset of the manual's documented `KEYB` `0-8` range — which is
  itself supporting structural evidence independent of the name-matching above.
- **This is corroborated by the manual's own worked example, not just the appendix
  table**: p.9's walkthrough shows the display `PROG:31 WAVETABLE:24 ... KEYB:1` for
  Program 31, and the decoded Wave 2.2 cassette's record 31, byte 0, is `0x18` = 24 —
  an exact match on `WAVETABLE`. (`KEYB` is the one place the manual's own two
  sources disagree with each other: p.9's display and its following prose both say
  `KEYB:1`/"4-voice polyphonic with two sounds together," but the preset-table
  appendix prints `KEYB: 0` for Program 31, and the decoded byte's top 3 bits are `0`
  — agreeing with the appendix and the cassette, not the p.9 prose. Flagged rather
  than silently resolved either way; not investigated further.)
- **Cross-generation check**: the identical offset/mask test against the *Wave 2.3*
  cassette (a different, later factory-program bank, not the one this Wave 2.2
  manual's table describes) still gets 46/83 (55.4%) on `WAVETABLE` alone and 38/83
  (45.8%) on both fields together — lower than the matching-generation number, as
  expected for a genuinely different preset bank, but nowhere near chance, which is
  read as evidence the `byte0 = wavetable | (keyb << 5)` packing is a real structural
  feature of the Wave 2.2/2.3 51-byte record format itself, not an artifact of one
  specific cassette. The same test against the *Wave 2* cassette (50-byte record,
  known-different, older format) gets 6/83 (7.2%) — indistinguishable from chance, as
  expected, since nothing here claims the Wave 2 record shares this layout.

**Everything else on the known field list remains unresolved** — filter
cutoff/emphasis, both ADSR-style envelopes, Envelope 3's attack/decay/amount and
routing, the 12 modulation-matrix switch bits, suboscillator `SW`/`DETU`, the 8
`SEMIT` tuning bytes, bender `BD`/`BI`, `KB-SPLIT`, and `UW` were all searched for by
the methods above and none produced a signal comparable to byte offset 0's. This is
reported as a genuine negative result, not a stopping point chosen for convenience:
the search was systematic (every offset, every generation, three statistical lenses)
and came up empty past the one field above.

**One more, unexplained, data point**: the 20-byte trailer following the 5100 bytes
of Program records is **byte-for-byte identical** between the Wave 2.2 and Wave 2.3
decodes of this session (`0a3404ec04e384ed84350464846601660266035a` in both,
independently re-decoded from two different physical cassettes) despite those two
cassettes' actual Program contents differing throughout. The Wave 2 decode's trailer
differs (`bdbdbdbdbd0cbdbdbdbdbdd92c35cc35cfbdbd17`) but is dominated by the same
`0xBD` filler byte already noted for that dump's trailing empty-looking Program
slots. A fixed, cassette-content-independent trailer value shared across two
otherwise-different Wave 2.2/2.3 dumps is more consistent with it being a firmware
constant (version stamp, fixed end-of-block marker, or similar) than per-program
data — worth recording even though its exact purpose remains open (see Open
questions).

### Cassette modulation scheme, observed directly

`ppgwavedefs.h` describes the format in a source comment as "a combination of FSK
(frequency shift keying) and PDM (pulse duration modulation)" using two carrier
tones, frequency A and frequency B = 2×A, derived from a base clock divided by 712
(1.5MHz for Wave 2.2/2.3 → ≈2107Hz/4213Hz; 1MHz for Wave 2 → ≈1404Hz/2809Hz). This
was independently confirmed by direct measurement, not just read from source: a
2-second zero-crossing count on the raw `w23_fact.wav` audio gave ≈4210Hz, matching
the compiled-in Wave 2.2/2.3 constant. Bit values are carried as pulse-duration
counts within runs of a given tone (`PPGWAVECASSPER0A/1A/0B/1B` = 2/8/8/20
tone-periods for a 0/1 bit at each frequency), preceded by a long synchronization
tone burst at frequency B. This session did not characterize the sync-tone/byte
-framing details beyond what was needed to successfully decode all three files;
`ppgwavecassencode.c` (the encoder half) was fetched but not read line-by-line.

## Open questions / not resolved in this pass

- **Partially resolved** (see "Preset/patch record structure — byte offsets" above):
  byte offset 0 of the Wave 2.2/2.3 51-byte Program record is confirmed, with real
  cross-checked confidence (68.7%/83.1%/65.1% match rates against a newly-found
  manual preset-table appendix, far above chance, plus an exact match on the
  manual's own p.9 worked example), to pack `WAVETABLE` (low 5 bits, 0-30) and
  `KEYB` keyboard-mode (top 3 bits, 0-8) together as `wavetable | (keyb << 5)`. The
  other ~30 known fields on the list — filter cutoff/emphasis, both ADSR envelopes,
  Envelope 3, the 12 modulation-matrix bits, suboscillator `SW`/`DETU`, the 8
  `SEMIT` bytes, bender `BD`/`BI`, `KB-SPLIT`, `UW` — were searched for
  systematically (every remaining offset, all three cassette generations, byte/
  nibble/bit-level statistics) and **none produced a usable signal**; this is a
  genuine negative result, not an abandoned search. The Wave 2 50-byte record's
  layout was not investigated at all beyond confirming its record length and that
  offset 0 does *not* carry the same packing (6-7% match, chance-level). Firmware
  disassembly of the `8,A,C,E` EPROMs (`tools/ppg/PPG Wave 2.3 version 6.zip`,
  present locally, not yet attempted by any session) remains the clear next step for
  the fields this pass couldn't pin down — the statistical approach has likely
  reached its limit for anything past the one field resolved above.
- The purpose of the fixed 20-byte trailer following the 100×51 (Wave 2.2/2.3) or
  102×50 (Wave 2) program records inside the decoded 5120-byte Programs block —
  narrowed slightly this session: the trailer is byte-for-byte identical between two
  independently-decoded Wave 2.2 and Wave 2.3 cassettes despite their differing
  Program contents, suggesting a fixed firmware constant rather than per-cassette
  data, but its actual meaning is still unknown.
- An unexplained repeating ~3-byte value pattern recurring at two different offset
  ranges (20-24 and 44-49) within the Wave 2.3 51-byte record, found during this
  session's byte-offset fingerprinting, present in 58-72% of the 100 records at each
  position but not in all of them — not resolved, not matched to any known field,
  flagged rather than forced into the byte-offset map above.
- A one-line discrepancy inside the owner's manual itself, noticed while
  cross-checking Program 31 against the record: the manual's own p.9 worked example
  (display readout and following prose) states `KEYB:1`, while the manual's own
  preset-table appendix prints `KEYB: 0` for the same program — and the decoded
  cassette byte agrees with the appendix, not the p.9 prose. Not investigated
  further; noted in case it matters to a future pass.
- Whether hard sync exists between a voice's two oscillators (Group A/B) — not
  described anywhere in the owner's manual pages read in this pass; only
  oscillator-to-suboscillator detune (within one Group) is documented.
- Tables 17-28 of the wavetable index in `w23_64.bin` were not successfully parsed
  past table 16 by this session's naive sequential decoder; unclear whether that's a
  parser limitation (e.g. a table using a different structure or terminator) or the
  genuine end of the index-table region within that EPROM.
- The "Combi-Program" multi-timbral-snapshot feature mentioned by the Sound on Sound
  review was not found or corroborated in the owner's manual pages read here.
- Precisely where in `w23_66.bin` each of the 244 individual waveforms starts/ends,
  and whether `w23_64.bin` also holds raw waveform samples after its index-table
  region, was not mapped byte-for-byte.
- Cassette sync-tone/byte-framing details beyond what was needed for a successful
  decode were not investigated further; the encoder source
  (`ppgwavecassencode.c`) was fetched but not read.
- The service manual's block diagram shows a second CPU/board (the "PROZ" board,
  labeled the "Sound computer," talking to a "PPG COM.BUS") alongside the I/O
  board's 6809 — its exact role relative to the ROM/waveform/patch data mapped here
  was not investigated, being outside this task's scope.
- The universal triangle/pulse/pulse/sawtooth tail at wavetable slots 60-63 (see
  Universal fixed-tail waveforms, part b) — whether it's a genuine real-hardware
  index-table feature this session's parser simply didn't read that far into, or an
  artifact of `PPGWTbl.pdf`'s own rendering tool, was not resolved.
- `PPGWTbl.pdf` itself only contains rendered image pages for wavetables 0-24 (V4);
  it never reaches wavetables 25 onward, despite its own introduction explicitly
  describing "Wavetable 28/29" as entirely-calculated special cases and wavetable 30
  as the Upper Wavetable — consistent with those three not fitting the document's
  two-keyframe-interpolation rendering format, but not directly confirmed, since no
  page or text for tables 25-30 was found in the fetched copy of this document.
