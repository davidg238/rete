/* tuvm on Zephyr — native_sim bring-up gate for the ISR->channel pulse seam
   (Task 7). Boots the solidified VM (Model A: a per-program baked image, no
   .oimg blob at all — see tools/ns_compile_oevm.py), then runs the image's
   own `main:`, which registers a channel reaction, fires one simulated GPIO
   edge, and loops draining the pulse mailbox via
   `platform pumpEvents. space drain` (see ../PulseNode.nsl). On native_sim
   the zephyr pulse backend's pulse_pump (src/backend/zephyr/pulse_zephyr.c)
   BLOCKS (K_FOREVER) once the one-edge mailbox is drained, so
   run_solidified_entry_point() never returns for this gate node — that is
   expected (the gate is checked by console output under `timeout`, not by
   process exit).

   This boot sequence mirrors tuvm/src/oevm_main.c (the host Model-A
   appliance driver) EXACTLY, swapping only the console (wired through
   src/backend/zephyr/platform_zephyr.c) and the in-language millis clock
   (k_uptime_get() instead of clock_gettime(), via the oevm_millis hook
   src/vm.c already reserves for "the Zephyr appliance"). Unlike the sibling
   oevm project's ns-zephyr/app/src/main.c, tuvm's GC does NOT do a
   conservative C-stack root scan (no gc_set_stack_base function exists in
   this engine — GC roots come from vm_stack/current_frame plus the explicit
   hooks below), so no stack-anchor call is needed here. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include "oevm.h"
#include "oimg.h"
#include "thread_bringup.h"

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>   /* NET_EVENT_IPV4_ADDR_ADD/DEL (#65) */
#include <zephyr/net/dhcpv4.h>
#include <string.h>

/* #65 INSTRUMENTATION — give the node a voice when it leaves the network.
   Deliberately LOG-ONLY, not a reconnect: the open half of #65 is a node that
   goes unreachable under load with a silent console and recovers only on a
   hardware reset, and until we have SEEN which event precedes it, auto-reconnect
   would paper over the very evidence we are collecting. Measured 2026-07-21: a
   run of ~800 served requests plus ~1000 refusals printed NOTHING past the boot
   banner, because nothing here was ever registered — a deauth, a lost lease, or
   an iface down were all equally invisible. That silence is why #65 reads as
   "no crash, no log". Cost is two callback structs and the strings.
   These fire on the net_mgmt event thread, not the reactor. */
static void wifi_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
                     struct net_if *iface) {
    ARG_UNUSED(iface);
    switch (ev) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        printk("wifi-evt: CONNECT_RESULT status=%d\n", st ? st->status : -1);
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT: {
        /* THE line #65 is hunting. If it appears at the moment the node stops
           answering ARP, the fatal mode is an association loss with no reconnect
           path — and the fix is a reconnect, not anything in the socket layer. */
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        printk("wifi-evt: DISCONNECT_RESULT status=%d  <-- NODE LEFT THE BSS\n",
               st ? st->status : -1);
        break;
    }
    default:
        printk("wifi-evt: unhandled event %llx\n", (unsigned long long)ev);
        break;
    }
}

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
                     struct net_if *iface) {
    ARG_UNUSED(cb); ARG_UNUSED(iface);
    switch (ev) {
    case NET_EVENT_IPV4_ADDR_ADD: printk("ip-evt: IPv4 address ADDED\n"); break;
    /* Distinguishes "lost the lease / address" from "lost the association" —
       the two produce the same symptom from the host side (ARP INCOMPLETE) and
       need different fixes. */
    case NET_EVENT_IPV4_ADDR_DEL: printk("ip-evt: IPv4 address REMOVED  <-- NO ADDRESS\n"); break;
    default: break;
    }
}

static struct net_mgmt_event_callback wifi_cb, ipv4_cb;

/* Associate to WPA2 WiFi (2.4 GHz) from the CONFIG_TUVM_WIFI_* symbols, start
   DHCPv4, and wait for an IPv4 lease before the VM binds the listener. Mirrors
   thread_bringup()'s poll-until-ready style. Loud on the console so the operator
   can read the assigned IP and point a browser at it. */
