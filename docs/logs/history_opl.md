# T00T — OPL Module Development Log

### OPL2 Engine Skeleton (#78)

The payoff of #77's `op.h` prefactor (which made the DX7 module's per-sample
kernel reusable by a non-DX7 engine without forking it): a new
`src/engines/opl/`, buildable via `make ENGINE=opl`, playing hand-authored
2-operator patches over MIDI with 9-voice polyphony, self-feedback, and the
existing delay/reverb/pan FX chain.

`src/engines/opl/` includes `src/engines/fm/op.h` directly and reuses,
unchanged: `FmOp`, `op_render`/`op_render_first`/`op_render_fb`,
`fm_voice_render_block()`, `FmVoiceBuses`, the `FmRouting` type, and
`eg_to_gain()`/`eg_exp2_table` (from the DX7 module's `env_dx.h`). Everything
above that layer — `EnvOpl`, `OplPatch`, and the note-on/envelope-step/
note-off/render-voice glue (`opl_voice.h`) — is OPL's own, following #76's
explicit scope: those DX7 functions are hardcoded to a `const FmPatch&` and
to `FmOp::eg` as a concrete `EnvDX`, so they aren't a fit for OPL's own patch
or envelope shape regardless of #77's own template work.

Two real correctness traps the reuse required getting right, neither of
which surfaced as a test failure — both were reasoned through before writing
any render code, from reading `op.h`/`patch.h`/`env_dx.h` closely rather than
by trial and error:

- **`FmOp::eg` is dead weight for this engine, and `fm_voice_active()` can't
  be reused because of it.** `FmOp`'s `eg` field is a concrete `EnvDX`, not a
  template parameter — OPL's per-voice array is still `FmOp[FM_NUM_OPS]` (see
  below), but that field is simply never touched. Calling the DX7 module's
  own `fm_voice_active()` on it would read a never-triggered `EnvDX` whose
  `ix` defaults to 0 (a real, "active" stage) rather than the idle sentinel —
  every voice would report itself permanently active. OPL keeps its own
  `EnvOpl[2]` array in parallel and its own `opl_voice_active()` reads that
  instead.
- **A patch's feedback amount isn't compile-time, so it can't live in the
  `constexpr FmRouting` literal itself.** `patch.h`'s two fixed algorithm
  routings are written with `kernel[0] = FM_KERNEL_FIRST`/`fb_shift[0] = 0`
  as a feedback-off baseline; `opl_voice_note_on()` copies the chosen literal
  into a per-voice `FmRouting` and only then patches `kernel[0]`/
  `fb_shift[0]`/`clear_bus_mask` from the patch's own `feedback` field —
  `fb_shift = 8 - feedback`, the same total-shift convention the DX7 module
  already uses in `op_render_fb`, which turns out to apply unchanged here
  too.

