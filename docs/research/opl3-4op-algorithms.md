# Research: Real OPL3 four-operator connection topologies

Resolves [#138](https://github.com/carllom/t00t/issues/138) (child of wayfinder
map [#136](https://github.com/carllom/t00t/issues/136), "4-operator OPL4-class
voices for the opl engine").

## Question

What are real OPL3's 4 four-operator Algorithms (register-level "connection"
topologies), expressed in this codebase's `FmRouting` shape (`order`/
`kernel`/`in_bus`/`out_bus`/`num_ops`/`clear_bus_mask`/`fb_shift`, see
`src/engines/fm/patch.h`)? For each, which operators are carriers vs.
modulators, how do they chain/sum, and which operator(s) receive feedback?

## Primary source

`tools/opl_ref/nuked/opl3.c` — Nuked-OPL3, fetched at the pinned SHA in
`tools/opl_ref/fetch_nuked_opl3.sh` (`cfedb09efc03f1d7b5fc1f04dd449d77d8c49d50`).
This is this module's own designated ground-truth reference (`module_opl.md`),
so its C source is read directly below rather than a secondary description.

## Chip-level structure (source-verified)

- A 4-op voice is built from two real OPL3 channels a fixed distance apart:
  channel `N` (0/1/2) is `ch_4op`, paired with channel `N+3` (3/4/5) as
  `ch_4op2` (`OPL3_ChannelSet4Op`, opl3.c:1063-1088). Each real channel has
  two operator slots (`slotz[0]`, `slotz[1]`).
- Mapped onto this codebase's op0-op3 numbering (`docs/module_opl.md`
  Glossary: "op0+op1 is the first Operator pair, op2+op3 the second"):
  - Operator pair 1 (`ch_4op`, the lower channel): `slotz[0]` = op0, `slotz[1]` = op1
  - Operator pair 2 (`ch_4op2`, the higher channel): `slotz[0]` = op2, `slotz[1]` = op3
- Each real channel has its own C0 register, i.e. its own connection bit
  (`con`) and feedback field (`fb`) (`OPL3_ChannelWriteC0`, opl3.c:977-980).
  In 4-op mode the two channels' `con` bits combine into one 2-bit selector:
  `alg = 0x04 | (pair1.con << 1) | (pair2.con)` (`OPL3_ChannelUpdateAlg`,
  opl3.c:949-975, specifically line 956). The actual wiring is set up once,
  in `OPL3_ChannelSetupAlg` (opl3.c:848-947), by a `switch (channel->alg & 0x03)`
  over exactly those 4 combinations (opl3.c:881-923) — confirming there are
  exactly 4 four-operator connections, selected by (pair1.con, pair2.con).

## Feedback wiring (source-verified)

`OPL3_SlotCalcFB` (opl3.c:695-706) computes `slot->fbmod` from
`slot->channel->fb` for **every** slot unconditionally, every sample — but
that value is only audible if some operator's `->mod` pointer is wired to
read it. Searching all 4 branches of `OPL3_ChannelSetupAlg`'s 4-op switch
(opl3.c:881-923), the assignment

```c
channel->pair->slotz[0]->mod = &channel->pair->slotz[0]->fbmod;
```

(op0 reading its own feedback history) appears **identically in all 4
cases**, and is the **only** self-feedback assignment anywhere in the
4-op branch. There is no case where `channel->slotz[0]->mod` (op2) is ever
wired to `&channel->slotz[0]->fbmod`; op2's `->mod` is always either
`zeromod` or another operator's `->out`.