static void wifi_bringup(void) {
    /* prj_esp32.conf now sets CONFIG_WIFI for the whole board, so this runs for
       EVERY esp32 node. A node with no credentials (empty CONFIG_TUVM_WIFI_SSID —
       i.e. no prj_esp32_wifi.local.conf overlay: TimerNode, perfstex #36) is not a
       WiFi node; skip, or it would burn ~30 s spinning for a DHCP lease that never
       comes. Only the HTTP node ships a non-empty SSID via the git-ignored overlay. */
    if (CONFIG_TUVM_WIFI_SSID[0] == '\0') { printk("wifi: no SSID configured, skipping\n"); return; }

    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { printk("wifi: no wifi iface\n"); return; }

    /* #65: registered BEFORE the connect request so the connect result itself is
       reported, and so nothing between here and the lease can happen unobserved. */
    net_mgmt_init_event_callback(&wifi_cb, wifi_evt,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt,
                                 NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL);
    net_mgmt_add_event_callback(&ipv4_cb);

    struct wifi_connect_req_params p;
    memset(&p, 0, sizeof p);
    p.ssid        = (const uint8_t *)CONFIG_TUVM_WIFI_SSID;
    p.ssid_length = strlen(CONFIG_TUVM_WIFI_SSID);
    p.psk         = (const uint8_t *)CONFIG_TUVM_WIFI_PSK;
    p.psk_length  = strlen(CONFIG_TUVM_WIFI_PSK);
    p.security    = WIFI_SECURITY_TYPE_PSK;
    p.channel     = WIFI_CHANNEL_ANY;
    p.band        = WIFI_FREQ_BAND_2_4_GHZ;
    p.mfp         = WIFI_MFP_OPTIONAL;

    int cr = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof p);
    printk("wifi: connect(ssid_len=%u) req=%d\n", (unsigned)p.ssid_length, cr);

    net_dhcpv4_start(iface);

    char ipbuf[NET_IPV4_ADDR_LEN];
    for (int i = 0; i < 300; i++) {           /* up to ~30 s for associate + lease */
        struct net_if_ipv4 *v4 = iface->config.ip.ipv4;   /* null-check BEFORE forming a ptr into it */
        if (v4) {
            struct net_if_addr *ua = &v4->unicast[0].ipv4;
            if (ua->addr_state == NET_ADDR_PREFERRED &&
                ua->address.in_addr.s_addr != 0) {
                net_addr_ntop(AF_INET, &ua->address.in_addr, ipbuf, sizeof ipbuf);
                printk("wifi: up, ip=%s -> http://%s:18080/thermal\n", ipbuf, ipbuf);
                return;
            }
        }
        k_msleep(100);
    }
    printk("wifi: no IPv4 lease after 30s, continuing\n");
}
#endif /* CONFIG_WIFI */

#if defined(CONFIG_PWM) && DT_NODE_EXISTS(DT_ALIAS(spkren))
#include <zephyr/drivers/pwm.h>
/* MagTag speaker SPIKE (tuvm#85 follow-on). A PWM square-wave chime on GPIO17 —
   the PAM8302 amp's audio input, routed from LEDC channel 0 by the board
   overlay's ledc0_default pinctrl group — un-muted by driving SPEAKER_ENABLE
   (GPIO16) high. Square wave is buzzy but proves the whole path end to end; a
   real bell tone wants the DAC (GPIO17 = DAC_1), a later rung. Compiled only
   when CONFIG_PWM is set (the button/WiFi builds leave it off), so those images
   are byte-for-byte unaffected. */
static const struct device *const chime_pwm = DEVICE_DT_GET(DT_NODELABEL(ledc0));
static const struct gpio_dt_spec chime_en   = GPIO_DT_SPEC_GET(DT_ALIAS(spkren), gpios);

