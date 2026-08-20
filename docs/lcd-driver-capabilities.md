# ST7789 capabilities beyond "overwrite a rectangle"

Research note. Answers: what does the ST7789 controller (and this driver)
actually expose that could optimize display updates, beyond the
CASET/RASET/RAMWR "blit a rectangle of new pixels" pattern this driver
already uses everywhere?

**Primary source**: Sitronix Technology Corp., *ST7789V — 240RGB x 320 dot
262K Color with Frame Memory Single-Chip TFT Controller/Driver*, Datasheet
**Version 1.6, 2017/09**.
<https://wiki.pine64.org/images/5/54/ST7789V_v1.6.pdf>

This driver's board comment calls the panel "ST7789P" (Waveshare's label for
their Rev2 1.83" module); Sitronix does not publish a separate "ST7789P"
datasheet, and the ST7789V document is the canonical full command reference
for the family. It was verified applicable here directly: every extended
("Command Set 2") register this driver's init table sends —
`PORCTRL`(B2h), `GCTRL`(B7h), `VCOMS`(BBh), `LCMCTRL`(C0h), `VDVVRHEN`(C2h),
`VRHS`(C3h), `VDVS`(C4h), `FRCTRL2`(C6h), `PWCTRL1`(D0h), `PVGAMCTRL`(E0h),
`NVGAMCTRL`(E1h) — matches this datasheet's command names and opcodes
exactly (§9.2, pp. 260–316). Page numbers below cite this document.

## What the driver currently does

Source: `src/wslcd/lcd_st7789.cpp`/`.h`, `src/wslcd/gfx.cpp`/`.h`,
`src/wslcd/display.h`.

- **Init** (`lcd_init`, once at boot): `MADCTL`(36h) orientation, `COLMOD`(3Ah)
  set to `0x05` = 16bpp RGB565, the panel-tuning register block above, then
  `INVON`(21h) — issued once, permanently, because this is an IPS panel that
  needs display inversion on for correct colour (see code comment) — then
  `SLPOUT`(11h) and `DISPON`(29h).
- **Every draw operation**, without exception, is: `lcd_set_window()` sends
  `CASET`(2Ah) + `RASET`(2Bh) + `RAMWR`(2Ch), then a pixel stream is DMA'd in
  over SPI (`lcd_blit`/`lcd_blit_start`/`lcd_blit_wait`, or `lcd_fill_window`
  for solid fills via a 2-byte ring-wrapped DMA source). `gfx_fill_rect`,
  `gfx_text` (per-glyph), and `gfx_gradient` (per-band) all follow this same
  set-window-then-stream-pixels pattern. There is no code path that reuses an
  already-open write window or continues a previous `RAMWR` — every call
  re-issues `CASET`/`RASET`/`RAMWR` from scratch, even for adjacent regions.