**Finding / flag for the map's decision:** real OPL3 hardware does give the
second Operator pair (op2/op3) its own C0 feedback field (`pair2.fb`,
op2's per-pair register), and that register is still latched into
`fbmod` every sample by `OPL3_SlotCalcFB` — but **in all 4 real 4-operator
connections, nothing ever reads op2's `fbmod`**, so it is architecturally
dead: programmable, silently ignored, and audibly a no-op. Only op0 ever
receives real feedback in 4-op mode. This matches independently-documented
OPL3 hardware behavior (the second channel's feedback bits are commonly
noted as "not used" in 4-op mode in AdLib/OPL3 programming references,
and MAME's `ymf262.cpp` wires feedback the same op0-only way for 4-op
channels). The map's destination phrasing ("feedback lands only on the
first operator of each Operator pair (op0 and/or op2)") should be read as
"op0 always; op2 is structurally never a live feedback point across the 4
real connections" — not "op0 or op2, patch's choice." A future 4-op patch
struct should not offer a functioning feedback field on op2 if it wants to
match real hardware exactly (op0-only feedback is real-hardware-accurate;
exposing an op2 feedback field would silently do nothing on real chips).

## The 4 connections

Selector bits below are `(pair1.con, pair2.con)`, matching
`OPL3_ChannelSetupAlg`'s `switch (channel->alg & 0x03)` case values
0x00-0x03 (opl3.c:881-923) — "pair1" = op0/op1's own channel C0 bit,
"pair2" = op2/op3's own channel C0 bit.

### Connection 0 — (con0=0, con1=0): full serial chain

`op0(fb) -> op1 -> op2 -> op3 -> OUT`. One carrier (op3); op0/op1/op2 all
pure modulators. (opl3.c:883-892)

```
order           = {0, 1, 2, 3}
kernel          = {FIRST, FIRST, FIRST, FIRST}   // FEEDBACK on op0 when fb>0, patched at note-on
in_bus          = {FM_BUS_ZERO, 1, 2, 3}
out_bus         = {1, 2, 3, FM_TARGET_OUT}
num_ops         = 4
clear_bus_mask  = 0   // set for bus 1 at note-on only if op0's feedback is patched in
fb_shift        = {<patched>, 0, 0, 0}
```

### Connection 1 — (con0=0, con1=1): two independent 2-op FM pairs, summed

`(op0(fb) -> op1) + (op2 -> op3) -> OUT`. Two carriers (op1, op3); op0/op2
are modulators (op2 unmodulated). Structurally this is `OPL_ROUTING_FM`
run twice into a shared bus. (opl3.c:893-902)

```
order           = {0, 1, 2, 3}
kernel          = {FIRST, FIRST, FIRST, PLAIN}
in_bus          = {FM_BUS_ZERO, 1, FM_BUS_ZERO, 3}
out_bus         = {1, FM_TARGET_OUT, 3, FM_TARGET_OUT}
num_ops         = 4
clear_bus_mask  = 0   // bus 1 only, if op0 feedback patched
fb_shift        = {<patched>, 0, 0, 0}
```

### Connection 2 — (con0=1, con1=0): op0 additive+feedback, plus a 3-op chain

`op0(fb) -> OUT` (directly, additive) **and** `op1 -> op2 -> op3 -> OUT`.
Two carriers (op0, op3); op1/op2 are modulators (op1 unmodulated).
(opl3.c:903-912)

```
order           = {0, 1, 2, 3}
kernel          = {FIRST, FIRST, FIRST, PLAIN}
in_bus          = {FM_BUS_ZERO, FM_BUS_ZERO, 2, 3}
out_bus         = {FM_TARGET_OUT, 2, 3, FM_TARGET_OUT}
num_ops         = 4
clear_bus_mask  = 0   // FM_TARGET_OUT bit only, if op0 feedback patched (op0 is FIRST writer of OUT)
fb_shift        = {<patched>, 0, 0, 0}
```

### Connection 3 — (con0=1, con1=1): op0 additive+feedback, a 2-op FM pair, and op3 standalone

`op0(fb) -> OUT` (additive) **and** `op1 -> op2 -> OUT` **and**
`op3 -> OUT` (standalone, unmodulated). Three carriers (op0, op2, op3);
op1 is the only pure modulator. (opl3.c:913-922)

```
order           = {0, 1, 2, 3}
kernel          = {FIRST, FIRST, PLAIN, PLAIN}
in_bus          = {FM_BUS_ZERO, FM_BUS_ZERO, 2, FM_BUS_ZERO}
out_bus         = {FM_TARGET_OUT, 2, FM_TARGET_OUT, FM_TARGET_OUT}
num_ops         = 4
clear_bus_mask  = 0   // FM_TARGET_OUT bit only, if op0 feedback patched
fb_shift        = {<patched>, 0, 0, 0}
```

## Summary table

| Connection | con0,con1 | Carriers | Modulators | Chains | Feedback op |
|---|---|---|---|---|---|
| 0 | 0,0 | op3 | op0,op1,op2 | op0→op1→op2→op3 | op0 |
| 1 | 0,1 | op1, op3 | op0, op2 | op0→op1 \| op2→op3 | op0 |
| 2 | 1,0 | op0, op3 | op1, op2 | op0(add) \| op1→op2→op3 | op0 |
| 3 | 1,1 | op0, op2, op3 | op1 | op0(add) \| op1→op2 \| op3(add) | op0 |

Feedback is on **op0 in every one of the 4 real connections** — never op2.
This confirms half of the map's destination decision ("feedback only on
the first operator of an Operator pair") and narrows it: for the specific
case of 4-op voices, only the *first* Operator pair's first operator (op0)
is ever a real feedback point on actual OPL3 hardware; op2's feedback
register is present in the register map but provably unwired in all 4
connections, per the source above.

## Idiom match against existing `OPL_ROUTING_FM`/`OPL_ROUTING_ADD`

The literals above follow `src/engines/opl/patch.h`'s existing convention
exactly: `in_bus[i] = i` when something writes into operator i's own bus,
else `FM_BUS_ZERO`; `out_bus[i]` = the receiving operator's index, or
`FM_TARGET_OUT` for a carrier; `kernel[i]` = `FM_KERNEL_FIRST` for a bus's
first writer, `FM_KERNEL_PLAIN` for a subsequent (summing) writer,
`FM_KERNEL_FEEDBACK` only ever on op0's slot, patched in at note-on from
the patch's feedback level exactly as `opl_voice.h` already does for the
2-op case (`OPL_ROUTING_FM`/`OPL_ROUTING_ADD`'s own header comment:
"kernel[0]/fb_shift[0]/clear_bus_mask on op0 are patched into a per-voice
copy... at note-on"). `clear_bus_mask` in every literal below is 0 at rest,
becoming non-zero (the bit for whichever bus op0's `FM_KERNEL_FEEDBACK`
first-writes) only for the specific voice when feedback is patched in,
mirroring the 2-op literals precisely — no new mechanism needed.