static void magtag_chime(void) {
    if (!device_is_ready(chime_pwm) || !gpio_is_ready_dt(&chime_en)) {
        printk("chime: pwm or enable pin not ready\n");
        return;
    }
    gpio_pin_configure_dt(&chime_en, GPIO_OUTPUT_ACTIVE);   /* un-mute the amp */

    /* THE AMP NEEDS TO SETTLE. Enabling SPEAKER_SHUTDOWN and driving PWM in the
       same breath is silent — measured on the bench, and the reason three
       earlier builds looked like a routing bug when nothing was wrong. Give the
       PAM8302 a moment before the first edge. */
    gpio_pin_configure_dt(&chime_en, GPIO_OUTPUT_ACTIVE);   /* un-mute, and hold */
    k_msleep(50);

    /* An ascending C-major triad. Every note is above the ~1 kHz LEDC floor
       measured on this part (500 Hz -> -EINVAL, 1000 Hz -> ok): the esp32s2
       LEDC counter is 14 bits (SOC_LEDC_TIMER_BIT_WIDTH) and the driver must fit
       clock/freq inside it, so low notes are simply unrepresentable. A real bell
       — decaying partials, low fundamental — therefore wants the DAC on this same
       pin (GPIO17 = DAC_1/A0, which is what the vendor guide drives), not PWM. */
    /* PRIME — cheap insurance, not a fix for an "always" bug. A COLD first
       pwm_set can return -EINVAL when the driver's slow clock source is not yet
       enabled: it falls back to the full 14-bit resolution, and the prescaler
       ((clk<<8)/freq/2^res) then underflows the driver's 0x100 minimum. LOW
       frequencies are the ones at risk — measured on the bench, first-call
       500 Hz -> 250 and 1047 Hz -> 119 were both rejected, while a first-call
       2000 Hz -> 500 passed. So spend the risky first call on a throwaway 2 kHz
       rather than on the chime's opening note. */
    uint32_t p_prime = 1000000000U / 2000U;
    int rc_prime = pwm_set(chime_pwm, 0, p_prime, p_prime / 2U, 0);
    printk("chime: prime rc=%d\n", rc_prime);

    /* Notes are LONG and run back-to-back with no silence between them: the
       buzzer is tiny, and 160 ms notes were inaudible on the bench while a 2 s
       tone was loud. Stepping the pitch without stopping keeps it unmistakably
       one rising chime rather than three easily-missed blips. */
    static const uint32_t notes_hz[] = { 1319, 1568, 2093 };   /* E6, G6, C7 */
    for (size_t i = 0; i < ARRAY_SIZE(notes_hz); i++) {
        uint32_t period_ns = 1000000000U / notes_hz[i];
        int rc = pwm_set(chime_pwm, 0, period_ns, period_ns / 2U, 0);
        printk("chime: %u Hz rc=%d\n", notes_hz[i], rc);
        k_msleep(500);
    }
    (void)pwm_set(chime_pwm, 0, p_prime, 0, 0);   /* pulse 0 = silence */

    gpio_pin_configure_dt(&chime_en, GPIO_OUTPUT_INACTIVE);   /* re-mute the amp */
    printk("chime: played\n");
}
#endif /* CONFIG_PWM && spkren */

#include "platform_backend.h"   /* OEVM_DISPLAY_ROWS/COLS — the one place the grid is named (tuvm#88) */

#if DT_HAS_CHOSEN(zephyr_display)
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
#if DT_HAS_CHOSEN(zephyr_display)
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

#if DT_HAS_CHOSEN(zephyr_display)
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
	cfb_framebuffer_clear(disp_dev, true);

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
#endif /* DT_HAS_CHOSEN(zephyr_display) */

int display_target_available(void)
{
#if DT_HAS_CHOSEN(zephyr_display)
	return disp_ok ? 1 : 0;
#else
	return 0;   /* no display node on this board */
#endif
}