- **No shadow/local framebuffer exists anywhere in this driver or its
  callers.** `gfx.cpp` keeps only a small scratch tile, `s_glyph`
  (24×24 px @ 16bpp = 1,152 B, sized for one glyph at max scale and reused
  for `gfx_gradient`'s bring-up bands) — never a full-panel buffer. The six
  engine `display.cpp` callers (chip/fm/groovebox/opl/speech/subtractive/
  tracker) drive `gfx_fill_rect`/`gfx_text` directly against the controller;
  "redraw only what changed" (per `display.h`'s doc comment) is done by
  comparing *application-level values* frame to frame, not by diffing pixel
  data. There is nothing in MCU RAM to run a boolean (AND/OR/XOR) operation
  against even if the controller could execute one.
- **Pins wired** (`lcd_st7789.h` header comment, `breadboard_rp2350` board):
  `DIN`(SPI1 TX)/`CLK`/`CS`/`DC`/`RST`/`BL` only. No MISO/SDO line, no TE
  line. This matters for two capabilities below (`RAMRD`, `TEON`).

## Available but unused capabilities

The controller is a dumb frame-buffer chip: it has an 18-bit-per-pixel GRAM
and a scan-out engine, and nothing else. **There is no hardware
compositing/blit ALU** — the full 316-page datasheet has zero hits for
"blend", "XOR", "boolean", "logical operation", or "ALU" of any kind. Every
capability below either changes what the panel *scans out* of GRAM, or
changes global panel state; none of them combine old and new pixel data in
hardware.

| Capability | Confirmed? | What it actually does | Verdict for this driver |
|---|---|---|---|
| **Partial area** — `PTLAR`(30h) + `PTLON`(12h)/`NORON`(13h) | Yes, §9.1.24, p.206; §9.1.13, p.186 | `PTLAR` sets a start/end **row** range (`PSL`/`PEL`, 16-bit, frame-memory row addresses); `PTLON` restricts the panel's *scan-out* to that row band, blanking the rest. It does not shrink or replace the CASET/RASET write window — writes to GRAM outside the partial band still happen exactly as before. It's a display-blanking / power feature, not a write-path shortcut. | **Not useful.** No write-side savings, and this driver already writes only the changed rect (via CASET/RASET) rather than the whole frame — there's nothing left for scan-out blanking to save here. |
| **Vertical scroll** — `VSCRDEF`(33h) + `VSCSAD`(37h) | Yes, §9.1.25, p.208; §9.1.29, p.218 | Defines Top-Fixed/Scroll/Bottom-Fixed line counts in GRAM (`TFA+VSA+BFA` must equal 320) and a scroll-start pointer; the panel scan-out then reads GRAM starting from that offset, producing hardware wraparound scroll with **no pixel movement**. Datasheet is explicit: *"This command just defines the Vertical Scrolling Area... and [does] not perform vertical scroll"* — new content for the row scrolled into view must still be written via ordinary `CASET`/`RASET`/`RAMWR` (flow chart, p.210). | **Actually useful, conditionally.** For a scrolling widget (waveform strip, tracker line log, VU history) this turns an O(rows) full-region redraw into a true delta write: one new row of pixels per tick, with the rest of the region untouched by the MCU. Not applicable to the value-compare/redraw-changed-field pattern used by the current UI (`display_task`) — no scrolling widget exists today. |
| **Tearing effect line** — `TEON`(35h)/`TEOFF`(34h), `STE`(44h, set tear scanline) | Yes, §9.1.26–27, pp.211–213 | Drives a dedicated TE output pin with a vblank (or vblank+hblank) pulse the MCU can wait on before writing, to avoid visible tearing. | **Not usable as wired.** No TE pin is connected on this board (`lcd_st7789.h` pinout has no TE line) — would require a new GPIO. Even if wired, this driver's updates are already small, infrequent rects via DMA, not full-frame pushes, so tearing risk/benefit is different from the video-frame use case this feature targets. Skip unless tearing is actually observed. |
| **Idle mode** — `IDMON`(39h)/`IDMOFF`(38h) | Yes, §9.1.30–31, pp.220–222 | Global, whole-panel color reduction to a fixed 8-color palette (MSB of each R/G/B channel) at a lower frame frequency, for power saving. GRAM still holds full RGB565/666 data — only the *readout interpretation* changes; SPI write traffic is unaffected. | **Not useful.** Doesn't touch the write path or bytes transferred at all; it's a display-fidelity/power tradeoff, and 8-color output isn't acceptable for this UI's parameter chrome anyway. |
| **Display inversion** — `INVON`(21h)/`INVOFF`(20h) | Yes, §9.1.15–16, pp.188–190 | Confirmed **global**: both opcodes take zero parameters — there is no region argument. Flips the entire panel's color inversion state. | Already used correctly, once, at init, for this IPS panel's color-correctness quirk (per the code's own comment) — not toggled again, and there is no per-region variant to exploit. Confirms this is not a usable "boolean invert a rect" primitive. |
| **`RAMWR`(2Ch) vs `WRMEMC`/"RAMWRC"(3Ch)** | Yes, §9.1.22, p.202; §9.1.33, p.225 | `RAMWR` resets the column/page counters to the window start (`XS`,`YS`) and streams pixels with auto-increment until the window fills or another command arrives. `WRMEMC` **resumes** writing from wherever the counters were left (i.e., continues a write that was interrupted by some other command) instead of resetting to `XS`,`YS` — it does *not* let you address a new rectangle without `CASET`/`RASET`; the window bounds still come from the last `CASET`/`RASET`. | **No benefit here.** This driver never interrupts a blit mid-stream (`lcd_blit`/`lcd_fill_window` run a DMA transfer to completion before anything else touches the controller), so there's no in-progress write to resume. Would only matter for a future design that chunks one huge write across multiple commands/status polls. |
| **Memory read-back** — `RAMRD`(2Eh) / `RDMEMC`(3Eh) | Yes, §9.1.23, p.204; §9.1.34, p.227 | Reads 18-bit-per-pixel GRAM contents back to the MCU, with an explicit restriction: *"The Command 3Ah [`COLMOD`] should be set to 66h when reading pixel data from frame memory"* — i.e. reading requires switching `COLMOD` to 18bpp, different from this driver's runtime 16bpp write mode. Serial-interface read support also depends on wiring (Table 13, p.55): some IM-pin configurations expose a dedicated SDO read line, others multiplex reads back over the bidirectional SDA/data line — but all of them need an MCU-side data-**in** path. | **Unusable as wired, and the real prerequisite gap.** This driver only sets up SPI1 TX (`spi_write_blocking`, no RX) and only wires `DIN`/`CLK`/`CS`/`DC`/`RST`/`BL` — no MISO/SDO GPIO. `RAMRD` is exactly the capability any software-side AND/OR/XOR compositing scheme would need (since the controller has no compositing ALU, "boolean blit" can only ever mean: read old pixels out, combine with new pixels in MCU RAM, write the result back). That would need (a) a wiring change to add a read line, (b) a `COLMOD` mode switch around every read, and (c) a shadow framebuffer to combine into — none of which exist today (see above). Even with all three added, a read-modify-write round trip over SPI is strictly more MCU/SPI time than the current single overwrite, so it's a poor fit for a driver whose whole design point (per its own header comment) is staying off Core 1's audio DMA IRQ. |

### One more thing that stands out: `COLMOD`(3Ah) pixel depth

Not a partial-update mechanism, but a real, currently-unused lever on the
*existing* overwrite-everything write path: `COLMOD` supports 12-bit/444,
16-bit/565, and 18-bit/666 interface pixel formats (§9.1.32/§9.2, and the
restriction note on `RAMRD` above). This driver hardcodes 16bpp (`0x05`) and
computes RGB565 wire-format pixels in software (`gfx_rgb()` in `gfx.h`).
Switching the frequently-redrawn UI elements to 12bpp would cut SPI bytes
per blit by ~25% with no change to the CASET/RASET/RAMWR structure — at the
cost of reduced color fidelity (4096 colors instead of 65536). Worth a
mention since it's the one datasheet capability that shrinks the actual
bytes-on-the-wire for the write pattern this driver already uses, unlike
everything in the table above.

## Recommendations

1. **The controller cannot do boolean compositing, full stop.** Nothing in
   the 316-page ST7789V datasheet describes hardware AND/OR/XOR/blend
   operations on GRAM contents — it's write-only pixel RAM plus a scan-out
   state machine. Any "boolean operator" style update would have to be
   software, MCU-side, against a framebuffer this driver doesn't currently
   keep. That's two gaps, not one: no read path is wired to the chip, and no
   shadow buffer exists in `gfx.cpp` to combine into. Adding both, then
   paying a read-modify-write SPI round trip per update, would very likely
   be *slower* than the current single-write overwrite for the small rects
   this driver already targets — not recommended.
2. **If a scrolling widget is ever wanted** (waveform trace, tracker log,
   history strip), `VSCRDEF`/`VSCSAD` is a genuine, datasheet-confirmed
   optimization: one new row written per tick via ordinary `RAMWR`, the rest
   of the region moved by the panel's own scan-out addressing, no unusable
   pixel data touched. This is the only capability here that reduces the
   *amount of pixel data written* for a real, plausible use case in this
   codebase.
3. **`COLMOD` at 12bpp** is a low-effort, code-only way to cut SPI bytes per
   blit ~25% on the existing overwrite pattern, if the resulting color
   banding is acceptable for the UI chrome.
4. Partial area, idle mode, and display inversion are real chip features
   but solve different problems (power/blanking, global fidelity tradeoff,
   whole-panel invert) than "optimize frequent small updates" — none of them
   reduce write traffic for this driver's actual usage pattern.
5. Tearing-effect sync and memory read-back are both gated on GPIOs this
   board doesn't wire up (TE pin, MISO/SDO) — out of reach without a
   hardware change, and of doubtful value here even if added.
