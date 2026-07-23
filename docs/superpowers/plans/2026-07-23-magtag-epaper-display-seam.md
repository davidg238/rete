# MagTag e-paper display seam — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An nsl node writes text to the MagTag's 2.9" e-paper panel and refreshes it — full panel and a single row — on both the host and the ESP32-S2.

**Architecture:** Four blocking prims (125-128) sit over Zephyr's CFB character framebuffer, which sits over a new `ultrachip,il0373` member of the in-tree `uc81xx` driver family, which sits over MIPI-DBI-on-SPI. nsl never sees a pixel; the framebuffer lives in a C arena (G3). Host and device implementations of the same four `display_target_*` functions are selected by `#ifdef __ZEPHYR__` inside existing `.c` files — no new source file, so the nine-enumerated-source-list tax (tuvm#63) is avoided.

**Tech Stack:** C99, Zephyr 4.2, devicetree, nsl, Xtensa ESP32-S2.

**Design:** `docs/design/2026-07-23-magtag-epaper-display-seam-design.md` (tuvm#88). Every decision reference below (D1-D10) points there.

## Global Constraints

- **Repos.** tuvm at `~/workspace/tuvm`, branch `feat/magtag-esp32s2-port`. nsl kernel at `~/workspace/nsl-tuvm`. nsl tests at `~/workspace/nsl-tests`. Zephyr at `~/zephyr-esp32/zephyr`.
- **The Zephyr checkout must stay on branch `tuvm-c6-radio-fix`.** Moving it regresses C6 Thread (tuvm#59). Commit onto that branch; never check out another.
- **`src/prim_ids.h` is append-only and counted-not-computed.** New ids are obtained by **counting `PRIM_NAMES` entries**, never by arithmetic on the previous id. Advance the parity guard at `src/dispatch.c:837` to the new last id or the build must fail.
- **nsl source extension is `.nsl`.** A bare `.ns` means frozen oracle / soup / archive.
- **nsl has no modulo.** `//` is integer division.
- **An app's `platform` is the kernel instance** — `string:`, `basicNew:indexable:` and `Array` are not reachable from a node.
- **Single-test inner loop:** `~/workspace/nsl-tuvm/tools/run-one.py <file.nsl>`. `run_tests_oevm.py` ignores argv and always globs the whole ~2-3 min suite. **A SKIP proves nothing** — it returns before compiling.
- **Never run two nsl suites at once** (shared `/tmp`).
- **Read the gate line, not "0 failed"** — the suite prints `0 failed` while aux gates are red (tuvm#63).
- **Flashing the MagTag is manual.** Hold BOOT0, click RESET. Use `zephyr/flash-magtag.sh`. The native USB does not enumerate under Zephyr; the soldered UART header is the only console. **Never trust a `/dev/ttyUSB*` number** — identify by `esptool read_mac`.
- **`rc=0` is not evidence of a physical effect.** A gate that claims the panel changed needs a photograph.

---

### Task 1: The IL0373 joins the `uc81xx` driver family

Adds a new compatible to Zephyr's existing e-paper family. No new driver, no new `.c` file. Deliverable: a build that instantiates the driver for a devicetree node declaring `ultrachip,il0373`.

**Files:**
- Modify: `~/zephyr-esp32/zephyr/drivers/display/uc81xx_regs.h` (append after the `ptl16` block, ~line 141)
- Modify: `~/zephyr-esp32/zephyr/drivers/display/uc81xx.c`
- Modify: `~/zephyr-esp32/zephyr/drivers/display/Kconfig.uc81xx:9`
- Create: `~/zephyr-esp32/zephyr/dts/bindings/display/ultrachip,il0373.yaml`

**Interfaces:**
- Consumes: nothing.
- Produces: devicetree compatible `ultrachip,il0373`, accepting every property of `ultrachip,uc81xx-common.yaml` (`width`, `height`, `busy-gpios`, `rotation`, `softstart`, and `full`/`partial` profile child nodes).

- [ ] **Step 1: Confirm the branch, and that the compatible does not already exist**

```bash
cd ~/zephyr-esp32/zephyr
git rev-parse --abbrev-ref HEAD          # MUST print: tuvm-c6-radio-fix
grep -ri "il0373" drivers/display/ dts/bindings/display/ | wc -l   # MUST print: 0
```

Expected: `tuvm-c6-radio-fix` and `0`. If the branch is anything else, **stop** — checking out another branch regresses C6 Thread.

- [ ] **Step 2: Add the two byte layouts**

Append to `uc81xx_regs.h` immediately after the `BUILD_ASSERT(sizeof(struct uc81xx_ptl16) == 9);` line:

```c
/* IL0373 (tuvm#88). A third TRES/PTL width: the horizontal fields are one byte
 * (the panel is at most 160 px wide, addressed in units of 8) while the vertical
 * ones are two (up to 296 rows). Neither the uc8175 8-bit pair nor the uc8176/79
 * 16-bit pair can express that, which is the only reason this controller needs
 * quirks of its own. Byte order is big-endian on the wire, applied by the
 * accessors in uc81xx.c via sys_cpu_to_be16 — NOT here.
 */
struct uc81xx_tres_il0373 {
	uint8_t  hres;
	uint16_t vres;
} __packed;

BUILD_ASSERT(sizeof(struct uc81xx_tres_il0373) == 3);

struct uc81xx_ptl_il0373 {
	uint8_t  hrst;
	uint8_t  hred;
	uint16_t vrst;
	uint16_t vred;
	uint8_t  flags;
} __packed;

BUILD_ASSERT(sizeof(struct uc81xx_ptl_il0373) == 7);
```

- [ ] **Step 3: Widen the `uc8176_set_cdi` guard to include the IL0373**

The IL0373 reuses `uc8176_set_cdi` verbatim (single-byte CDI). Its `#if` currently excludes us, so the function would not be compiled and the quirks struct would fail to link.

In `uc81xx.c`, change this line (currently at 634):

```c
#if DT_HAS_COMPAT_STATUS_OKAY(ultrachip_uc8175) || DT_HAS_COMPAT_STATUS_OKAY(ultrachip_uc8176)
```

to:

```c
#if DT_HAS_COMPAT_STATUS_OKAY(ultrachip_uc8175) || DT_HAS_COMPAT_STATUS_OKAY(ultrachip_uc8176) || \
	DT_HAS_COMPAT_STATUS_OKAY(ultrachip_il0373)
```

- [ ] **Step 4: Add the accessors and the quirks struct**

Insert into `uc81xx.c` immediately after the `#endif` that closes the `uc8176_set_cdi` block (i.e. after what is currently line 655, before `#if DT_HAS_COMPAT_STATUS_OKAY(ultrachip_uc8175)` / `uc8175_quirks`):

```c
#if DT_HAS_COMPAT_STATUS_OKAY(ultrachip_il0373)
static int uc81xx_set_tres_il0373(const struct device *dev)
{
	const struct uc81xx_config *config = dev->config;
	const struct uc81xx_tres_il0373 tres = {
		.hres = config->width,
		.vres = sys_cpu_to_be16(config->height),
	};

	LOG_HEXDUMP_DBG(&tres, sizeof(tres), "TRES");

	return uc81xx_write_cmd(dev, UC81XX_CMD_TRES, (const void *)&tres, sizeof(tres));
}

static inline int uc81xx_set_ptl_il0373(const struct device *dev, uint16_t x, uint16_t y,
					uint16_t x_end_idx, uint16_t y_end_idx,
					const struct display_buffer_descriptor *desc)
{
	const struct uc81xx_ptl_il0373 ptl = {
		.hrst  = x,
		.hred  = x_end_idx,
		.vrst  = sys_cpu_to_be16(y),
		.vred  = sys_cpu_to_be16(y_end_idx),
		.flags = UC81XX_PTL_FLAG_PT_SCAN,
	};

	/* Setup Partial Window and enable Partial Mode */
	LOG_HEXDUMP_DBG(&ptl, sizeof(ptl), "ptl");

	return uc81xx_write_cmd(dev, UC81XX_CMD_PTL, (const void *)&ptl, sizeof(ptl));
}

static const struct uc81xx_quirks il0373_quirks = {
	.max_width  = 160,
	.max_height = 296,

	.auto_copy = false,

	.set_cdi  = uc8176_set_cdi,
	.set_tres = uc81xx_set_tres_il0373,
	.set_ptl  = uc81xx_set_ptl_il0373,
};
#endif
```

Then add the instantiation at the end of the file, after the `ultrachip_uc8179` one (currently line 816):

```c
DT_FOREACH_STATUS_OKAY_VARGS(ultrachip_il0373, UC81XX_DEFINE,
			     &il0373_quirks);
```

- [ ] **Step 5: Add the binding**

Create `dts/bindings/display/ultrachip,il0373.yaml`:

```yaml
# Copyright (c) 2026 David Girle
# SPDX-License-Identifier: Apache-2.0

description: UltraChip IL0373 EPD controller

compatible: "ultrachip,il0373"

include: ultrachip,uc81xx-common.yaml
```

- [ ] **Step 6: Let Kconfig build the driver for the new compatible**

In `Kconfig.uc81xx`, change line 9 from:

```
	depends on DT_HAS_ULTRACHIP_UC8175_ENABLED || DT_HAS_ULTRACHIP_UC8176_ENABLED || DT_HAS_ULTRACHIP_UC8179_ENABLED
```

to:

```
	depends on DT_HAS_ULTRACHIP_UC8175_ENABLED || DT_HAS_ULTRACHIP_UC8176_ENABLED || DT_HAS_ULTRACHIP_UC8179_ENABLED || DT_HAS_ULTRACHIP_IL0373_ENABLED
```

- [ ] **Step 7: Verify the vendor prefix is accepted**

```bash
cd ~/zephyr-esp32/zephyr
grep -n "^ultrachip" dts/bindings/vendor-prefixes.txt
```

Expected: a line registering `ultrachip`. If absent, the build will warn about an unknown vendor prefix — in that case add `ultrachip	UltraChip Inc.` to that file and note the deviation in the commit message. (D1 anticipated this as a one-line correction, not a design change.)

- [ ] **Step 8: Commit — driver half only**

The CFB change in Task 3 lands as its own commit; D10 requires them separable because they would be separate upstream PRs.

```bash
cd ~/zephyr-esp32/zephyr
git add drivers/display/uc81xx.c drivers/display/uc81xx_regs.h \
        drivers/display/Kconfig.uc81xx dts/bindings/display/ultrachip,il0373.yaml
git commit -m "drivers: display: uc81xx: add IL0373 support

The IL0373 shares the UC81xx command set exactly (PSR/PWR/POF/PON/BTST/
DTM1/DRF/DTM2/PLL/CDI/TRES/VDCS), active-low BUSY, and mono KW mode. It
differs only in the TRES and PTL field widths: one byte horizontal (max
160 px) and two bytes vertical (max 296 rows), which neither the existing
8-bit nor 16-bit pair can express.

Adds the two packed layouts, the two accessors, a quirks struct reusing
uc8176_set_cdi verbatim, and the binding."
```

Expected: commit succeeds. The driver is not yet exercised — Task 2 instantiates it.

---

### Task 2: MagTag devicetree and a C-level bring-up smoke

Wires the panel and proves it paints **before any VM work exists**, which is the mitigation D2/Risk 2 asks for: confirm BUSY toggles at all before trusting a pixel. Deliverable: a boot-time C routine draws a filled rectangle on the physical panel.

**Files:**
- Modify: `~/workspace/tuvm/zephyr/boards/esp32s2_saola_esp32s2.overlay`
- Modify: `~/workspace/tuvm/zephyr/prj_esp32s2.conf`
- Modify: `~/workspace/tuvm/zephyr/src/main.c`

**Interfaces:**
- Consumes: the `ultrachip,il0373` compatible from Task 1.
- Produces: `DT_CHOSEN(zephyr_display)` resolving to the panel; console lines `DISPLAY ready`, `DISPLAY smoke rc=<n>`.

- [ ] **Step 1: Add the SPI pinctrl group to the overlay**

Append to the existing `&pinctrl { … }` block in `zephyr/boards/esp32s2_saola_esp32s2.overlay` (the block that already holds `ledc0_default`), inside its braces:

```dts
	spim2_default: spim2_default {
		group1 {
			pinmux = <SPIM2_MISO_GPIO37>;
			input-enable;
		};
		group2 {
			pinmux = <SPIM2_SCLK_GPIO36>,
				 <SPIM2_MOSI_GPIO35>;
			output-enable;
		};
	};
```

Those three macro names are verified present at `zephyr/include/zephyr/dt-bindings/pinctrl/esp32s2-pinctrl.h:3608,3691,3778`.

- [ ] **Step 2: Add the SPI controller, MIPI-DBI shim and panel node**

Append to the same overlay file, at top level (after the `&ledc0 { … }` block):

```dts
/* tuvm#88 — the e-paper panel. Pins read off the vendor pinout diagram
 * (zephyr/boards/magtag/magtag-overview.png, DISPLAY block):
 *   EPD_BUSY 5  EPD_RESET 6  EPD_DC 7  EPD_CS 8   (all gpio0, RTC-capable)
 *   EPD_MOSI 35 EPD_SCK 36   EPD_MISO 37          (FSPI -> spi2, so gpio1)
 *
 * CS IS A PLAIN GPIO, NOT FSPICS0 (which is GPIO10 and goes to the D10 pad).
 * It is therefore driven by the SPI driver's cs-gpios, not by pinctrl.
 *
 * BUSY IS DECLARED ACTIVE_LOW because that is what the silicon does and what
 * ultrachip,uc81xx-common.yaml:20 requires ("The BUSY pin of UC81xx is active
 * low. If connected directly the MCU pin should be configured as active low").
 * uc81xx_busy_wait then spins while the LOGICAL level is 1. Note Adafruit's own
 * examples pass EPD_BUSY -1 and never read it (adafruit-magtag.pdf:5123) — the
 * pin is genuinely wired, they simply rely on fixed delays instead.
 *
 * The panel is 128 wide x 296 tall in its own frame; rotation = 270 presents it
 * as the 296x128 landscape the case shows.
 */
&spi2 {
	status = "okay";
	pinctrl-0 = <&spim2_default>;
	pinctrl-names = "default";
	#address-cells = <1>;
	#size-cells = <0>;
	cs-gpios = <&gpio0 8 GPIO_ACTIVE_LOW>;
};

/ {
	mipi_dbi_epd: mipi-dbi-epd {
		compatible = "zephyr,mipi-dbi-spi";
		spi-dev = <&spi2>;
		dc-gpios = <&gpio0 7 GPIO_ACTIVE_HIGH>;
		reset-gpios = <&gpio0 6 GPIO_ACTIVE_LOW>;
		write-only;
		#address-cells = <1>;
		#size-cells = <0>;

		epd: il0373@0 {
			compatible = "ultrachip,il0373";
			reg = <0>;
			mipi-max-frequency = <4000000>;
			width = <128>;
			height = <296>;
			rotation = <270>;
			busy-gpios = <&gpio0 5 GPIO_ACTIVE_LOW>;

			full { };
			partial { };
		};
	};

	chosen {
		zephyr,display = &epd;
	};
};
```

Empty `full { }` / `partial { }` profiles mean "use the controller's OTP reset values", which is what `ultrachip,uc81xx-common.yaml`'s child-binding documents. The `partial` node existing at all is what enables the driver's partial path; its **waveform** is still the OTP one, which is the flashing caveat D2 records.

- [ ] **Step 3: Turn the drivers on**

Append to `zephyr/prj_esp32s2.conf`:

```
# tuvm#88 — the e-paper display seam. CONFIG_UC81XX is selected automatically by
# the IL0373 devicetree node (Kconfig.uc81xx depends on DT_HAS_ULTRACHIP_IL0373_
# ENABLED), and it in turn selects MIPI_DBI.
CONFIG_SPI=y
CONFIG_DISPLAY=y
CONFIG_CHARACTER_FRAMEBUFFER=y
# CFB k_mallocs its framebuffer (subsys/fb/cfb.c:687): 296 * 128 / 8 = 4736 B.
# 8192 leaves slack for the allocator header and anything else on the system heap.
CONFIG_HEAP_MEM_POOL_SIZE=8192
```

- [ ] **Step 4: Add the bring-up smoke to `main.c`**

This deliberately uses the **raw display API, not CFB** — it tests the driver and the wiring only, so a failure here cannot be a CFB problem.

Add near the other board-specific bring-up code in `zephyr/src/main.c`:

```c
#if DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>

/* tuvm#88 bring-up smoke. Raw display API on purpose: if this paints, the
 * IL0373 quirks, the MIPI-DBI wiring and BUSY are all good, and anything that
 * fails later is CFB's or the seam's. Per Risk 2, a panel that never asserts
 * BUSY means the init never landed — check that before doubting pixels. */
static void display_smoke(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	struct display_capabilities caps;
	static uint8_t band[296];        /* 8 rows x (296/8) bytes, all black */
	struct display_buffer_descriptor desc;
	int rc;

	if (!device_is_ready(dev)) {
		printk("DISPLAY not ready\n");
		return;
	}
	display_get_capabilities(dev, &caps);
	printk("DISPLAY ready %ux%u\n", caps.x_resolution, caps.y_resolution);

	memset(band, 0x00, sizeof band);   /* MONO10: 0 bits are black */
	desc.buf_size = sizeof band;
	desc.width    = 296;
	desc.height   = 8;
	desc.pitch    = 296;

	rc = display_write(dev, 0, 0, &desc, band);
	printk("DISPLAY smoke rc=%d\n", rc);
	display_blanking_off(dev);
}
#endif
```

Call `display_smoke();` from `main()` alongside the existing board bring-up, guarded the same way.

- [ ] **Step 5: Build**

```bash
cd ~/workspace/tuvm/zephyr
./run.sh esp32s2 build 2>&1 | tail -30
```

Expected: a clean link, and in the output a devicetree line confirming the overlay was found:
`Found devicetree overlay: .../boards/esp32s2_saola_esp32s2.overlay`.
**If that line is absent the overlay was silently ignored** and nothing below will work — the filename is load-bearing (see the comment at the top of the overlay).

Record the reported `dram0` percentage from the link output; it is a deliverable of Task 7.

- [ ] **Step 6: Flash and observe**

```bash
cd ~/workspace/tuvm/zephyr
./flash-magtag.sh
```

Hold BOOT0, click RESET when prompted. Expected console:

```
DISPLAY ready 296x128
DISPLAY smoke rc=0
```

**and a visible black band across the top of the panel.** Per the global constraints, `rc=0` alone is not the gate — the band is. Photograph it.

If `DISPLAY not ready`: the driver never bound; check the devicetree overlay line from Step 5.
If `rc=0` but nothing appears: suspect BUSY. Probe GPIO5 with `gpio level:`-style logging or a meter — a pin that never moves means the controller never accepted the init (Risk 2).

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/boards/esp32s2_saola_esp32s2.overlay zephyr/prj_esp32s2.conf zephyr/src/main.c
git commit -m "feat(magtag): the e-paper panel wired and painting from C (tuvm#88)

spi2/FSPI + a zephyr,mipi-dbi-spi shim + the ultrachip,il0373 node from the
Zephyr-side patch. Pins from the vendor pinout: BUSY 5, RESET 6, DC 7, CS 8
(plain GPIO, not FSPICS0), MOSI 35 / SCK 36 / MISO 37.

Bring-up smoke uses the RAW display API, not CFB, so a failure here cannot be
a CFB problem — the risk order Risk 2 asks for.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `cfb_framebuffer_finalize_area` — CFB learns a bounded flush

D3: `cfb_framebuffer_finalize` always writes the whole framebuffer at (0,0) and `char_fb` is file-static with no public accessor, so per-row partial refresh is otherwise inexpressible. Deliverable: a public CFB call that flushes a sub-rectangle.

**Files:**
- Modify: `~/zephyr-esp32/zephyr/include/zephyr/display/cfb.h`
- Modify: `~/zephyr-esp32/zephyr/subsys/fb/cfb.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `int cfb_framebuffer_finalize_area(const struct device *dev, uint16_t x, uint16_t y, uint16_t width, uint16_t height);` — returns 0 on success, `-ENODEV` if no framebuffer, `-EINVAL` if the rectangle leaves the framebuffer. Task 5 calls it.

- [ ] **Step 1: Declare it**

In `include/zephyr/display/cfb.h`, immediately after the `cfb_framebuffer_finalize` declaration (line 191):

```c
/**
 * @brief Finalize a rectangular region of the framebuffer to the display.
 *
 * Identical to @ref cfb_framebuffer_finalize but writes only the given
 * rectangle. On displays whose driver treats a bounded write as a partial
 * update (e-paper), this is what makes a partial refresh possible: the full
 * finalize always writes the whole panel.
 *
 * @param dev Pointer to device structure
 * @param x Column of the top-left corner, in pixels
 * @param y Row of the top-left corner, in pixels
 * @param width Width of the region, in pixels
 * @param height Height of the region, in pixels
 *
 * @retval 0 on success
 * @retval -ENODEV if the framebuffer has not been initialized
 * @retval -EINVAL if the rectangle is not contained by the framebuffer
 * @retval negative errno propagated from the display driver
 */
int cfb_framebuffer_finalize_area(const struct device *dev, uint16_t x, uint16_t y,
				  uint16_t width, uint16_t height);
```

- [ ] **Step 2: Implement it**

In `subsys/fb/cfb.c`, immediately after the closing brace of `cfb_framebuffer_finalize` (line 585):

```c
int cfb_framebuffer_finalize_area(const struct device *dev, uint16_t x, uint16_t y,
				  uint16_t width, uint16_t height)
{
	const struct display_driver_api *api = dev->api;
	const struct char_framebuffer *fb = &char_fb;
	int err;

	__ASSERT_NO_MSG(DEVICE_API_IS(display, dev));

	if (!fb->buf) {
		return -ENODEV;
	}

	if ((uint32_t)x + width > fb->x_res || (uint32_t)y + height > fb->y_res) {
		return -EINVAL;
	}

	struct display_buffer_descriptor desc = {
		.buf_size = (size_t)width * height / 8U,
		.width = width,
		.height = height,
		.pitch = width,
	};

	/* The framebuffer is contiguous row-major with a pitch of x_res, so a
	 * full-width band starting on a byte boundary is contiguous. Anything
	 * narrower would need a strided copy, which no caller needs yet.
	 */
	if (width != fb->x_res) {
		return -ENOTSUP;
	}

	uint8_t *start = fb->buf + ((size_t)y * fb->x_res / 8U);

	if ((fb->pixel_format == PIXEL_FORMAT_MONO10) == fb->inverted) {
		cfb_invert(fb);
		err = api->write(dev, x, y, &desc, start);
		cfb_invert(fb);
		return err;
	}

	return api->write(dev, x, y, &desc, start);
}
```

The `width != fb->x_res` guard is deliberate and documented: a full-width band is contiguous in the buffer, a narrower one is not, and this rung only ever refreshes whole character rows. Returning `-ENOTSUP` is honest about the limit rather than silently writing the wrong bytes.

- [ ] **Step 3: Build it (via Task 2's app, which already enables CFB)**

```bash
cd ~/workspace/tuvm/zephyr
./run.sh esp32s2 build 2>&1 | tail -20
```

Expected: clean link. Nothing calls the new function yet, so this only proves it compiles.

- [ ] **Step 4: Commit — separately from Task 1**

D10 requires the subsystem change to be its own commit, because it would be its own upstream PR.

```bash
cd ~/zephyr-esp32/zephyr
git add include/zephyr/display/cfb.h subsys/fb/cfb.c
git commit -m "subsys: fb: cfb: add cfb_framebuffer_finalize_area

cfb_framebuffer_finalize always writes the whole framebuffer at (0,0), and
char_fb is file-static with no public accessor, so a caller cannot flush a
sub-rectangle. On e-paper, where a bounded display_write is what the driver
turns into a partial refresh, that makes partial updates impossible through
CFB at all.

Restricted to full-width bands: the framebuffer is contiguous row-major, so
a band starting on a row boundary needs no strided copy. Narrower rectangles
answer -ENOTSUP rather than writing the wrong bytes."
```

---

### Task 4: Prims 125-128, the host grid, and the raw kernel forwarders

The seam's host half, end to end, testable without hardware. Deliverable: an nsl test that writes text and sees it rendered.

**Files:**
- Modify: `~/workspace/tuvm/src/prim_ids.h` (append before `#endif`)
- Modify: `~/workspace/tuvm/src/dispatch.c` (enum ~line 770, `PRIM_NAMES` ~line 826, parity guard line 837)
- Modify: `~/workspace/tuvm/src/platform.c`
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl`
- Create: `~/workspace/nsl-tests/14_display/01_display_grid.nsl`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - C: `int display_target_available(void)` → 1 if a display exists, 0 if not. `int display_target_clear(void)`, `int display_target_put(int row, int col, const char *s, int n)`, `int display_target_refresh(void)`, `int display_target_refresh_row(int row)` — each returning **0 = ok, 1 = bounds, 2 = driver fault**.
  - Prims: `displayClear`, `displayAt:col:put:`, `displayRefresh`, `displayRefreshRow:`. Each answers **nil** when no display exists, otherwise the status SMI above.
  - nsl: kernel forwarders `displayClear`, `displayAt:col:put:`, `displayRefresh`, `displayRefreshRow:`.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/14_display/01_display_grid.nsl`:

```
(*
expect:
clear 0
put 0
oob 1
display:
  0: TUVM
  2: BELL 4
refresh 0
display row 2: BELL 4
row 0
finished
*)
(* ORDERING IS NOT A TYPO. The grid is printed by the C leaf DURING the prim
   call, so it lands before the `refresh 0` that reports the call's result. Any
   expect: block that puts `refresh 0` first is asserting an order the code
   cannot produce. *)
(* tuvm#88 — the host half of the display seam, using the RAW kernel forwarders.
   The GpioPulse-style wrapper that turns these status codes into tagged raises is
   the next task, and this test must keep passing after it lands (the same shape
   13_pulse/02_gpio_set_level.nsl uses).

   The host stub RENDERS: refresh prints the non-blank rows, so this asserts on
   the text that was actually laid into the grid rather than merely on a status
   code. That is the host-divergence hole tuvm#82 left, closed. *)
class DisplayGrid packageUsing: manifest = () (
  public main: platform args: args = (
    ('clear ' , (platform displayClear) printString) out.
    ('put ' , (platform displayAt: 0 col: 0 put: 'TUVM') printString) out.
    platform displayAt: 2 col: 0 put: 'BELL 4'.
    ('oob ' , (platform displayAt: 99 col: 0 put: 'X') printString) out.
    ('refresh ' , (platform displayRefresh) printString) out.
    ('row ' , (platform displayRefreshRow: 2) printString) out.
    'finished' out.
  )
) : (
)
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/workspace/nsl-tests && ~/workspace/nsl-tuvm/tools/run-one.py 14_display/01_display_grid.nsl
```

Expected: FAIL — an unknown-selector error for `displayClear`. **A SKIP is not a pass**: if it skips, the file is not being compiled and the test proves nothing.

- [ ] **Step 3: Reserve the prim ids by counting**

```bash
cd ~/workspace/tuvm
python3 - <<'PY'
import re
src = open('src/dispatch.c').read()
body = src.split('static const char *const PRIM_NAMES[] = {',1)[1].split('};',1)[0]
print("PRIM_NAMES entries:", len(re.findall(r'"(?:[^"\\]|\\.)*"', body)))
PY
```

Expected: `124`. The four new prims are therefore **125, 126, 127, 128**. If it prints anything else, use *that* number + 1..4 — the ids are counted, never computed from the previous constant.

- [ ] **Step 4: Append to `src/prim_ids.h`**

Immediately before the closing `#endif /* OEVM_PRIM_IDS_H */`:

```c
/* tuvm#88 (MagTag e-paper): the display seam. clear/at:col:put: touch only the
   C-side character framebuffer and are cheap; refresh/refreshRow: drive the panel
   and BLOCK the VM for the paint — safe because the pulse mailbox sheds and
   coalesces rather than aborting (src/backend/zephyr/pulse_zephyr.c, tuvm#34), so
   a button pressed mid-paint is delivered once afterwards.
   Appended after gpioLevel; no pre-existing id shifts. 125-128 were arrived at by
   COUNTING PRIM_NAMES (124 entries before this append), not by 124+1 arithmetic,
   per the standing rule above.
   ANSWER CONVENTION, an extension of the gpio pair's: nil still means "this board
   has no such thing" (no display bound at all) and is the structural error a wrong
   literal produces. An operational outcome the driver knows about answers an SMI
   instead — 0 ok, 1 out of bounds, 2 driver fault — because one nil cannot carry
   three different tags. The raises are written in nsl (Display in kernel.nsl), not
   here, the same division Vector>>at: uses over basicAt:. */
#define PRIM_pf_displayClear      125
#define PRIM_pf_displayAt         126
#define PRIM_pf_displayRefresh    127
#define PRIM_pf_displayRefreshRow 128
```

- [ ] **Step 5: Append to the `dispatch.c` enum, names table and parity guard**

After `PRIM_pf_gpioLevel,` in the enum:

```c
    /* tuvm#88: the display seam — displayClear / displayAt:col:put: /
       displayRefresh / displayRefreshRow:. Appended after gpioLevel; no
       pre-existing id shifts. Counted (not computed) to be the 125th-128th
       PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_displayClear,
    PRIM_pf_displayAt,
    PRIM_pf_displayRefresh,
    PRIM_pf_displayRefreshRow,
```

After `"gpioSet:to:", "gpioLevel:",` in `PRIM_NAMES`:

```c
    "displayClear", "displayAt:col:put:", "displayRefresh", "displayRefreshRow:",
```

And advance the guard on line 837:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_displayRefreshRow) ? 1 : -1];
```

- [ ] **Step 6: Add the host grid stub to `src/platform.c`**

Insert after the `#endif` that closes the `#ifdef __ZEPHYR__` gpio block (i.e. after the `extern int gpio_target_level(int pin);` / `#endif` pair around line 124):

```c
/* tuvm#88: the host side of the display seam — a real character grid, not a
   stub that only reports errors. `refresh` PRINTS the non-blank rows, so an nsl
   test asserts on the text that was actually laid down (the existing expect:
   mechanism is the read-back path, which is why no fifth "read the grid" prim
   has to be spent). On target these live in zephyr/src/main.c beside CFB.
   Geometry matches the MagTag at an 8x8 font: 296/8 = 37 columns, 128/8 = 16
   rows. A host/device mismatch here would make a host-green test meaningless,
   so the numbers are named once and asserted against the device at bring-up. */
#ifndef __ZEPHYR__
#define HOST_DISPLAY_ROWS 16
#define HOST_DISPLAY_COLS 37
static char host_display[HOST_DISPLAY_ROWS][HOST_DISPLAY_COLS + 1];
static int host_display_init_done = 0;

static void host_display_reset(void) {
    int r, c;
    for (r = 0; r < HOST_DISPLAY_ROWS; r++) {
        for (c = 0; c < HOST_DISPLAY_COLS; c++) host_display[r][c] = ' ';
        host_display[r][HOST_DISPLAY_COLS] = '\0';
    }
    host_display_init_done = 1;
}
static void host_display_ensure(void) {
    if (!host_display_init_done) host_display_reset();
}
/* Output goes through platform_console_write, NOT printf. The `out` selector
   uses it (see PRIM_pf_print above), and mixing the two would interleave two
   different buffering regimes — an expect: block would then be asserting an
   order that is an artefact of stdio, not of the program. */
static void host_display_emit(const char *s, int n) {
    platform_console_write(s, (size_t)n);
}
/* Trailing blanks carry no information and would make every expect: block
   whitespace-fragile, so they are trimmed on the way out. */
static int host_display_row_len(int r) {
    int end = HOST_DISPLAY_COLS;
    while (end > 0 && host_display[r][end - 1] == ' ') end--;
    return end;
}

int display_target_available(void) { host_display_ensure(); return 1; }

int display_target_clear(void) { host_display_reset(); return 0; }

int display_target_put(int row, int col, const char *s, int n) {
    int i;
    host_display_ensure();
    if (row < 0 || row >= HOST_DISPLAY_ROWS) return 1;
    if (col < 0 || n < 0 || col + n > HOST_DISPLAY_COLS) return 1;
    for (i = 0; i < n; i++) host_display[row][col + i] = s[i];
    return 0;
}

int display_target_refresh(void) {
    char line[80];
    int r, len, end;
    host_display_ensure();
    host_display_emit("display:\n", 9);
    for (r = 0; r < HOST_DISPLAY_ROWS; r++) {
        end = host_display_row_len(r);
        if (end == 0) continue;
        len = snprintf(line, sizeof line, "  %d: %.*s\n", r, end, host_display[r]);
        if (len > 0) host_display_emit(line, len);
    }
    return 0;
}

int display_target_refresh_row(int row) {
    char line[80];
    int len, end;
    host_display_ensure();
    if (row < 0 || row >= HOST_DISPLAY_ROWS) return 1;
    end = host_display_row_len(row);
    len = snprintf(line, sizeof line, "display row %d: %.*s\n", row, end, host_display[row]);
    if (len > 0) host_display_emit(line, len);
    return 0;
}
#else
/* On target these live in zephyr/src/main.c, beside the CFB calls and the
   devicetree node they read — the same split gpio_target_set/level use, and for
   the same reason: a new .c file costs edits to nine enumerated source lists
   across two repos (tuvm#63). */
extern int display_target_available(void);
extern int display_target_clear(void);
extern int display_target_put(int row, int col, const char *s, int n);
extern int display_target_refresh(void);
extern int display_target_refresh_row(int row);
#endif
```

- [ ] **Step 7: Add the four dispatch cases**

In `platform_dispatch`, after the `PRIM_pf_gpioLevel` case:

```c
    case PRIM_pf_displayClear: {
        /* tuvm#88. nil = no display on this board (structural); otherwise an SMI
           status the nsl wrapper turns into a tag. See prim_ids.h for why the
           convention differs from the gpio pair's plain nil. */
        if (!display_target_available()) { *out = NULL; return 1; }
        *out = TAG_SMI(display_target_clear());
        return 1;
    }
    case PRIM_pf_displayAt: {          /* displayAt: row col: col put: aString */
        oop data = args[2];
        int rc;
        if (data == NULL || IS_SMI(data) || StringVtable == NULL ||
            obj_vtable(data) != StringVtable) {
            fprintf(stderr, "VM Error: platform displayAt:col:put: expects a String\n");
            abort();
        }
        if (!display_target_available()) { *out = NULL; return 1; }
        {
            struct String *s = (struct String *)data;
            rc = display_target_put((int)UNTAG_SMI(args[0]), (int)UNTAG_SMI(args[1]),
                                    (const char *)s->bytes, (int)s->length);
        }
        *out = TAG_SMI(rc);
        return 1;
    }
    case PRIM_pf_displayRefresh: {
        if (!display_target_available()) { *out = NULL; return 1; }
        *out = TAG_SMI(display_target_refresh());
        return 1;
    }
    case PRIM_pf_displayRefreshRow: {
        if (!display_target_available()) { *out = NULL; return 1; }
        *out = TAG_SMI(display_target_refresh_row((int)UNTAG_SMI(args[0])));
        return 1;
    }
```

- [ ] **Step 8: Add the raw kernel forwarders**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, after the `gpioLevel:` forwarder (line 2345):

```
  (* tuvm#88: the display seam. Thin forwarders, same shape as the gpio family
     above. All four answer nil when the board has no display and an SMI status
     otherwise — the Display wrapper is what turns those into tagged raises, so
     these stay raw and a test can observe the codes directly. *)
  public displayClear = ( ^ platform displayClear )
  public displayAt: r col: c put: s = ( ^ platform displayAt: r col: c put: s )
  public displayRefresh = ( ^ platform displayRefresh )
  public displayRefreshRow: r = ( ^ platform displayRefreshRow: r )
```

- [ ] **Step 9: Run the test to verify it passes**

```bash
cd ~/workspace/nsl-tests && ~/workspace/nsl-tuvm/tools/run-one.py 14_display/01_display_grid.nsl
```

Expected: PASS, matching the `expect:` block exactly. If the parity guard fires instead (`negative-sized array`), the `PRIM_NAMES` count and the enum have drifted — recount at Step 3, do not adjust by arithmetic.

- [ ] **Step 10: Commit**

```bash
cd ~/workspace/tuvm
git add src/prim_ids.h src/dispatch.c src/platform.c
git commit -m "feat(display): prims 125-128 and a rendering host grid (tuvm#88)

Ids arrived at by COUNTING PRIM_NAMES (124 entries before this append), per the
standing rule in prim_ids.h; the parity guard advanced to
PRIM_pf_displayRefreshRow so a miscount is a build error.

Answer convention extends the gpio pair's rather than repeating it: nil still
means 'this board has no such thing', but an operational outcome answers an SMI
(0 ok / 1 bounds / 2 fault) because one nil cannot carry three tags.

The host side is a REAL grid that prints on refresh, so tests assert on rendered
text rather than status codes alone — closing the host-divergence hole tuvm#82
left, where set:to: could not raise on the host at all.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(kernel): display seam forwarders (tuvm#88)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/nsl-tests && git add 14_display/01_display_grid.nsl && git commit -m "test(display): the host grid renders (tuvm#88)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: The `Display` class and its three tagged raises

D5. Deliverable: nsl code calls `display refresh` and gets a tagged exception, not a status code.

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl`
- Create: `~/workspace/nsl-tests/14_display/02_display_raises.nsl`

**Interfaces:**
- Consumes: the four kernel forwarders from Task 4.
- Produces: `platform display` answering a `Display` with `clear`, `at:col:put:`, `refresh`, `refreshRow:`, each raising `#displayUnavailable` / `#displayBounds` / `#displayFault` or answering self.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/14_display/02_display_raises.nsl`:

```
(*
expect:
display:
  1: HELLO
caught displayBounds
text row or column outside the grid
finished
*)
(* tuvm#88 — an out-of-grid row is a WRONG LITERAL, not a load condition, so it
   raises rather than silently doing nothing and leaving a node that looks fine
   and displays nothing. The tag is asserted, not merely the fact that something
   failed — an untagged catch would pass even if the raise came from elsewhere.
   Same shape as 13_pulse/03_gpio_undeclared_raises.nsl. *)
class DisplayRaises packageUsing: manifest = () (
  public main: platform args: args = (
    | display |
    display:: platform display.
    display clear.
    display at: 1 col: 0 put: 'HELLO'.
    display refresh.

    [ display at: 99 col: 0 put: 'X' ]
      on: [ :e | e tag = #displayBounds ]
      do: [ :e | 'caught displayBounds' out.
                 ('text ' , (e at: #text ifAbsent: [ '?' ])) out ].
    'finished' out.
  )
) : (
)
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/workspace/nsl-tests && ~/workspace/nsl-tuvm/tools/run-one.py 14_display/02_display_raises.nsl
```

Expected: FAIL — `platform display` is not a selector yet.

- [ ] **Step 3: Add the `Display` class**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, immediately after the `GpioPulse` class (which ends at line 2112):

```
  (* tuvm#88: the display seam's nsl half. The C leaves stay raw — nil for a board
     with no display, an SMI status otherwise — and the RAISE lives here, the same
     division Vector>>at: uses over basicAt: and the only one available since
     build_and_raise is static to dispatch.c.
     Three tags rather than one because the three conditions want different
     responses: unavailable is the wrong board, bounds is a wrong literal, and
     fault is the panel or its wiring. A node probing an unknown board can catch
     #displayUnavailable alone and carry on headless. *)
  class Display = ( | seam | ) (
    public clear = ( ^ self check: (seam displayClear) )
    public at: r col: c put: s = ( ^ self check: (seam displayAt: r col: c put: s) )
    public refresh = ( ^ self check: (seam displayRefresh) )
    public refreshRow: r = ( ^ self check: (seam displayRefreshRow: r) )
    check: rc = (
      rc == nil ifTrue: [ ^ self error: 'no display on this board' tag: #displayUnavailable ].
      rc = 1 ifTrue: [ ^ self error: 'row or column outside the grid' tag: #displayBounds ].
      rc = 2 ifTrue: [ ^ self error: 'display driver fault' tag: #displayFault ].
      ^ self )
    setSeam: c = ( seam:: c. ^ self )
  ) : (
    public usingSeam: c = ( ^ self new setSeam: c )
  )
```

The class-side idiom is `^ self new setSeam: c`, exactly as `GpioPulse` does at
`kernel.nsl:2110-2112`. **Never `super new` class-side** — it is one of the standing
nsl grammar traps and fails at compile time with an unhelpful message.

- [ ] **Step 4: Add the accessor**

After the `public gpio` accessor at line 2471:

```
  public display           = ( ^ Display usingSeam: self )
```

- [ ] **Step 5: Run both display tests to verify they pass**

```bash
cd ~/workspace/nsl-tests
~/workspace/nsl-tuvm/tools/run-one.py 14_display/01_display_grid.nsl
~/workspace/nsl-tuvm/tools/run-one.py 14_display/02_display_raises.nsl
```

Expected: both PASS. Test 01 must still pass — it uses the raw forwarders and is the regression guard that adding the wrapper changed nothing underneath.

- [ ] **Step 6: Run the full suite**

```bash
cd ~/workspace/nsl-tests && python3 ~/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -20
```

Expected: no failures, and **read the gate line**, not just `0 failed` (tuvm#63). Record the new total; it re-baselines the count the design doc flagged as stale.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(kernel): Display and its three tagged raises (tuvm#88)

Three tags rather than one because the conditions want different responses:
unavailable is the wrong board, bounds is a wrong literal, fault is the panel.
A node probing an unknown board can catch #displayUnavailable and go headless.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd ~/workspace/nsl-tests && git add 14_display/02_display_raises.nsl && git commit -m "test(display): the bounds raise is tagged (tuvm#88)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: The device seam — CFB behind the same four functions

Deliverable: the same four `display_target_*` functions, implemented on the MagTag over CFB, so the nsl seam drives the physical panel.

**Files:**
- Modify: `~/workspace/tuvm/zephyr/src/main.c`

**Interfaces:**
- Consumes: `cfb_framebuffer_finalize_area` (Task 3); the panel node (Task 2); the `extern` declarations in `platform.c` (Task 4).
- Produces: the device implementations of the five functions declared `extern` in Task 4, Step 6.

- [ ] **Step 1: Replace the bring-up smoke with the CFB-backed seam**

In `zephyr/src/main.c`, replace the `display_smoke()` block from Task 2 with:

```c
#if DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>

/* tuvm#88: the device half of the display seam. The host half is in
 * src/platform.c under #ifndef __ZEPHYR__; both sides implement exactly these
 * five functions, and the geometry must agree or a host-green test says nothing
 * about the board — which is why the row/col counts are printed at boot for
 * comparison against HOST_DISPLAY_ROWS/COLS.
 *
 * refresh/refreshRow BLOCK for the whole paint: uc81xx_busy_wait polls BUSY with
 * k_sleep(1ms) in the calling thread. That is safe because the pulse mailbox
 * sheds and coalesces (pulse_zephyr.c, tuvm#34), so a button pressed mid-paint
 * arrives once afterwards. The async driver-job alternative is the clock rung's
 * problem, per design D6.
 */
static const struct device *disp_dev;
static int disp_rows, disp_cols, disp_font_h;
static bool disp_ok;

static void display_bringup(void)
{
	uint8_t fw, fh;

	disp_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp_dev)) {
		printk("DISPLAY not ready\n");
		return;
	}
	if (cfb_framebuffer_init(disp_dev)) {
		printk("DISPLAY cfb init failed\n");
		return;
	}
	if (cfb_get_font_size(disp_dev, 0, &fw, &fh)) {
		printk("DISPLAY no font\n");
		return;
	}
	cfb_framebuffer_set_font(disp_dev, 0);
	cfb_framebuffer_clear(disp_dev, true);

	disp_rows   = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_ROWS);
	disp_cols   = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_COLS);
	disp_font_h = fh;
	disp_ok     = true;
	printk("DISPLAY OK rows=%d cols=%d font=%ux%u\n", disp_rows, disp_cols, fw, fh);
}

int display_target_available(void) { return disp_ok ? 1 : 0; }

int display_target_clear(void)
{
	return cfb_framebuffer_clear(disp_dev, false) ? 2 : 0;
}

int display_target_put(int row, int col, const char *s, int n)
{
	char buf[64];
	uint8_t fw, fh;

	if (row < 0 || row >= disp_rows) return 1;
	if (col < 0 || n < 0 || col + n > disp_cols) return 1;
	if (n >= (int)sizeof buf) return 1;

	/* cfb_print takes a NUL-terminated C string and PIXEL coordinates; an nsl
	 * String is counted and not terminated, so it is copied rather than cast. */
	memcpy(buf, s, n);
	buf[n] = '\0';

	if (cfb_get_font_size(disp_dev, 0, &fw, &fh)) return 2;
	return cfb_print(disp_dev, buf, col * fw, row * fh) ? 2 : 0;
}

int display_target_refresh(void)
{
	return cfb_framebuffer_finalize(disp_dev) ? 2 : 0;
}

int display_target_refresh_row(int row)
{
	int width = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_WIDTH);

	if (row < 0 || row >= disp_rows) return 1;

	/* Full-width band on a character-row boundary — the only shape
	 * cfb_framebuffer_finalize_area supports, and the only one this seam needs. */
	return cfb_framebuffer_finalize_area(disp_dev, 0, row * disp_font_h,
					     width, disp_font_h) ? 2 : 0;
}
#endif
```

Call `display_bringup();` from `main()` where `display_smoke();` was.

- [ ] **Step 2: Build**

```bash
cd ~/workspace/tuvm/zephyr && ./run.sh esp32s2 build 2>&1 | tail -30
```

Expected: clean link. **Record the `dram0` percentage** — this is the measurement the design demands instead of an assumption (#53 discipline).

- [ ] **Step 3: Flash and check the geometry agrees with the host**

```bash
cd ~/workspace/tuvm/zephyr && ./flash-magtag.sh
```

Expected console: `DISPLAY OK rows=16 cols=37 font=8x8`.

**If rows/cols differ from `HOST_DISPLAY_ROWS` (16) / `HOST_DISPLAY_COLS` (37)**, the host tests are asserting against a grid the board does not have. Fix the host constants in `src/platform.c` to match the board and re-run Task 4's tests — the device is the authority.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/src/main.c
git commit -m "feat(magtag): the device display seam over CFB (tuvm#88)

Same five display_target_* functions as the host grid in platform.c, over CFB's
character framebuffer. refreshRow: uses cfb_framebuffer_finalize_area — a full
width band on a character-row boundary, which the driver turns into a partial
window write.

Geometry is printed at boot so a mismatch against HOST_DISPLAY_ROWS/COLS is
visible rather than silently making the host tests meaningless.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: The hardware gates

Deliverable: the three device gates of the design's Verification section, with photographs.

**Files:**
- Create: `~/workspace/tuvm/zephyr/MagTagDisplayNode.nsl`

**Interfaces:**
- Consumes: everything above.
- Produces: the gate evidence for the tuvm#88 close-out.

- [ ] **Step 1: Write the gate node**

Create `~/workspace/tuvm/zephyr/MagTagDisplayNode.nsl`:

```
(* tuvm#88 gate node. Gate 1: a full screen of text paints. Gate 2: BUTTON_A
   bumps a counter and refreshes ONLY its row — the gate that actually proves
   partial refresh, since a full refresh would repaint everything and look
   identical. Gate 3: a deliberately out-of-grid write raises with its tag
   rather than doing nothing.

   Buttons are the proven seam from tuvm#82/#84: register the press pin, read
   held state with `gpio level:` if needed. *)
class MagTagDisplayNode packageUsing: manifest = () (
  public main: platform args: args = (
    | space display gpio press count |
    space:: platform space.
    display:: platform display.
    gpio:: platform gpio.
    count:: 0.

    display clear.
    display at: 0 col: 0 put: 'TUVM on the MagTag'.
    display at: 2 col: 0 put: 'e-paper seam, tuvm#88'.
    display at: 4 col: 0 put: 'presses: 0'.
    display refresh.
    'gate1 painted' out.

    [ display at: 99 col: 0 put: 'X' ]
      on: [ :e | e tag = #displayBounds ]
      do: [ :e | 'gate3 caught displayBounds' out ].

    press:: space channel: 'btn/press'.
    space on: press do: [ :m |
      count:: count + 1.
      display at: 4 col: 0 put: 'presses: ' , count printString.
      display refreshRow: 4.
      ('gate2 row 4 -> ' , count printString) out ].
    gpio pin: 15 edge: #falling post: #down on: press.

    'ready' out.
    [ true ] whileTrue: [ platform pumpEvents ].
  )
) : (
)
```

Before flashing, syntax-check it from a scratch directory (a node file lives outside the test tree, so the suite will not catch a typo):

```bash
cd /tmp/claude-1000/-home-david-workspace-tuvm/82e761b7-69ae-49a4-8a88-509ec64b3dc1/scratchpad \
  && ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/tuvm/zephyr/MagTagDisplayNode.nsl
```

- [ ] **Step 2: Build and flash the node**

```bash
cd ~/workspace/tuvm/zephyr && ./run.sh esp32s2 build && ./flash-magtag.sh
```

- [ ] **Step 3: Gate 1 — full refresh**

Expected console: `DISPLAY OK rows=16 cols=37 font=8x8`, then `gate1 painted`, `gate3 caught displayBounds`, `ready`.
Expected panel: three lines of text, legible.
**Photograph it.** `rc=0` is not the gate.

- [ ] **Step 4: Gate 2 — partial refresh**

Press BUTTON_A three times. Expected console: `gate2 row 4 -> 1`, `-> 2`, `-> 3`.
Expected panel: the `presses:` line counts up **and the other two lines do not redraw**.

Judgement call, recorded either way: if the row's window visibly flashes white-black-white on each update, that is the OTP-waveform behaviour D2 predicted. **The gate is the pixels, not the absence of flash.** If it flashes, gate 2 still passes and Task 8 opens the LUT follow-up.

If the *whole panel* redraws, `cfb_framebuffer_finalize_area` is writing the full framebuffer — check its descriptor and the `width != fb->x_res` guard.

- [ ] **Step 5: Gate 3 — the fault path**

Already covered by `gate3 caught displayBounds` in Step 3. Confirm the line appeared; its absence means the raise did not fire and D5 is unimplemented.

- [ ] **Step 6: Record the measurements**

```bash
cd ~/workspace/tuvm/zephyr && ./run.sh esp32s2 build 2>&1 | grep -i "dram0\|Used"
~/workspace/tuvm/tools/heap-ceiling.sh    # before quoting any OEVM_HEAP_SIZE
```

Record dram0 percentage and absolute bytes. `OEVM_HEAP_SIZE` remains provisional unless `heap-ceiling.sh` was actually run.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git add zephyr/MagTagDisplayNode.nsl
git commit -m "feat(magtag): the display gate node — full and partial refresh (tuvm#88)

Gate 2 refreshes ONLY row 4 on each press: a full refresh would repaint
everything and look identical, so the partial path is the only thing the gate
can be measuring.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Close-out and promotion

The rung is not closed until its durable knowledge reaches a mainline document of record (CLAUDE.md's promotion rule). Deliverable: the ladder, CLAIMS.md and the driver-exposure doc all updated, and tuvm#88 closed with the deviations named.

**Files:**
- Modify: `~/workspace/tuvm/review/program-sequence.md`
- Modify: `~/workspace/tuvm/review/tests/CLAIMS.md`
- Modify: `~/workspace/tuvm/review/zephyr-driver-exposure.md`

- [ ] **Step 1: Add the rung to the ladder**

Append a section to `review/program-sequence.md`, after the speaker rung, covering:
- **Both MagTag panel revisions** and which one this was proven on — the single most reusable fact, since a post-July-2025 board needs `solomon,ssd1680` instead and none of the IL0373 work applies.
- The IL0373-is-a-UC81xx finding and the three-part extension.
- That CFB could not express a partial refresh and grew `finalize_area`.
- The measured dram0 and grid geometry.
- Whether partial flashed (the OTP-waveform question) and what was decided.

- [ ] **Step 2: Add the CLAIMS.md row**

A row for the tagged raises — a behavioural guarantee, exactly as tuvm#82's was. **No performance row**: nothing in this rung is a performance claim.

- [ ] **Step 3: Update the driver-exposure doc where this rung revised it**

In `review/zephyr-driver-exposure.md`, record the three revisions the design named:
- CFB chosen over the ~1 KB glyph blitter the Tier-2 sketch hedged on (D3) — and that CFB needed a subsystem patch to do partial;
- the C-side driver *job* deferred in favour of a blocking binding (D6), with the clock rung named as what will force it;
- the catalog claim recorded as **extended** (a new family member) rather than merely consumed — and the Tier-1 TFT claim (D7) left explicitly open.

- [ ] **Step 4: Close the issue with the deviations named**

```bash
cd ~/workspace/tuvm
gh issue comment 88 --body "<close-out: what shipped, what deviated from the design, the measurements, and any follow-up issues filed>"
```

Follow-up issues to file if they arose:
- the no-flash partial LUT, if the OTP waveform flashed (D2's timebox expiring is a **finding**, not a failure);
- upstreaming either Zephyr commit (D10) — a decision, not an obligation;
- the async driver job (D6), if the blocking refresh proved annoying in practice.

- [ ] **Step 5: Commit**

```bash
git add review/program-sequence.md review/tests/CLAIMS.md review/zephyr-driver-exposure.md
git commit -m "docs: promote the MagTag e-paper rung (tuvm#88)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Notes for the implementer

**Do the tasks in order.** Task 2 deliberately paints from raw C *before* any VM code exists, so that a failure is unambiguously the driver, the devicetree or BUSY — not the seam. Skipping ahead throws that bisect away and is the single most likely way to lose a day here.

**`review/` is David's review-agent tree** — Task 8 edits specific files there by name. Never sweep, move or delete anything else in it.

**Two things that look like bugs and are not:**
- A `gpio level:` read on an *output* pin answers `false` regardless of what was driven — the esp32 driver does not leave the input path enabled on a `GPIO_OUTPUT` pin (tuvm#82 rung, point 5). Do not use it as a fault detector.
- The panel may hold its last image with the board powered off. That is e-paper working correctly, not a stale flash.
