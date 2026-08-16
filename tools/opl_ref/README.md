# OPL reference rig

Ground-truth comparison infrastructure for the OPL module: [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3),
a register-level OPL2/OPL3 emulator, driven the same way a real chip is
programmed. Nothing here is ever linked into device firmware — host-only
tooling.

## Setup

```bash
cd tools/opl_ref
./fetch_nuked_opl3.sh       # Nuked-OPL3, pinned commit -> nuked/
make                        # -> ./nuked_render ./nuked_dump

# t00t side, built as part of the shared host build
cd ../..
make host                   # -> tools/host_render/build/t00t_opl_ctl_dump
```

`nuked/` and the build products are gitignored — Nuked-OPL3 is LGPL-2.1, a
copyleft license, so it's fetched at a pinned SHA rather than committed into
this repo. `fetch_nuked_opl3.sh` pins what it downloads.

## Use

Render one patch through each side:

```bash
./nuked_render --op0 1,0,14,15,10,3,8,1,0,0 --op1 1,0,0,15,9,2,8,1,0,0 \
    --feedback 3 --algo fm --note 57 --vel 127 --out out/ref.wav
```

`--op0`/`--op1` are `mult,ksl,tl,ar,dr,sl,rr,egt,ksr,ws` — the same field
order as `src/engines/opl/patch.h`'s `OplOpParams`, since OPL's own patch
struct is already register-shaped (unlike the DX7 module's ratio/detune
scheme, this is close to a direct field-to-register mapping).

### Control-plane state

No audio, no spectra — just numbers, dumped as CSV on both sides:

```bash
./nuked_dump --domain mult
./nuked_dump --domain ksl
./nuked_dump --domain tl
./nuked_dump --domain eg --mult 1 --tl 0 --ar 15 --dr 9 --sl 2 --rr 8 --note 57 --vel 127

../host_render/build/t00t_opl_ctl_dump --domain mult
# ...same --domain set, same flags for eg
```

Both dumpers share the same domain names and `# domain=X cols=...`
self-describing CSV header. There is no diff script yet — comparing these
two dumps and correcting `src/engines/opl/env_opl.h`'s curves against
Nuked-OPL3 is separate, later work; this rig is the infrastructure that
makes that comparison possible, not the comparison itself.

## Files

| | |
|---|---|
| `fetch_nuked_opl3.sh` | Nuked-OPL3 at a pinned SHA. Bump deliberately: it moves every baseline number a future diff would score against. |
| `nuked_render.cpp` | the reference renderer, driving Nuked-OPL3 through its real register interface (`OPL3_WriteReg`). |
| `nuked_dump.cpp` | control-plane state as CSV: the frequency-multiplier and KSL tables (exact, read from Nuked-OPL3's own file-local tables), the TL register's linear scale, and one operator's live envelope trajectory. |
| `../host_render/render_opl.cpp` | the t00t side, rendering hand-authored patches through the real engine. |
| `../host_render/t00t_opl_ctl_dump.cpp` | the t00t side of the control-plane dump, reading `src/engines/opl/`'s real headers directly. |
