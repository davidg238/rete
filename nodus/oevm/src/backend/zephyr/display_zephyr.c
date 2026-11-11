/* The display leaves of the board (rete#440 R6a): the character-grid seam over Zephyr's
   CFB. Moved verbatim from zephyr/src/main.c; the behaviour is unchanged. */
#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "target_backend.h"

#include "platform_backend.h"   /* OEVM_DISPLAY_ROWS/COLS — the one place the grid is named (tuvm#88) */

#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <string.h>   /* memcpy in display_target_put — not pulled in unconditionally
                       * elsewhere in this TU (same finding the smoke code this
                       * replaces already made for memset). */
#endif

/* tuvm#88: the device half of the display seam. The host half is in
 * src/platform.c under #ifndef __ZEPHYR__; both sides implement exactly these
 * five functions, and the geometry must agree or a host-green test says nothing
 * about the board — which is why disp_rows/disp_cols are hard-checked against
 * OEVM_DISPLAY_ROWS/COLS (platform_backend.h) at bring-up below, not merely
 * printed for a human to eyeball.
 *
 * Finding 3: these five symbols must exist and link on EVERY Zephyr board,
 * including ones with no display node at all (ESP32, C6, nRF52840-DK/dongle) —
 * src/platform.c declares them `extern` unconditionally under __ZEPHYR__ and the
 * prim dispatch in src/platform.c calls them, guarded only by
 * display_target_available() (every dispatch site checks that first — see the
 * PRIM_pf_display* cases). So the #if DT_HAS_CHOSEN(zephyr_display) guard goes
 * INSIDE each function body (copying the gpio_target_set/level pattern just
 * above), never around the definitions themselves. On a no-display board,
 * available() returns 0 and the rest are unreachable in practice but still
 * return 2 (driver fault) rather than silently claiming success.
 *
 * refresh/refreshRow BLOCK for the whole paint: uc81xx_busy_wait polls BUSY with
 * k_sleep(1ms) in the calling thread. That is safe because the pulse mailbox
 * sheds and coalesces (pulse_zephyr.c, tuvm#34), so a button pressed mid-paint
 * arrives once afterwards. The async driver-job alternative is the clock rung's
 * problem, per design D6.
 */
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
static const struct device *disp_dev;
static int disp_rows, disp_cols, disp_font_h;

/* tuvm#88 gate-2 finding: CFB's character drawing ORs, it does not overwrite.
 *
 * draw_char_htmono (cfb.c:262) only ever sets bits — `fb->buf[i] |= BIT(...)`
 * — and never clears the glyph cell first. Its `draw_bg` parameter is accepted
 * and then ignored entirely; only the VTILED path honours it (cfb.c:196), and
 * this panel is not VTILED (uc81xx reports MONO_MSB_FIRST | EPD). Separately,
 * draw_text passes its `wrap` argument INTO that draw_bg slot (cfb.c:356-359),
 * conflating two unrelated concepts: cfb_print supplies true and cfb_draw_text
 * supplies false, so on a VTILED panel whether the background is cleared is
 * decided by which spelling of "draw a string" you called. That conflation does
 * not affect THIS panel — htmono ignores the argument either way — so it is a
 * second, independent defect, not the cause here.
 *
 * Consequence: printing '1' where '0' already stands leaves the union of both
 * glyphs. Observed on the bench as a counter rendering 0,1,2,3 stacked in one
 * cell — the framebuffer, in RAM, before any panel command; a driver-side
 * theory about stale controller banks was tested and correctly refuted.
 *
 * The host seam does not have this problem: platform.c keeps a char grid and
 * display_target_put REPLACES characters in it. So the divergence was between
 * the two seams, and a host-green test could never have caught it. The fix
 * removes the divergence rather than papering over it — the device keeps the
 * same shadow grid, and CFB is demoted to a pure renderer that always draws
 * onto a freshly cleared framebuffer. Public CFB API only, no patch to a
 * shared subsystem.
 *
 * CFB's OR-only htmono drawing is worth reporting upstream on its own (it is
 * why every caller must own a shadow), but that is a separate contribution
 * from this rung's cfb_framebuffer_finalize_area.
 */
static char disp_grid[OEVM_DISPLAY_ROWS][OEVM_DISPLAY_COLS];

