# The e-paper display seam — nsl paints the MagTag

**Date:** 2026-07-23
**Status:** design, approved, not implemented
**Issue:** tuvm#88
**Follows:** `feat/magtag-esp32s2-port` — b50e89c (speaker rung promoted). Buttons,
LEDs, outbound GPIO, WiFi and PWM sound are all proven on this board; it has no output
a human can read.
**Companion:** `review/zephyr-driver-exposure.md` (2026-07-11) — the pre-bring-up sketch
that named the display as the flagship Tier-2 example. This document is that sketch
settled against the hardware actually on the bench, and it revises the sketch in two
places (noted in D3 and D6).

## Scope

An nsl node writes text to the MagTag's 2.9" e-paper panel and refreshes it — full
panel and a single row — on both the host and the ESP32-S2. That is the whole rung.

**Out of scope, deliberately:**

- **The ship's-bell clock.** WiFi/NTP + DAC bell + display converged on one node is the
  motivating demo (`review/program-sequence.md`, the speaker rung's close-out), and it
  stacks #85's arena arithmetic and the DAC work on top of an unproven panel. It is a
  later rung that this one unblocks.
- **Grayscale.** The panel is 4-level; Zephyr's `uc81xx` is mono-only
  (`uc81xx.c:477` advertises `PIXEL_FORMAT_MONO10` and nothing else). B/W is what this
  rung wants and B/W is what the driver family gives, so the gap costs nothing.
- **A non-blocking refresh.** See D6.
- **TFT.** See D7.

## The problem

Two problems, one of which was a surprise.

**The expected one:** nothing in the VM can reach a display. The gpio seam (#82) drives
a pin; there is no path from nsl to a framebuffer, and per G3 a framebuffer must not
live on the VM heap anyway.

**The surprise:** *the panel on the bench has no Zephyr driver.* The MagTag shipped in
two revisions —

> "As of July 22, 2025 – The display has been updated (old one discontinued); it now
> uses a modern **SSD1680** chip instead of the discontinued ILI0373"
> — `zephyr/boards/magtag/adafruit-magtag.pdf:470`

The bench board is the **original, IL0373**. Zephyr supports the 2025 SSD1680 in-tree
(`solomon,ssd1680.yaml` → `ssd16xx.c`) and supports the IL0373 **nowhere** — verified
absent from both workspaces, `~/zephyr-esp32/zephyr` @ v4.2.0 and `~/ncs/zephyr` @
4.2.99.

That is not the setback it first looks like. It is the more interesting result: it turns
the rung from *consuming* the Tier-1 catalog into *extending* it, which is a stronger
demonstration of the same thesis. The cost is bounded because the IL0373 is a UC81xx in
all but name — see D1.

## Decisions

### D1 — The IL0373 joins the existing `uc81xx` family; no new driver is written

The evidence that this is a family member, not a stranger:

| IL0373 needs | `uc81xx` today | verdict |
|---|---|---|
| PSR 0x00, PWR 0x01, POF 0x02, PON 0x04, BTST 0x06, DTM1 0x10, DRF 0x12, DTM2 0x13, PLL 0x30, CDI 0x50, TRES 0x61, VDCS 0x82 | byte-identical opcodes, `uc81xx_regs.h:11-63` | free |
| BUSY active low | `ultrachip,uc81xx-common.yaml:20` — "The BUSY pin of UC81xx is active low" | free |
| Mono / KW mode | driver is KW-only by construction (`uc81xx.c:25`) | free |
| Single-byte CDI | `uc8176_set_cdi` (`uc81xx.c:635`) | free |
| RAM geometry 160 × 296 | `uc8175_quirks` caps at 80 × 160; `uc8176` at 400 × 300 | **new quirks struct** |
| TRES = 1-byte hres + 2-byte vres | only `tres8` (2 B) and `tres16` (4 B) exist, `uc81xx_regs.h:109,126` | **new variant** |
| PTL = 1 B + 1 B + 2 B + 2 B + 1 B | only `ptl8` (5 B) and `ptl16` (9 B) exist, `uc81xx_regs.h:116,133` | **new variant** |

The opcode list and the TRES byte layout are read off Adafruit's own CircuitPython
driver, which is on the bench at `zephyr/boards/magtag/adafruit-circuitpython-il0373-py-2.0.5.zip`
(`_START_SEQUENCE`, and `start_sequence[26..28] = width & 0xFF, height >> 8, height & 0xFF`),
cross-checked against `uc81xx_regs.h`. `ram_width=160, ram_height=296, busy_state=False`
in that driver's `super().__init__` is where the geometry and the BUSY polarity come from.

So the change is: **one binding yaml + one quirks struct + two ~25-line functions**, each
sitting in an extension point the family already provides. No new `.c` file in Zephyr, no
new subsystem.

New byte layouts, mirroring the existing pairs and using `sys_cpu_to_be16` exactly as
`uc81xx_set_tres_16` does (`uc81xx.c:606`):

```c
struct uc81xx_tres_il0373 { uint8_t  hres; uint16_t vres; } __packed;  /* 3 */
struct uc81xx_ptl_il0373  { uint8_t  hrst; uint8_t  hred;
                            uint16_t vrst; uint16_t vred;
                            uint8_t  flags; } __packed;                /* 7 */
```

`il0373_quirks` = `{ .max_width = 160, .max_height = 296, .auto_copy = false,
.set_cdi = uc8176_set_cdi, .set_tres = …_il0373, .set_ptl = …_il0373 }`.

**Naming.** The compatible needs a vendor prefix that exists in
`dts/bindings/vendor-prefixes.txt`. The controller is a UC8151-class part sold under the
IL number; the implementation choice is `ultrachip,il0373`, which keeps it in the family
directory and needs no new prefix. If `vendor-prefixes.txt` argues otherwise at
implementation time, that is a one-line correction, not a design change.

### D2 — Partial refresh is in scope, because the family already implements it generically

`uc81xx.c:398-460` is controller-independent: with blanking off, a `display_write`
becomes PTIN → PTL(window) → DTM2 → refresh → PTOUT, and (because `auto_copy` is false)
a second PTL + DTM1 to sync the back buffer. The **only** per-controller piece is
`set_ptl`, which D1 already budgets. Partial therefore costs one struct and one function
beyond full refresh, not a second mechanism.

**The honest risk, stated up front.** With no `partial` profile child node in devicetree,
the driver reuses the *full-refresh OTP waveform* and merely scans a smaller window
(`ultrachip,uc81xx-common.yaml`, child-binding description). So the first partial refresh
will very likely still flash the window and may ghost. A true no-flash partial needs
custom LUTs — which devicetree already supports (`lutc`/`lutww`/`lutkw`/`lutwk`/`lutkk`/`lutbd`
on the profile child node), so the *mechanism* is free and only the *values* are unknown.

Where to look for values, in order: `GxEPD2_290_T5` — the Arduino library the vendor
names for the original MagTag (`adafruit-magtag.pdf:516`) — then the UC8151 datasheet's
example LUTs. Adafruit's CircuitPython IL0373 driver has **no** partial mode, so it is
not a source. **This is a bench-time unknown and the plan must timebox it:** if the LUT
hunt exceeds its box, the rung ships with window-scanned partial (flashing but correct)
and the no-flash LUT becomes a follow-up issue. Correct-but-flashing still passes the
gate in D8; it is the pixels that must be right.

### D3 — nsl speaks characters, via Zephyr's CFB; it never sees a pixel

`subsys/fb/cfb.c` is in-tree, mono, and ships default fonts. At 128 × 296 with CFB's
`font1016` (10 × 16 px) that is a **12-column × 18-row grid** — see D9: this driver family
cannot rotate, so the grid is portrait.

**Correction, found by the final whole-branch review.** Earlier drafts of this document said
8 × 8 and therefore 16 × 37. That was wrong: **CFB ships no 8 × 8 font.** It has exactly
three — `font1016`, `font1524`, `font2032` (`cfb_fonts.c:5156-5172`) — and index 0, which the
seam uses, is `font1016`. The 8 came from CFB's `ppt`, which is a *tiling granularity*
hardcoded to 8 (`cfb.c:724`), not a font size. `font1016` was chosen over the larger two
because 12 characters per line is the narrowest usable width for status text; `font1524`
would give only 8. **Geometry now lives in ONE place** — `OEVM_DISPLAY_ROWS`/`COLS` in
`src/platform_backend.h` — and the device bring-up hard-checks its derived values against
them and refuses to come up on mismatch, because a printed number a human is meant to eyeball
is not an enforcement mechanism. This is the "ascii-art UI" decision of
`review/zephyr-driver-exposure.md` Tier 2, taken as written.

Compatibility verified rather than assumed — this was the check most likely to kill the
choice:

- CFB branches on `SCREEN_INFO_MONO_VTILED` (`cfb.c:279`) and falls to the horizontal
  path `draw_char_htmono` (`cfb.c:221`) for drivers without it. `uc81xx` advertises
  `SCREEN_INFO_MONO_MSB_FIRST | SCREEN_INFO_EPD` (`uc81xx.c:477`) — no VTILED, so the
  htmono path is taken.
- CFB honours MSB-first (`cfb.c:105,227,441`).
- `PIXEL_FORMAT_MONO10` is handled (`cfb.c:578`).

**Revises the sketch:** the 2026-07-11 doc hedged between CFB "where the panel's pixel
format allows" and a hand-rolled ~1 KB glyph blitter. It allows. The blitter is dropped.

**Where the buffer lives.** `cfb.c:687` does `fb->buf = k_malloc(fb->size)` — 296 × 128 ÷ 8
= **4,736 B** from the kernel heap. That is a C arena, not the VM heap, so G3 holds; but
it is not free, and `CONFIG_HEAP_MEM_POOL_SIZE` must rise to cover it plus slack. Per the
#53 discipline this gets **measured on the linked image, not assumed**.

**CFB cannot express a partial refresh, so CFB grows one.** Found while writing the
implementation plan, and it is a direct conflict between this decision and D2:
`cfb_framebuffer_finalize` (`cfb.c:558-585`) always writes the *whole* framebuffer at
(0, 0), and `char_fb` is a file-static with no public buffer accessor — so nothing outside
the subsystem can hand `display_write` a sub-rectangle.

The resolution is to **add a bounded finalize to CFB**:

```c
int cfb_framebuffer_finalize_area(const struct device *dev,
                                  uint16_t x, uint16_t y,
                                  uint16_t width, uint16_t height);
```

It is `finalize`'s body with the descriptor and origin taken from the arguments instead of
`fb->x_res`/`fb->y_res`/0,0, plus the same `cfb_invert` dance around the write. ~20 lines.

The alternatives were rejected: doing partial-*waveform*-but-full-*window* (blanking off at
init) makes gate 2 unprovable, and hand-rolling a framebuffer plus glyph blitter reverses
this decision and re-adds the ~1 KB blitter for the sake of one rectangle. Patching CFB
keeps D2 and D3 both whole. It does mean **the Zephyr-side patch spans a driver family and
a subsystem, not just a driver** — see D10.

Bounds discipline: the area is derived from the font metrics inside the seam, never passed
in by nsl, so a caller still cannot state a pixel coordinate (D4).

### D4 — Four prims at 125-128, blocking, mirroring the #82 gpio seam

| id | prim | nsl | blocks? |
|---|---|---|---|
| 125 | `displayClear` | `display clear` | no — CFB buffer only |
| 126 | `displayAt:col:put:` | `display at: 1 col: 0 put: 'BELL 4'` | no — CFB buffer only |
| 127 | `displayRefresh` | `display refresh` | **yes** — full panel |
| 128 | `displayRefreshRow:` | `display refreshRow: 1` | **yes** — partial window |

`src/prim_ids.h` is append-only and counted-not-computed; 123/124 are `gpioSet:to:`/
`gpioLevel:`, so these are 125-128 and the compile-time parity guard at `src/dispatch.c:837`
advances from `PRIM_pf_gpioLevel` to the last of them.

**Why four and not fewer.** `clear` is separate from `refresh` because clearing the buffer
and driving the panel are different costs by three orders of magnitude, and fusing them
would make the cheap operation silently expensive. `refreshRow:` is separate from
`refresh` rather than a range argument on it because the row is the grid's unit — the
partial window is computed from the font metrics, not supplied by the caller, so nsl
never states a pixel coordinate. That is the same instinct as D2 of the gpio design
(silkscreen numbers, not raw pins): keep the caller in the units it actually thinks in.

**No `displayText:` convenience wrapper**, for the same reason #82 refused `toggle:` — an
append-only table cannot reclaim an id spent on sugar.

### D5 — Failure raises with a tag, in nsl, over a nil from C

Exactly the division #82 established (`Vector>>at:` over `basicAt:`), for the same reason:
`build_and_raise` is static to `dispatch.c`.

| condition | C answers | nsl raises |
|---|---|---|
| no display node on this board | `nil` | `#displayUnavailable` |
| row/col outside the 12 × 18 grid | `nil` | `#displayBounds` |
| driver returned `-EIO` | `nil` | `#displayFault` |

`#displayFault` is the one that earns its keep. The speaker rung's lesson was that
`rc=0` is not evidence of a physical effect (`review/program-sequence.md`, speaker gotcha
3); the inverse also holds — a driver error that gets discarded turns a dead panel into a
silent no-op, which is precisely the failure mode `diagnosing-a-wedged-node` warns costs
hours. It raises.

### D6 — The refresh blocks the VM, and that is safe here

`uc81xx_busy_wait` (`uc81xx.c:100-106`) polls the BUSY pin with `k_sleep(K_MSEC(1))` in
the *calling* thread, so `display refresh` stalls the VM for the whole multi-second paint.

**Verified, not assumed, that this is a responsiveness cost and not a correctness one:**
`src/backend/zephyr/pulse_zephyr.c:42` — *"Shed, never k_oops. A full mailbox is a load
condition, not a broken…"* (the tuvm#34 fix) — and the mailbox coalesces per source
(`pulse_zephyr.c:31-34`). So a button pressed during a paint is delivered **once** after
it: not lost, not duplicated, not a node kill. The initial hypothesis that a long block
could overflow the mailbox into a Tier-2 crash was checked and is false.

For a panel that repaints on the order of once a minute, blocking is the honest trade.

**The alternative, and why it is a later rung.** The async shape — `refresh` posts to a
dedicated C driver thread and completion returns as a pulse event — is the "driver job"
`review/zephyr-driver-exposure.md` Tier 2 actually describes, and it is the better
eventual answer. It costs a thread stack on a dram0 already at 81.82%, invents a
completion-event vocabulary that does not exist, and adds a mid-paint re-request failure
mode. Shipping the simple seam first and migrating in its own rung is the #82 → #84
discipline applied again: do not replace a proven seam in the rung that first ships its
replacement. **The ship's-bell clock is what will force it**, because that node must ring
and watch buttons while painting.

**Revises the sketch:** the 2026-07-11 doc assumed a C-side driver *job* from the start.
This rung defers the job and keeps the binding.

### D7 — Validating on e-paper first is a RAM decision, and TFT is the falsifiable claim

`drivers/display/` ships e-paper (`ssd16xx`, `uc81xx`) and TFT (`st7789v`, `st7735r`,
`ili9xxx`) behind one `display.h`. Binding the API family is meant to buy both. E-paper
goes first purely on RAM: mono 296 × 128 is 4,736 B against RGB565 320 × 240 at 150 KB,
and the S2 has no PSRAM in play (`review/program-sequence.md`, MagTag caveat 9).

**The claim this rung leaves standing, and how it dies:** if adding a TFT later needs more
than a devicetree node plus a larger buffer, the Tier-1 "bind the family, inherit the
catalog" claim is wrong — **and that is the finding**, to be written down rather than
worked around. Nothing in this design should be shaped to protect the claim.

### D8 — Host gets a real grid stub, not just the raise paths

The posix backend keeps the 12-column × 18-row character grid in a plain array, so host tests assert
on **rendered text**, not merely on error tags.

This deliberately closes the hole #82 left: there, `set:to:` created a slot on the host
and so could not raise, leaving the raise with host coverage only through `level:`
(that design's Risk 4). A display stub that renders lets the host cover placement,
overwrite, clear, and bounds — the behaviour — and leaves only the physical refresh to
hardware.

### D9 — Devicetree: `spi2` (FSPI), and BUSY is wired after all

Read off the vendor pinout (`zephyr/boards/magtag/magtag-overview.png`, DISPLAY block):

| signal | GPIO | note |
|---|---|---|
| EPD_BUSY | 5 | gpio0, RTC-capable |
| EPD_RESET | 6 | gpio0, RTC-capable |
| EPD_DC | 7 | gpio0, RTC-capable |
| EPD_CS | 8 | gpio0 — a plain GPIO, **not** FSPICS0 (which is GPIO10) |
| EPD_MOSI | 35 | FSPID → gpio1 |
| EPD_SCK | 36 | FSPICLK → gpio1 |
| EPD_MISO | 37 | FSPIQ → gpio1 |

This confirms the note already in `zephyr/boards/esp32s2_saola_esp32s2.overlay:22`.

**BUSY was the rung's top risk and it is retired.** Adafruit's own examples pass
`#define EPD_BUSY -1` (`adafruit-magtag.pdf:5123`, and again at 5757/5959), while Zephyr's
`ultrachip,uc81xx-common.yaml:17` makes `busy-gpios` **`required: true`** and the driver
polls it. Had BUSY not been broken out, the choice would have been between a fabricated
always-inactive GPIO and patching the binding. The pinout shows it reaching **GPIO5**, so
the requirement is satisfied honestly and Adafruit simply chooses not to use it.

**The panel stays 128 W × 296 H — this family cannot rotate, and the rung ships portrait.**
An earlier draft of this decision specified `rotation = <270>` for a 296 × 128 landscape
grid. That was wrong, and the error is worth recording because it is an easy one to repeat:
`rotation` is declared in **`solomon,ssd16xx-common.yaml:34`**, the *other* e-paper family,
and **not** in `ultrachip,uc81xx-common.yaml`. `uc81xx.c` contains no rotation logic at all,
and Zephyr's edtlib treats an undeclared property as a hard error, so the devicetree would
not even have generated.

Nor is adding it cheap. `ssd16xx`'s rotation (`ssd16xx.c:890`) only writes a controller
scan-direction register at init; the evidence says the IL0373 has no equivalent, because
**Adafruit's CircuitPython driver performs rotation in the displayio *software* layer**, not
on the controller. CFB has no rotation either. Landscape therefore means software-rotating a
framebuffer — a second 4,736 B buffer plus a transpose blitter, on a `dram0` already at
81.8%, and partly re-creating the blitter D3 dropped.

So the character grid is **portrait: 16 columns × 37 rows**, and **node text is capped at 12
characters per line** or it raises `#displayBounds`.

**Portrait is the design target, not a concession** (David, 2026-07-23): screens and the
clock face are to be designed for a 12 × 18 portrait grid. Landscape is therefore not a
deferred goal — it is simply not wanted, which removes the whole question from the clock
rung's path. `tuvm#89` remains open only as the record of *why* 90/270 is expensive and of
the fact that a 2025 SSD1680 board would get it free.

**180° IS available, and free.** Distinguish it sharply from 90/270: a 180° rotation is a
double axis flip, which this controller does in hardware. `uc81xx.c:198-203` hardcoded

```c
uint8_t psr = UC81XX_PSR_KW_R | UC81XX_PSR_UD | UC81XX_PSR_SHL | UC81XX_PSR_SHD | UC81XX_PSR_RST;
```

where `UD` (bit 3) is gate scan direction and `SHL` (bit 2) is source shift direction
(`uc81xx_regs.h:84-85`). Clearing both flips both axes — no framebuffer work, no second
buffer, no blitter, one register write. This rung adds a `rotate-180` devicetree boolean to
`ultrachip,uc81xx-common.yaml` to expose it, which is a further upstream-quality addition to
the family (see D10).

Its purpose is physical: it chooses **which end of the portrait panel the USB-C connector
sits at**. Which of the two states puts it at the bottom is a fact about how the panel is
mounted and cannot be determined from documentation — the knob ships **off**, and the bench
flips it if the first paint comes out inverted. That is one extra build, not a redesign.

Between `spi2` and the display node sits a `zephyr,mipi-dbi-spi` shim
(`drivers/mipi_dbi/mipi_dbi_spi.c`) — `uc81xx` is a MIPI-DBI device, not a raw-SPI one
(`ultrachip,uc81xx-common.yaml:12` includes `mipi-dbi-spi-device.yaml`).

### D10 — The Zephyr change is a local commit on the branch that already carries one

`~/zephyr-esp32/zephyr` is on `tuvm-c6-radio-fix` @ cbc06d38482 and **must stay there** or
C6 Thread regresses. The IL0373 support rides as a further commit on that branch, with the
same "vendored, not upstreamed" status the radio fix has.

Upstreaming is explicitly **not** decided by this rung. It is a good candidate for a
second Zephyr PR after `zephyr#114163`, and the code should be written to that standard
so the option stays open — but the rung closes on hardware, not on review. Note the
standing tax: `west update` already breaks two boards' ROM patches (#48); this adds a
third out-of-tree dependency to the same list.

**The patch is two separable pieces** (D3): a new `uc81xx` family member, and
`cfb_framebuffer_finalize_area` in `subsys/fb`. They should be **separate commits**,
because they would be separate upstream PRs — the driver one is a straightforward new
compatible, while the subsystem one changes a public header and will attract more review.
Keeping them apart also means the CFB half can be dropped without losing the driver half
if it turns out upstream has a better idea.

## Touch list

**Zephyr (`~/zephyr-esp32/zephyr`, branch `tuvm-c6-radio-fix`)**
- `dts/bindings/display/ultrachip,il0373.yaml` — new, includes the common binding
- `drivers/display/uc81xx_regs.h` — two packed structs + `BUILD_ASSERT`s
- `drivers/display/uc81xx.c` — `set_tres`/`set_ptl` variants, `il0373_quirks`,
  `DT_FOREACH_STATUS_OKAY_VARGS(ultrachip_il0373, …)`
- `drivers/display/Kconfig.uc81xx` — the new compatible in the `depends on` list
- `subsys/fb/cfb.c` + `include/zephyr/display/cfb.h` — `cfb_framebuffer_finalize_area`
  (D3); a subsystem addition, not a driver one

**tuvm**
- `src/prim_ids.h` — 125-128, append-only
- `src/dispatch.c` — four prim cases; parity guard advanced
- `src/platform.c` + backend split via `#ifdef __ZEPHYR__` (D6 of the gpio design —
  **no new `.c` file**, so the nine-source-list tax #63 is avoided)
- `src/backend/posix/` — the grid stub (D8)
- nsl kernel — the `display` accessor and the four raises (D5)
- `zephyr/boards/esp32s2_saola_esp32s2.overlay` — `&spi2` + mipi-dbi + display node
- `zephyr/prj_esp32s2.conf` — `CONFIG_DISPLAY`, `CONFIG_MIPI_DBI`, `CONFIG_SPI`,
  `CONFIG_CHARACTER_FRAMEBUFFER`, raised `CONFIG_HEAP_MEM_POOL_SIZE`
- `zephyr/MagTagDisplayNode.nsl` — the gate node
- `nsl-tests/14_display/` — host tests

## Verification

**Host** (`nsl-tests/14_display/`, run with `tools/run-one.py` for the inner loop —
`run_tests_oevm.py` always globs the whole suite):
1. text placed at a row/col reads back from the stub grid
2. overwrite and `clear` behave
3. row/col outside the 12 × 18 grid raises `#displayBounds`
4. a board with no display raises `#displayUnavailable`

Then the full suite green (214/0 as recorded at the #82 rung; re-baseline before
trusting the delta) — and the **gate line** read, not the
"0 failed" line (#63).

**Device** (MagTag, over the soldered UART console — the native USB does not enumerate
under Zephyr; flash with `zephyr/flash-magtag.sh`):
- **Gate 1 — full refresh.** `MagTagDisplayNode` paints a screen of text; the panel
  visibly shows it.
- **Gate 2 — partial refresh.** Button A bumps a counter and calls `refreshRow:`;
  **only that row changes.** This is the gate that actually proves partial, and it is
  passed by correct pixels even if the window flashes (D2).
- **Gate 3 — the fault path.** A deliberately failing refresh raises `#displayFault`
  rather than printing nothing (D5).
- **dram0 recorded** from the linked image, and `tools/heap-ceiling.sh` re-run before any
  `OEVM_HEAP_SIZE` claim is quoted.

A photograph of the panel is the artifact for gates 1 and 2. Per
`diagnosing-a-wedged-node`, a `rc=0` return is **not** evidence the panel changed.

## Risks

1. **Partial refresh needs LUTs we do not have** (D2). Likeliest cost overrun in the rung.
   *Mitigation:* timebox the LUT hunt; ship window-scanned partial and file the no-flash
   LUT as a follow-up. Gate 2 passes either way.
2. **The IL0373 is *nearly* a UC81xx and the gap is somewhere unmeasured** — most likely
   the PSR panel-setting bits, where CircuitPython patches resolution-select bits
   (`start_sequence[17]`) that `uc81xx_controller_init` writes its own way.
   *Mitigation:* bring up with a logic-free bisect — confirm BUSY toggles at all before
   trusting any pixel; a panel that never asserts BUSY means the init never landed.
3. **dram0.** The S2 build is at 81.82% of 217 KB; CFB adds 4,736 B plus the SPI/MIPI-DBI/
   display drivers. It should fit at a 64 KB arena, but #53 is the standing lesson that
   "should fit" is not a measurement. *Mitigation:* measure before claiming; the arena is
   the lever (#85 already halves it for WiFi).
4. **`west update` breaks a third thing** (D10, #48). Accepted, documented.
5. **The 2025 SSD1680 board is a different rung.** Anyone reproducing this on a
   post-July-2025 MagTag needs `solomon,ssd1680` instead — which is *easier* (in-tree) but
   is not what this design describes. Worth a line in the ladder so the next person is not
   surprised.

## Close-out (promotion rule)

Not closed until:
- `review/program-sequence.md` gains the display rung, including **both** MagTag panel
  revisions and which one this was proven on;
- `review/zephyr-driver-exposure.md` is updated where this rung revised it — CFB chosen
  over the glyph blitter (D3), the driver *job* deferred in favour of a blocking binding
  (D6), and the catalog claim recorded as *extended* (a new family member) rather than
  merely consumed;
- `review/tests/CLAIMS.md` gains a row for the tagged raises (a behavioural guarantee),
  as #82's did — **no** performance row, since nothing here is a performance claim;
- the TFT claim of D7 is left explicitly open, so the rung that tests it knows it is a
  test and not a formality.