int display_target_clear(void)
{
#if DT_HAS_CHOSEN(zephyr_display)
	disp_grid_reset();
	return 0;
#else
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

int display_target_put(int row, int col, const char *s, int n)
{
#if DT_HAS_CHOSEN(zephyr_display)
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
#if DT_HAS_CHOSEN(zephyr_display)
	int err = disp_render();

	if (err) return err;
	return cfb_framebuffer_finalize(disp_dev) ? 2 : 0;
#else
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

int display_target_refresh_row(int row)
{
#if DT_HAS_CHOSEN(zephyr_display)
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

#ifdef CONFIG_TUVM_NET_HEARTBEAT
/* #65 diagnostic — a census of the packet pools on its OWN thread.
   Two things make this the right next instrument after the net_mgmt handlers
   proved the node stays associated while going unreachable:
     1. It runs on a thread that is NOT the VM reactor and NOT the net stack, so
        the line stopping is itself a result — it separates "a pool drained to
        zero" (exhaustion, the counts march down and stay down) from "nothing is
        scheduling any more" (deadlock, the line simply stops mid-run).
     2. Every number is read with no allocation, so observing costs none of the
        resource being observed.
   Deliberately printk and not LOG_INF: the log subsystem buffers through its own
   pool and a wedged system is exactly when that stops coming out. */
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_context.h>
#include "http_backend.h"

static void count_ctx(struct net_context *ctx, void *user_data) {
    ARG_UNUSED(ctx);
    (*(int *)user_data)++;
}

static void net_heartbeat(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct k_mem_slab *rx, *tx;
    struct net_buf_pool *rx_data, *tx_data;
    for (;;) {
        net_pkt_get_info(&rx, &tx, &rx_data, &tx_data);
        int ctxs = 0;
        net_context_foreach(count_ctx, &ctxs);
        /* pkt slabs are k_mem_slab (always countable); the DATA pools need
           CONFIG_NET_BUF_POOL_USAGE, which the netdiag overlay turns on. */
        printk("hb %u ms: pkt rx=%u/%u tx=%u/%u  data rx=%ld/%u tx=%ld/%u  ctx=%d  inflight=%d\n",
               (unsigned)k_uptime_get_32(),
               k_mem_slab_num_free_get(rx), rx->info.num_blocks,
               k_mem_slab_num_free_get(tx), tx->info.num_blocks,
               atomic_get(&rx_data->avail_count), rx_data->buf_count,
               atomic_get(&tx_data->avail_count), tx_data->buf_count,
               ctxs, http_inflight_count());
        /* #65: accept failures that were not "nothing pending". Non-zero is the
           signature of the wedge — the listener cannot accept at all. */
        uint32_t af = http_accept_failures();
        if (af) printk("hb   accept-failures=%u  <-- LISTENER CANNOT ACCEPT\n", (unsigned)af);
        k_msleep(CONFIG_TUVM_NET_HEARTBEAT_MS);
    }
}
/* Priority 7: preemptible and BELOW the net RX/TX threads, so the heartbeat can
   never itself be the reason a packet is not serviced. */
K_THREAD_DEFINE(net_hb_tid, 1536, net_heartbeat, NULL, NULL, NULL, 7, 0, 3000);
#endif /* CONFIG_TUVM_NET_HEARTBEAT */

/* Task 8 (nRF52840-DK acceptance bar): bind the on-board BUTTON1 (P0.11) as the
   PHYSICAL edge source for the pulse seam, via the `pulsegpio` DT alias added in
   boards/nrf52840dk_nrf52840.overlay. Guarded on the alias's existence: for the
   native_sim gate (Task 7) no overlay is applied, so the alias is absent and this
   whole block — and the wire_pulse_gpio() call in main() — compiles out, leaving
   that build byte-for-byte identical (its edge still comes from the node's
   sim-only `gpio fire:`). On the DK, each button press runs pulse_isr in IRQ
   context, which posts the pulse for the SAME logical pin the baked node
   (PulseNodeDK.nsl) registered. */
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))

/* Logical pin the DK node registers via `gpio pin: 13 ...` (PulseNodeDK.nsl).
   The PHYSICAL pin (P0.11) is decoupled from it — the ISR maps one to the
   other, mirroring how a driver maps a board pin to an app-level channel.
   Used ONLY on single-button boards; see pulse_logical_of() below. */
#define PULSE_LOGICAL_PIN 13

/* Debounce window, milliseconds. See
   docs/design/2026-07-23-magtag-buttons-leds-design.md D3 for WHY this lives in
   C rather than nsl, and note the reason is NOT the obvious one:
   pulse_post_isr already coalesces per (chan_id, handle_idx) and sheds rather
   than aborting when full (src/pulse_backend.h:17, G2/tuvm#20), so bounce can
   neither flood the mailbox nor kill the node. What coalescing CANNOT do is
   survive a pump — it merges only while an entry is still queued, so once
   pulse_pump drains it the next bounce edge enqueues afresh and the node sees
   several logical presses. Debounce here is therefore a CORRECTNESS measure
   (one physical press == one logical event), not load shedding.
   30 ms is conventional and NOT measured on these particular switches. */
#ifndef OEVM_BUTTON_DEBOUNCE_MS
#define OEVM_BUTTON_DEBOUNCE_MS 30
#endif

/* THE RELEASE EDGE IS INTERNAL, not an event. The debounce state machine below
   watches BOTH edges (see GPIO_INT_EDGE_BOTH in wire_pulse_gpio) — but only the
   press fires a logical pin. The release edge is consumed here purely to re-arm
   the state machine (`in->pressed = false`); it produces nothing a node can see.

   Held-state / chording ("hold A, press B") is answered by `gpio level:` instead
   (tuvm#82), which a node reads only when it cares — MagTagChordNode.nsl reads
   levels 15/14/12/11 on a press. The earlier release-event route (a settled
   release firing logical + OEVM_BUTTON_RELEASE_BASE, 100) doubled the event rate
   to carry state and was retired in tuvm#84; that is why no release pin is fired
   below and the OEVM_BUTTON_RELEASE_BASE macro is gone. */
extern void gpio_target_fire(int pin);   /* src/platform.c — ISR-safe */

/* One entry per button. `logical` is the pin an nsl node names in
   `gpio pin: N edge: ... post: ... on: <channel>`; src/platform.c's
   host_gpio_register table is already keyed per logical pin with its OWN channel
   and OWN posted message, so four buttons need no VM- or nsl-side change at all
   — only this file arms four physical pins instead of one. */
struct pulse_input {
    struct gpio_dt_spec spec;
    struct gpio_callback cb;
    int      logical;
    uint32_t last_ms;   /* uptime of the last ACCEPTED transition */
    bool     seen;      /* false until the first accepted transition (0 is a real uptime) */
    bool     pressed;   /* debounced state; a press fires only on released -> pressed */
};

static struct pulse_input pulse_inputs[] = {
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio), gpios) },
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio1))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio1), gpios) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio2))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio2), gpios) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio3))
    { .spec = GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio3), gpios) },