static void disp_grid_reset(void)
{
	memset(disp_grid, ' ', sizeof disp_grid);
}

static int disp_render(void)
{
	char line[OEVM_DISPLAY_COLS + 1];
	uint8_t fw, fh;

	if (cfb_get_font_size(disp_dev, 0, &fw, &fh)) return 2;
	if (cfb_framebuffer_clear(disp_dev, false)) return 2;

	for (int r = 0; r < OEVM_DISPLAY_ROWS; r++) {
		/* cfb_print takes a NUL-terminated C string and PIXEL
		 * coordinates; the shadow row is counted, so it is copied. */
		memcpy(line, disp_grid[r], OEVM_DISPLAY_COLS);
		line[OEVM_DISPLAY_COLS] = '\0';
		if (cfb_print(disp_dev, line, 0, r * fh)) return 2;
	}
	return 0;
}
#endif
static bool disp_ok;

#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
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

	/* tuvm#88 gate-1 finding: without this the panel paints WHITE ON BLACK.
	 *
	 * uc81xx advertises PIXEL_FORMAT_MONO10 ("1 = Black", display.h:45) at
	 * uc81xx.c:478-479, but its controller RAM convention is the opposite —
	 * uc81xx_controller_init clears BOTH RAM banks to 0xff to reach a WHITE
	 * panel (uc81xx.c:537), which is only coherent if 1 = White. CFB inverts
	 * only when `(pixel_format == MONO10) == inverted` (cfb.c:578, and the
	 * same rule in the cfb_framebuffer_finalize_area this rung added), so
	 * with MONO10 and inverted == false it writes the buffer through
	 * untouched: CFB's glyph bits (1) land as white and the cleared field
	 * (0) as black. Flipping the flag restores black-on-white for both the
	 * full and the partial path.
	 *
	 * cfb_framebuffer_invert only toggles a flag — no I/O — so it is safe
	 * and necessary BEFORE the clear below, which would otherwise push a
	 * black field to the panel.
	 *
	 * The driver's MONO10 declaration looks wrong for the whole uc81xx
	 * family rather than just the IL0373, which would make it a fourth
	 * candidate upstream contribution — but one panel is not evidence about
	 * uc8175/uc8176, so this rung compensates locally and records the
	 * question instead of changing a shared driver's capabilities.
	 */
	cfb_framebuffer_invert(disp_dev);
	disp_grid_reset();
	/* Clear the CFB buffer + shadow grid but do NOT push to the glass (second arg
	 * false): e-paper holds its image unpowered, and the deep-sleep clock's ordinary
	 * strike wakes must not touch the panel (design §5) — pushing here wiped the
	 * watch-end stats 15 s (device: 30 min) after every paint, leaving the panel
	 * blank ~87% of the time. The watch-end paint does a full-buffer refresh, so
	 * whatever the glass held before this boot is replaced wholesale at the next
	 * paint; a stale image after a reflash lasts at most one watch. */
	cfb_framebuffer_clear(disp_dev, false);

	/* Not CFB_DISPLAY_ROWS/COLS: those two are asymmetric for a panel that
	 * does not set SCREEN_INFO_MONO_VTILED, and this one (uc81xx) doesn't —
	 * it reports MONO_MSB_FIRST | EPD only. ROWS divides by ppt only in the
	 * VTILED case, so on this panel it hands back the raw pixel height (296)
	 * instead of the character-row count; COLS gets the right answer here
	 * only by the mirror-image coincidence. CFB_DISPLAY_HEIGHT/WIDTH are the
	 * raw pixel extents unconditionally, so dividing by the font cell size
	 * is the only derivation that is right for both axes on both kinds of
	 * panel. fw/fh come straight from a compiled-in font table entry
	 * (FONT_ENTRY_DEFINE), which is never zero for a real font, and
	 * cfb_get_font_size already sent us home on the error path above if
	 * there was no font at all — so this division is safe. */
	disp_rows   = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_HEIGHT) / fh;
	disp_cols   = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_WIDTH) / fw;
	disp_font_h = fh;

	/* tuvm#88 Finding 2c: a printed number is not an enforcement mechanism — a
	 * human has to notice a mismatch in a boot log, and won't. Hard-check the
	 * derived grid against the ONE place the host side also gets its numbers
	 * from (platform_backend.h) and refuse to mark the display available on a
	 * mismatch, so the seam reports unavailable rather than silently operating
	 * on a different geometry than every host test assumes. */
	if (disp_rows != OEVM_DISPLAY_ROWS || disp_cols != OEVM_DISPLAY_COLS) {
		printk("DISPLAY geometry mismatch: derived rows=%d cols=%d "
		       "(font=%ux%u) but OEVM_DISPLAY_ROWS/COLS=%d/%d — "
		       "refusing to mark display available\n",
		       disp_rows, disp_cols, fw, fh,
		       OEVM_DISPLAY_ROWS, OEVM_DISPLAY_COLS);
		return;
	}

	/* tuvm#88 Finding 1: uc81xx_controller_init leaves blanking_on == true,
	 * and uc81xx_write only issues the panel refresh command when blanking is
	 * OFF — with blanking on, every write loads controller RAM and returns 0
	 * (success) while the panel itself never repaints and stays blank. This
	 * call is what puts the driver into the mode where a write actually
	 * refreshes the glass; without it, display_target_refresh() would report
	 * success forever against a panel nothing ever appears on. */
	if (display_blanking_off(disp_dev)) {
		printk("DISPLAY blanking_off failed\n");
		return;
	}

	disp_ok = true;
	printk("DISPLAY OK rows=%d cols=%d font=%ux%u\n", disp_rows, disp_cols, fw, fh);
}
#endif /* CONFIG_DISPLAY && DT_HAS_CHOSEN(zephyr_display) */