Every voice's `FmOp` array is the full six-wide `FM_NUM_OPS` array
`fm_voice_render_block()` expects, even though only two operator slots ever
carry real signal — the remaining four are zeroed once at boot
(`opl_voice_init_inert()`) and never touched again, each pointed at its own
never-read scratch bus. This was a deliberate tradeoff (see `module_opl.md`'s
Decision Record #2) to reuse `fm_voice_render_block()` completely unchanged
rather than write and maintain a second, hand-rolled two-operator interpreter
loop; its real per-voice cost is unmeasured (see below).

`EnvOpl` shares `EnvDX`'s own level domain (Q24 octaves) so `eg_to_gain()`
needed no changes at all — TL/KSL/velocity compose into an attack ceiling in
that domain the same way the DX7 module's output level/key scaling/velocity
compose into `outlevel`, just in OPL-native dB units. Every stage is
currently a linear ramp in that log domain; real OPL2 attack is a curved
shape, a known, explicitly tracked gap (`module_opl.md`'s Future/TODO), not
a claim of chip accuracy.

Verification: `tools/host_render/render_opl.cpp` (new) renders all five
`patches.h` test patches — one FM-chain patch with feedback, one percussive
FM-chain patch with heavier feedback, an FM-chain bass patch, an additive
organ-ish patch, and an additive percussive patch — through the exact device
code path, confirming bounded, non-silent audio and that every one reaches
its idle state within a 3-second release tail after note-off. All five pass.
`make ENGINE=opl` links cleanly; `make ENGINE=fm` afterward confirms the
shared `fm/` tree is unaffected, and the DX7 module's own full self-check
suite (`render_fm`: routing, EG shape, release, pitch EG, LFO, 32-patch bank
render) still passes unchanged. A binary symbol check confirmed the DX7
module's own dead weight this build never references — `fm_sine_table`, the
DX7 rate/level tables — is absent from the linked `t00t.elf`, i.e. the
cross-engine `#include` genuinely costs nothing unused, not just in theory.

Not done in this pass, both explicitly deferred: **hardware confirmation**
("audible on real breadboard_rp2350 hardware") needs Carl at the bench, the
same as every other hardware-gated ticket in this project's history — a host
WAV render is not a substitute. **Nuked-OPL3 conformance** (`tools/opl_ref/`,
`opl_ctl_diff.py`, `opl_regress.py`, all described in #76's own scope) was
not built — `EnvOpl`'s curves are a plausible best-effort shape, not a
verified match to real hardware, mirroring how the DX7 module's own envelope
curves shipped a working patch well before Dexed conformance testing existed
for them.

### Sustain-Level By-Ear Pass

Carl's first flash of this skeleton produced real audio (two distinct
patches confirmed by ear), but reported low output volume. `render_opl`'s
own bounded/non-silent check hadn't caught this, because a peak-of-the-whole-
render check is dominated by the brief attack transient regardless of where
a patch actually settles.

Root cause, traced with a scratch block-by-block instrumentation of
`OPL_PATCH_LEAD`'s carrier: attack correctly reaches its full ceiling within
the first control block, exactly as designed, but decay then ramped the
level down by roughly 27 dB (`sl=9`, 3 dB/register-step) to reach its
sustain target within about 2 ms. Since a held note spends nearly all of its
audible duration at the sustain level, not the attack peak, that 27 dB drop
is what a player actually hears as "quiet" -- the kernel and gain-domain
math (all reused unchanged from the DX7 module) were never the problem; the
hand-authored `sl` register values in `patches.h` were simply too
attenuated for a patch meant to be heard as sustained, not plucked.

Fixed by lowering `sl` on `OPL_PATCH_LEAD`'s and `OPL_PATCH_BASS`'s
operators (roughly -27 dB/-18 dB sustain attenuation down to -6 dB/-3 dB)
and `OPL_PATCH_ORGAN`'s slightly further for margin -- `OPL_PATCH_BELL`/
`OPL_PATCH_PERC` keep `sl=15` unchanged, since their whole design point is
decaying to true silence on their own (egt=false), not holding at a
sustained level. A scratch instrumentation comparing overall peak against
the settled level 500 ms into a held note confirms the sustained portion
now sits within a few dB of the attack peak instead of tens of dB below it.
Re-verified: `render_opl`'s existing bounded/non-silent/idle-after-release
checks still pass unchanged (peak-of-whole-render is, expectedly, the same
number as before -- it was never measuring the thing that was actually
wrong). Still needs Carl to reflash and confirm the sustained level now reads as
loud enough by ear; the `render_opl` harness itself doesn't check
sustained-vs-peak loudness, so a future fidelity pass might want to add
that check specifically, since a whole-render peak measurement alone
already missed this once.

### Percussive Decay Rate By-Ear Pass

Same reflash, next finding: `OPL_PATCH_BELL` and `OPL_PATCH_PERC` (both
`egt=false`) played as a bare click with no audible body at all -- not a
short-but-real percussive hit, just a transient. A scratch check of
`opl_effective_rate()`/`OPL_RATE_TIME_S` at the two patches' actual decay
register values (`dr=10` for `OPL_PATCH_BELL`'s carrier, `dr=14` for
`OPL_PATCH_PERC`'s) confirmed why: at the played test note, those decay
rates cross the *entire* 15-octave range in under a millisecond (0.00098s
and 0.00024s respectively). Percussive mode's decay target is silence
itself (not a sustain level, see the Sustain-Level entry above), so a
near-instant decay rate meant these patches were, correctly per their own
data, decaying to nothing before a listener's ear could register anything
but a click -- again not a kernel or gain-domain bug, a rate register choice
that mapped to far too short a real-world time.

Fixed by lowering both patches' decay rates by roughly an order of
magnitude (`OPL_PATCH_BELL`: op0 12->3, op1 10->1; `OPL_PATCH_PERC`: op0
15->5, op1 14->2), re-checked against the same rate-to-time helper: BELL's
carrier now rings for about 500 ms with its modulator's brighter edge
fading out first (~125 ms), and PERC's carrier now has a ~250 ms body with
a ~31 ms modulator transient on top -- both real, audible decays rather
than sub-millisecond clicks. `render_opl`'s bounded/non-silent/idle checks
still pass.

Confirmed on real breadboard_rp2350 hardware: all five patches audible with
their own distinct sonic character, `OPL LEAD`/`OPL ORGAN` at a real
sustained volume, `OPL BELL`/`OPL PERC` each with an audible decay/release
tail rather than a click.

### Nuked-OPL3 Reference Build + Dump Tooling (#79)

Infrastructure for the fidelity work #78 explicitly deferred: an independent
reference emulator plus dump tooling, not the actual curve correction itself
(split out as separate, later tickets once this landed).

`tools/opl_ref/` fetches [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3)
(`nukeykt/Nuked-OPL3`, pinned at `cfedb09efc03f1d7b5fc1f04dd449d77d8c49d50`)
at build time rather than vendoring it — it's LGPL-2.1, not the permissive
MIT license the chip module's vendored `tools/ay_ref/ayumi` carries, so it
follows the same fetch-not-vendor pattern the DX7 module's `tools/fm_ref/`
(Dexed) already established. Confirmed directly against the fetched source
(`opl3.c`'s `OPL3_SlotWrite*`/`ad_slot[]` functions) that Nuked-OPL3's
register bit-layout matches `src/engines/opl/patch.h`'s `OplOpParams` field
for field, and that its own frequency-multiplier table (`mt[16]`, doubled
integers) is byte-identical to `opl_mult_table[16]` once doubled — a
confirmation that #78's own patch design, chosen for register-shaped
clarity rather than DX7-style abstraction, was already correct against real
hardware on at least that one table, entirely independent of this ticket's
own work.

`nuked_render.cpp`/`nuked_dump.cpp` drive Nuked-OPL3 through its real
`OPL3_WriteReg()` interface — genuinely programming a (emulated) chip, not
calling into an abstraction layer. Velocity has no real OPL2 register at
all, so it's folded onto the TL register using the exact formula
`env_opl.h`'s own `env_opl_init()` uses, documented inline as a t00t-side
convention rather than something the reference chip does natively — this is
what makes "same patch, note, velocity" comparable between the two sides at
all.

`tools/host_render/t00t_opl_ctl_dump.cpp` is the OPL module's first
addition to the shared `tools/host_render/CMakeLists.txt` build (`make
host`) rather than a separate hand-written Makefile like the DX7 module's
`t00t_ctl_dump` needs — that separation exists there specifically because
`patches.h` is gitignored/generated and needs a presence gate; OPL's
`patches.h` is checked in, so there's no reason to keep it out of the
shared build, matching how the chip module's `t00t_ay_dump`/`t00t_chip_dump`
are already wired in.

Both dump tools share four domains (`mult`/`ksl`/`tl`/`eg`) and the same
`# domain=X cols=...` self-describing CSV header the chip module's dump
tools already use (judged a better fit than the DX7 module's older
`--what`-flag convention, since OPL's complexity sits between the chip
module's plain counters and the DX7 module's piecewise curves). `mult` and
`tl` matched exactly between the two sides on the first real run, as
expected (`mult` is the table-identity check above; `tl` is linear by
hardware definition on both sides). `ksl` and `eg` are only shape-comparable
right now, not numerically reconciled -- expected, since correcting
`env_opl.h`'s curves against these numbers is explicitly separate, later
work.

Verified: `tools/opl_ref/fetch_nuked_opl3.sh && make` builds `nuked_render`/
`nuked_dump` cleanly; both produce real, bounded, non-silent/non-degenerate
output (`nuked_render` a real WAV, `nuked_dump --domain eg` a real
attack/decay trajectory). `make host` (top-level) builds
`t00t_opl_ctl_dump` as part of the existing shared host build; its four
domains' output is shape-comparable to `nuked_dump`'s for the same domains.
`make ENGINE=opl` and `make ENGINE=fm` both still build clean -- this was
host-tooling-only work, nothing device-side changed.