#endif
};
#define PULSE_N_INPUTS ARRAY_SIZE(pulse_inputs)

/* LOGICAL PIN POLICY, and why it is conditional rather than uniform.
   Single-button boards (nRF52840-DK P0.11, dongle P1.06) keep the historic 13,
   because PulseNodeDK.nsl and ThreadTimerNode already register `gpio pin: 13`
   and those are proven rung-7/8 gates — renumbering them here would break a
   working node to no purpose. Multi-button boards have no such legacy, so they
   take the DT spec's own GPIO number, which makes an nsl node read like the
   board silkscreen (`gpio pin: 15` IS the MagTag's BUTTON_A). Deriving it from
   the spec rather than hardcoding a per-board table also keeps board knowledge
   out of this shared file. */
static inline int pulse_logical_of(const struct pulse_input *in) {
    return (PULSE_N_INPUTS == 1) ? PULSE_LOGICAL_PIN : (int)in->spec.pin;
}

static void pulse_isr(const struct device *port, struct gpio_callback *cb,
                      uint32_t pins) {
    ARG_UNUSED(cb);
    uint32_t now = k_uptime_get_32();

    for (size_t i = 0; i < PULSE_N_INPUTS; i++) {
        struct pulse_input *in = &pulse_inputs[i];
        /* Demux: one callback per input, but the handler is shared, so match on
           (port, pin) rather than trusting which cb fired. Zephyr passes the pin
           bitmask in `pins` — the single-button version ignored it. */
        if (in->spec.port != port || !(pins & BIT(in->spec.pin))) {
            continue;
        }
        /* DEBOUNCED PRESS/RELEASE STATE MACHINE. Arrived at by measurement, after
           two simpler designs were tried on hardware and failed:

           (1) Edge-only ("fire on every interrupt, lock out for 30 ms"). The seam
               fired TWICE per press: hold BUTTON_A for 3 s and the second event
               lands 2944 ms later, exactly at release. Ten deliberate presses gave
               thirteen events, the extras 196/255/280 ms after their press — one
               per press held longer than the lockout, since a shorter press has
               its release swallowed by the window. That is why it first looked
               like intermittent contact bounce and is not: GPIO_INT_EDGE_TO_ACTIVE
               does NOT mean press-only here, despite Zephyr inverting the trigger
               for ACTIVE_LOW pins and gpio_esp32.c mapping TRIG_LOW -> NEGEDGE.
               A time lockout CANNOT fix this: the release happens an arbitrary
               time after the press, so no window is wide enough.

           (2) Edge + level check ("ignore interrupts where the pin reads
               inactive"). Better but still wrong: five 3 s holds gave EIGHT
               events, i.e. three releases still leaked. Sampling the level at ISR
               entry is a coin flip against a bouncing contact — the release
               bounces, and roughly half the time the ISR samples during a brief
               active glitch and fires.

           So: track the debounced STATE, and require a settled release before
           another press can fire. A press fires only on a genuine
           released -> pressed transition; bounce in either direction lands inside
           the 30 ms window and is absorbed.

           This needs a release interrupt to re-arm, which is exactly why
           GPIO_INT_EDGE_BOTH is requested in wire_pulse_gpio() rather than
           EDGE_TO_ACTIVE. Asking for both edges makes the re-arm GUARANTEED on any
           compliant driver instead of depending on what edge-to-active happens to
           mean — which is the assumption that broke here in the first place.
           gpio_pin_get_dt returns the LOGICAL level (1 == active), honouring
           GPIO_ACTIVE_LOW from the devicetree, so this stays polarity-correct on
           every board.

           Unsigned subtraction is wrap-correct across the ~49.7-day
           k_uptime_get_32() rollover. */
        int lvl = gpio_pin_get_dt(&in->spec);
        if (lvl < 0) {
            continue;   /* read failed; nothing sane to report from an ISR */
        }
        if (in->seen && (uint32_t)(now - in->last_ms) < OEVM_BUTTON_DEBOUNCE_MS) {
            continue;   /* still settling — absorbs bounce on BOTH edges */
        }
        if (!in->pressed) {
            if (lvl != 1) {
                continue;   /* already released; nothing to do */
            }
            in->pressed = true;
            in->seen    = true;
            in->last_ms = now;
            gpio_target_fire(pulse_logical_of(in));   /* -> pulse_post_isr (non-blocking) */
        } else {
            if (lvl != 0) {
                continue;   /* still held */
            }
            in->pressed = false;   /* settled release: re-arm, no event fired */
            in->seen    = true;
            in->last_ms = now;
            /* Release fires nothing: held-state is read via `gpio level:` (#82),
               not carried on a release event (#84). The transition above is the
               whole purpose of watching this edge — it re-arms the next press. */
        }
    }
}