int display_target_available(void)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	/* LAZY bringup (tuvm#94 stage 5): every display prim funnels through this check,
	 * and bringup must NOT run on wakes that never paint. The reason is in the uc81xx
	 * driver: display_blanking_off() is implemented as a full panel refresh of the
	 * controller RAM that controller_init just cleared to white — so a boot-time
	 * bringup blanks the glass on EVERY wake, wiping the previous watch-end's stats
	 * (witnessed on the 1-min demo build, 2026-07-25; the driver's own device init
	 * only resets the controller and leaves the glass holding its image). Strike
	 * wakes never call a display prim, so with bringup here the panel persists; paint
	 * wakes get the white flash immediately followed by their stats refresh. */
	static int disp_tried;

	if (!disp_tried) {
		disp_tried = 1;
		display_bringup();
	}
	return disp_ok ? 1 : 0;
#else
	return 0;   /* no display node on this board */
#endif
}

int display_target_clear(void)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	disp_grid_reset();
	return 0;
#else
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

int display_target_put(int row, int col, const char *s, int n)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	if (row < 0 || row >= disp_rows) return 1;
	if (col < 0 || n < 0 || col + n > disp_cols) return 1;

	/* Mutate the shadow only — exactly what the host's display_target_put
	 * does (platform.c). Rendering happens at refresh, so the two seams
	 * agree on WHEN the grid becomes pixels as well as on what it holds. */
	memcpy(&disp_grid[row][col], s, (size_t)n);
	return 0;
#else
	ARG_UNUSED(row); ARG_UNUSED(col); ARG_UNUSED(s); ARG_UNUSED(n);
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

int display_target_refresh(void)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	int err = disp_render();

	if (err) return err;
	return cfb_framebuffer_finalize(disp_dev) ? 2 : 0;
#else
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

int display_target_refresh_row(int row)
{
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
	int width = cfb_get_display_parameter(disp_dev, CFB_DISPLAY_WIDTH);
	int err;

	if (row < 0 || row >= disp_rows) return 1;

	/* Re-render the WHOLE framebuffer, then push only this row's band. The
	 * other rows render to identical bytes (same shadow), so the bounded
	 * write is unaffected — this costs RAM work, not panel time. */
	err = disp_render();
	if (err) return err;

	/* Full-width band on a character-row boundary — the only shape
	 * cfb_framebuffer_finalize_area supports, and the only one this seam needs. */
	return cfb_framebuffer_finalize_area(disp_dev, 0, row * disp_font_h,
					     width, disp_font_h) ? 2 : 0;
#else
	ARG_UNUSED(row);
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}