static void wire_pulse_gpio(void) {
    for (size_t i = 0; i < PULSE_N_INPUTS; i++) {
        struct pulse_input *in = &pulse_inputs[i];

        if (!gpio_is_ready_dt(&in->spec)) {
            printk("pulsegpio[%u] device not ready\n", (unsigned)i);
            continue;   /* one dead pin must not cost the others */
        }
        in->logical = pulse_logical_of(in);
        gpio_pin_configure_dt(&in->spec, GPIO_INPUT);
        /* BOTH edges, not EDGE_TO_ACTIVE — deliberate, and the reason is measured
           rather than stylistic. pulse_isr fires a press only on a debounced
           released -> pressed transition, so it MUST see the release in order to
           re-arm. Requesting both edges makes that guaranteed on any compliant
           driver, instead of depending on what edge-to-active happens to mean on a
           given one: on this board EDGE_TO_ACTIVE already delivered release
           interrupts anyway, which is precisely the bug that made a press fire
           twice. Asking for what we actually need removes the assumption. */
        gpio_pin_interrupt_configure_dt(&in->spec, GPIO_INT_EDGE_BOTH);
        /* A callback per input rather than one shared mask: gpio_add_callback is
           per-port, and nothing guarantees a board puts every button on one port
           (the MagTag does; the next board might not). */
        gpio_init_callback(&in->cb, pulse_isr, BIT(in->spec.pin));
        gpio_add_callback(in->spec.port, &in->cb);
    }
}
#else
static inline void wire_pulse_gpio(void) { }   /* native_sim: no physical button */
#endif

/* tuvm#82: the OUTBOUND half of the gpio seam. Outputs are declared as gpioout /
   gpioout1..3 aliases, exactly as inputs are declared as pulsegpio / pulsegpio1..3,
   and the whole table compiles away on a board that declares none. An nsl node
   names the SILKSCREEN gpio number; the alias names never leave this file, which is
   why the lookup below matches on spec.pin. */
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
static const struct gpio_dt_spec gpio_outputs[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout), gpios),
#if DT_NODE_EXISTS(DT_ALIAS(gpioout1))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout1), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout2))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout2), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout3))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout3), gpios),
#endif
};
#define GPIO_N_OUTPUTS ARRAY_SIZE(gpio_outputs)

static void wire_gpio_outputs(void) {
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++) {
        if (!gpio_is_ready_dt(&gpio_outputs[i])) {
            printk("gpioout[%u] device not ready\n", (unsigned)i);
            continue;   /* one dead pin must not cost the others */
        }
        /* INACTIVE, not ACTIVE. On the MagTag gpioout1 is the NeoPixel POWER GATE,
           declared GPIO_ACTIVE_LOW, and coming up powered is exactly the quiescent
           draw the vendor guide says to avoid. INACTIVE therefore drives that line
           physically HIGH — the opposite of what the flag's name suggests, and
           correct. Do not "fix" this to GPIO_OUTPUT_ACTIVE. */
        gpio_pin_configure_dt(&gpio_outputs[i], GPIO_OUTPUT_INACTIVE);
    }
}
#else
static inline void wire_gpio_outputs(void) { }
#endif

/* The two leaves src/platform.c calls under #ifdef __ZEPHYR__. Return conventions
   match the host definitions they replace: set answers 1/0, level answers 0/1/-1,
   where -1 means "no board declared this pin" and becomes nil in nsl.
   LOGICAL levels throughout (gpio_pin_set_dt / _get_dt), so the overlay's
   GPIO_ACTIVE_LOW flags carry the polarity: true == NeoPixels POWERED, and
   true == button PRESSED. */
int gpio_target_set(int pin, int level) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin)
            return gpio_pin_set_dt(&gpio_outputs[i], level) == 0;
#endif
    ARG_UNUSED(pin); ARG_UNUSED(level);
    return 0;   /* not declared as an output on this board */
}

int gpio_target_level(int pin) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin) {
            int v = gpio_pin_get_dt(&gpio_outputs[i]);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))
    /* Searching the INPUT table too is the whole chording payoff: a node can ask
       "is BUTTON_A held right now" for zero mailbox traffic — the sole chording
       route since #84 retired the doubled press/release event stream. The table
       already exists and already holds the button specs, so this costs nothing.
       Match on the LOGICAL pin (pulse_logical_of), not spec.pin: a node names the
       logical pin it registered with `gpio pin: N ...`, and on single-button boards
       that pin is decoupled from the physical one (PULSE_LOGICAL_PIN 13 vs P0.11).
       Matching on spec.pin would make `gpio level: 13` on the nRF DK find nothing
       and raise #gpioUndeclared for a button that IS registered — and disagree with
       the host, which keys host_gpio_register by the logical pin. The READ still
       goes through the physical spec. On the MagTag logical==spec.pin, so this is
       identity there; it fixes the single-button case. */
    for (size_t i = 0; i < PULSE_N_INPUTS; i++)
        if (pulse_logical_of(&pulse_inputs[i]) == pin) {
            int v = gpio_pin_get_dt(&pulse_inputs[i].spec);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
    ARG_UNUSED(pin);
    return -1;
}

#ifndef TUVM_MODEL_B
/* XIP stubs — Model A never loads an .oimg blob; oimg.h's is_in_blob() checks
   oimg_blob_base first and returns 0 while it is NULL, so the XIP accessor
   paths are never taken. These definitions must live here (mirroring
   src/oevm_main.c) because src/oimg_loader.c is deliberately NOT linked into
   this build — see zephyr/CMakeLists.txt's comment for why.
   Under Model B these MUST NOT be compiled: src/oimg_loader.c is linked and
   defines all four itself (oimg_loader.c:15-19). */
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3] = {0};

extern void init_solidified_vtables(void);
extern void init_reflection_symbols(void);
extern oop  run_solidified_entry_point(void);
#else
/* Model B: the program is a .oimg blob in flash .rodata, not baked C.
   kernel_boot() comes from oevm.h and oimg_load_blob() from oimg.h, but
   oimg_root()/oimg_bootstrap() are declared in NO header (defined in
   src/oimg_loader.c, used by src/oevm_image_main.c), so they are externed here. */
extern const uint8_t tuvm_blob_start[], tuvm_blob_end[];   /* zephyr/blob.S */
extern oop  oimg_root(void);
extern oop  oimg_bootstrap(oop entry_vtable);
#endif

extern void (*gc_additional_roots_hook)(void);
extern uint64_t (*oevm_millis)(void);

static void oevm_roots_callback(void) {
    /* Standard GC roots (vm_stack, current_frame, g_active_stack) are scanned
       automatically by gc.c; this gate needs no additional roots. */
}

static uint64_t zephyr_millis(void) { return (uint64_t)k_uptime_get(); }

int main(void) {
    /* Unbuffered stdout: native_sim's console is a thin wrapper over the
       host's own stdout. Without this, libc line-buffers and a crash
       mid-run could hide output that already happened in-language. */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* stderr too: oimg_load_blob() reports EVERY validation failure (bad magic,
       format_version, registry_version, blob_size) via fprintf(stderr), and
       under Model B those messages are the only thing distinguishing a stale
       blob from a bad layout. CONFIG_STDOUT_CONSOLE wires stdout; nothing
       unbuffers stderr, so without this a failing blob can die silently.
       setvbuf is safe here — it is not on the ESP32 ROM's forced-newlib list
       (unlike fflush; see prj_esp32.conf), and the stdout call above already
       proves it out on this silicon. */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* USB-CDC-console boards (the nRF52840 dongle): give the SoC USB-CDC time to
       enumerate before OT bring-up + console output — else early prints are lost
       and OT on the boot path starves USB enumeration (see docs/ZEPHYR_LESSONS.md
       §1, the "Issue A" mitigation). Gated on the chosen console being a
       cdc-acm-uart, so the DK (uart0/VCOM console) and native_sim compile this
       out — their builds are unchanged. */
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)
    k_sleep(K_SECONDS(3));
#endif

    gc_additional_roots_hook = oevm_roots_callback;
    oevm_millis = zephyr_millis;

#ifdef TUVM_MODEL_B
    kernel_boot();   /* ONCE, before load/bootstrap — the order src/oevm_image_main.c uses */
    if (oimg_load_blob(tuvm_blob_start,
                       (size_t)(tuvm_blob_end - tuvm_blob_start)) != 0) {
        /* Loud: never boot a bad blob silently. The specific check that failed
           was just printed to stderr by the loader. */
        printk("tuvm: oimg_load_blob failed\n");
        return 0;
    }
#else
    init_solidified_vtables();
    init_reflection_symbols();
#endif

    wire_pulse_gpio();   /* Task 8 (DK): arm BUTTON1 as the pulse edge; no-op on native_sim */
    wire_gpio_outputs();   /* tuvm#82: configure declared outputs INACTIVE; no-op where none */

#if defined(CONFIG_PWM) && DT_NODE_EXISTS(DT_ALIAS(spkren))
    magtag_chime();   /* spike: prove the MagTag speaker path at boot */
#endif

#if DT_HAS_CHOSEN(zephyr_display)
    display_bringup();   /* tuvm#88: init CFB, verify geometry, blanking off, wire the seam */
#endif

#ifdef CONFIG_NET_L2_OPENTHREAD
    tuvm_thread_bringup();   /* shared OT bring-up seam (src/thread_bringup.c) */
#endif
#ifdef CONFIG_WIFI
    wifi_bringup();      /* #46: associate to WiFi + DHCPv4 before the VM binds the http listener */
#endif

#ifdef TUVM_MODEL_B
    oop result = oimg_bootstrap(oimg_root());
#else
    oop result = run_solidified_entry_point();  /* runs PulseNode>>main:args: —
        registers the reaction, fires the simulated edge, then blocks forever
        in [ true ] whileTrue: [ platform pumpEvents. space drain ] once the
        mailbox is drained (see pulse_zephyr.c's pulse_pump: K_FOREVER). */
#endif
    (void)result;

    /* Not normally reached (see above) — kept so a future node whose main:
       actually returns doesn't fall off main() into an undefined return. */
    for (;;) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
