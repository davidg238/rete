/* oevm on Zephyr — native_sim bring-up gate for the ISR->channel pulse seam
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

   This boot sequence mirrors oevm/src/oevm_main.c (the host Model-A
   appliance driver) EXACTLY, swapping only the console (wired through
   src/backend/zephyr/platform_zephyr.c) and the in-language millis clock
   (k_uptime_get() instead of clock_gettime(), via the oevm_millis hook
   src/vm.c already reserves for "the Zephyr appliance"). Unlike the sibling
   oevm project's ns-zephyr/app/src/main.c, oevm's GC does NOT do a
   conservative C-stack root scan (no gc_set_stack_base function exists in
   this engine — GC roots come from vm_stack/current_frame plus the explicit
   hooks below), so no stack-anchor call is needed here. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>  /* uart_poll_in — the strike-on-demand console */
#include <stdlib.h>               /* atoi, same */
#include <stdint.h>
#include <stdio.h>
#include "oevm.h"
#include "space.h"        /* rete#233 T10: space_channel_count at the deep-sleep line */
#include "reboot_note.h"  /* rete#215: the retained panic note (this file owns the storage) */
#include "observer.h"     /* observer_start (rete#215, invarlint#48) */
#include "oimg.h"
#include "thread_bringup.h"
#include "log_udp.h"
#include "target_backend.h"   /* invarlint#53, rete#440 R6a: the leaves and their wire-up entries, declared once */

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

/* Associate to WPA2 WiFi (2.4 GHz) from the CONFIG_OEVM_WIFI_* symbols, start
   DHCPv4, and wait for an IPv4 lease before the VM binds the listener. Mirrors
   thread_bringup()'s poll-until-ready style. Loud on the console so the operator
   can read the assigned IP and point a browser at it. */
static void wifi_bringup(void) {
    /* prj_esp32.conf now sets CONFIG_WIFI for the whole board, so this runs for
       EVERY esp32 node. A node with no credentials (empty CONFIG_OEVM_WIFI_SSID —
       i.e. no prj_esp32_wifi.local.conf overlay: TimerNode, perfstex #36) is not a
       WiFi node; skip, or it would burn ~30 s spinning for a DHCP lease that never
       comes. Only the HTTP node ships a non-empty SSID via the git-ignored overlay. */
    if (CONFIG_OEVM_WIFI_SSID[0] == '\0') { printk("wifi: no SSID configured, skipping\n"); return; }

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
    p.ssid        = (const uint8_t *)CONFIG_OEVM_WIFI_SSID;
    p.ssid_length = strlen(CONFIG_OEVM_WIFI_SSID);
    p.psk         = (const uint8_t *)CONFIG_OEVM_WIFI_PSK;
    p.psk_length  = strlen(CONFIG_OEVM_WIFI_PSK);
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
                /* Just the lease. The old form advertised ".../thermal", which is a
                   different node's endpoint — this main.c is shared by every esp32
                   node, and the deep-sleep clock serves nothing at all. */
                printk("wifi: up, ip=%s\n", ipbuf);
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

#ifdef CONFIG_OEVM_NET_HEARTBEAT
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
        k_msleep(CONFIG_OEVM_NET_HEARTBEAT_MS);
    }
}
/* Priority 7: preemptible and BELOW the net RX/TX threads, so the heartbeat can
   never itself be the reason a packet is not serviced. */
K_THREAD_DEFINE(net_hb_tid, 1536, net_heartbeat, NULL, NULL, NULL, 7, 0, 3000);
#endif /* CONFIG_OEVM_NET_HEARTBEAT */

#ifndef OEVM_MODEL_B
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
/* Model B: the program is a .oimg blob in flash, not baked C.
   kernel_boot() comes from oevm.h; oimg_load_blob(), oimg_root() and
   oimg_bootstrap() from oimg.h. */
#include <zephyr/storage/flash_map.h>
#if FIXED_PARTITION_EXISTS(program_a)
/* rete#310 / rete#233: the program lives in one of two flash slots, chosen and mapped by
   the runner (src/runner.c: the manifest, the signature seat, the trial). The firmware
   links no program. */
#include "ota.h"
#include "ota_backend.h"
#include "runner.h"
#else
extern const uint8_t oevm_blob_start[], oevm_blob_end[];   /* zephyr/blob.S */
#endif
#endif

extern uint64_t (*oevm_millis)(void);

/* Baked by cmake/gen_build_id.cmake into a generated TU, regenerated every
   build (#56 §5.4). A rodata literal, so `strings zephyr.elf` recovers it —
   which is what makes the ELF archive repairable after the fact. */
extern const char oevm_build_id[];

static uint64_t zephyr_millis(void) { return (uint64_t)k_uptime_get(); }

/* shipsBellsV3 §11.2 — ONE strike, from C, with no VM involved.
 *
 * This is bench instrumentation for the first energisation of a coil, not the
 * product path: the real striker will go through the gpio seam from nsl. It
 * exists because the first time current flows through a solenoid is the wrong
 * moment to also be debugging an image load, a heap size and a channel
 * reaction — if this pulse throws the plunger, every one of those is exonerated
 * in advance.
 *
 * Compiled ONLY when OEVM_SOLENOID_PULSE_MS is defined on the command line
 * (run.sh --conf-extra), so no ordinary build can strike anything by accident.
 *
 * BUSY-WAIT, NOT k_msleep. The on-time IS the measurement here — §11.2 walks
 * 100 -> 50 -> 30 -> 25 -> 20 -> 15 -> 10 ms hunting the minimum pulse that
 * still throws — and k_msleep quantises to the kernel tick, which would silently
 * round the bottom of that ladder up and make two rungs report as one. IRQs are
 * deliberately NOT locked: 15 ms of locked interrupts would be far more hostile
 * to a build that also carries a radio than the few microseconds an ISR can add
 * to the pulse, and that error is in the harmless direction against a cap of a
 * second. */
#if defined(OEVM_SOLENOID_PULSE_MS) && DT_NODE_EXISTS(DT_ALIAS(gpioout))
#ifndef OEVM_SOLENOID_PIN
#define OEVM_SOLENOID_PIN 18
#endif
/* §8's firmware on-time cap, enforced where it cannot be forgotten. ~10 W flows
 * while the coil is held; a fat-fingered --conf-extra is exactly the fault the
 * cap exists for, and a build error costs nothing next to a burnt coil. */
BUILD_ASSERT(OEVM_SOLENOID_PULSE_MS > 0 && OEVM_SOLENOID_PULSE_MS <= 1000,
             "OEVM_SOLENOID_PULSE_MS must be within (0, 1000] ms — shipsBellsV3 §8 on-time cap");

static void solenoid_oneshot(void) {
    /* The delay is for the operator, not the hardware: esptool's post-flash reset
       means the board is already running by the time a console reader attaches,
       and a strike nobody is listening to is a wasted flash cycle. */
    printk("strike: GPIO%d armed, %d ms pulse, firing in 3 s\n",
           OEVM_SOLENOID_PIN, OEVM_SOLENOID_PULSE_MS);
    k_sleep(K_SECONDS(3));

    /* gpio_target_set answers 1/0, where 0 means "this board declared no such
       output" — which on a bring-up path is the difference between "the driver
       did not fire" and "the firmware never addressed the pin". Never discard it. */
    int on  = gpio_target_set(OEVM_SOLENOID_PIN, 1);
    k_busy_wait(OEVM_SOLENOID_PULSE_MS * 1000);
    int off = gpio_target_set(OEVM_SOLENOID_PIN, 0);

    printk("strike: fired on=%d off=%d (1==pin driven, 0==pin not declared)\n", on, off);
}
#endif

/* shipsBellsV3 — STRIKE ON DEMAND from the console, so the §11.2 ladder can be
 * walked by ear without a reflash between rungs (2026-07-31 bench).
 *
 * Type a decimal number of milliseconds and Enter; that pulse fires immediately.
 * Deliberately NOT the Zephyr shell: CONFIG_SHELL would take ownership of the
 * console this image already prints the VM's output on, for one command. This is
 * ~30 lines of uart_poll_in against the same console device, and RX is otherwise
 * unused in this build (no shell, no console subsystem — verified in .config).
 *
 * Independent of OEVM_SOLENOID_PULSE_MS: this build has no boot strike at all,
 * which is the point — every strike is one the operator asked for, at a moment
 * they chose, so a ring nobody typed is unambiguously a fault rather than a
 * feature. That distinction is what the boot-window investigation still needs.
 */
#if defined(OEVM_SOLENOID_CONSOLE) && DT_NODE_EXISTS(DT_ALIAS(gpioout))
#ifndef OEVM_SOLENOID_PIN
#define OEVM_SOLENOID_PIN 18
#endif

/* Two limits, both about the coil rather than the code. 250 ms is well inside
 * §8's 1 s cap and already 2.5x the top of §11.2's ladder — an interactive path
 * where a stray keystroke can energise a coil does not need the full range. The
 * 1 s cooldown bounds the duty cycle when someone leans on Enter: at 250 ms per
 * strike that is 25%, and §7's ~10 W while held is what is being rationed. */
#define STRIKE_MAX_MS   250
#define STRIKE_COOLDOWN_MS 1000

static const struct device *const strike_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* The bare pulse. No limits and no cooldown of its own — every caller below
   applies those BEFORE getting here, once per operator request rather than once
   per strike, so a bell pattern is not fought by the single-strike governor. */
static void strike_pulse(int ms) {
    /* Busy-wait, not k_msleep: the on-time IS the measurement — see the note on
       solenoid_oneshot above for why the tick would blur the bottom rungs. */
    int on = gpio_target_set(OEVM_SOLENOID_PIN, 1);
    k_busy_wait(ms * 1000);
    int off = gpio_target_set(OEVM_SOLENOID_PIN, 0);
    printk("strike: %d ms fired on=%d off=%d\n", ms, on, off);
}

static int64_t strike_last;          /* uptime of the last operator request */
static int     strike_width = 10;    /* ms; what `bN` strikes with. 10 was solid on
                                        the bench 2026-07-31, 5 failed. */

static bool strike_governor(void) {
    int64_t now = k_uptime_get();
    if (strike_last && (now - strike_last) < STRIKE_COOLDOWN_MS) {
        printk("strike: too soon (%d ms since last, need %d) — coil duty cycle\n",
               (int)(now - strike_last), STRIKE_COOLDOWN_MS);
        return false;
    }
    strike_last = now;
    return true;
}

static void strike_fire(int ms) {
    if (ms <= 0 || ms > STRIKE_MAX_MS) {
        printk("strike: %d ms REFUSED (allowed 1..%d)\n", ms, STRIKE_MAX_MS);
        return;
    }
    if (!strike_governor()) {
        return;
    }
    strike_width = ms;   /* so `bN` rings at whatever width was last proven by ear */
    strike_pulse(ms);
}

/* N BELLS, STRUCK IN PAIRS — the ship's-bell convention, on the solenoid.
   Two quick strikes, a gap, two more, with a LONE FINAL STROKE for an odd count.

   THE GAPS ARE RUNTIME-SETTABLE, and the reason is a bench result: the 160/600 ms
   pair borrowed from bell_strike_pattern() above was "way too fast" on both gaps
   (2026-07-31). That is not a bug in either — the DAC bell is a synthetic tone
   that decays in a few hundred ms, while a real bell rings for seconds, so the
   rhythm that suits one crowds the other. Physical decay is a property of the
   bell on the bench, so it is tuned by ear, not chosen in a header.

   Defaults below are a deliberate over-correction from the DAC figures; expect to
   land them by ear with `p`/`g` and then promote whatever wins into the design.

   BELL_MAX_WIDTH_MS is the duty guard: eight strikes at the 250 ms single-shot
   ceiling would be 2 s of coil inside 3 s, so the pattern path caps the width far
   lower. At 50 ms x 8 it is 400 ms of coil, now spread over even longer. Gaps are
   OFF-time, so stretching them only ever improves the duty cycle. */
#define BELL_MAX_COUNT      8
#define BELL_MAX_WIDTH_MS   50
#define BELL_GAP_MIN_MS     50
#define BELL_GAP_MAX_MS     5000

static int bell_gap_pair = 500;    /* within a pair */
static int bell_gap_set  = 1600;   /* between pairs */

static void bell_show(void) {
    printk("bells: width %d ms, pair gap %d ms, set gap %d ms\n",
           strike_width, bell_gap_pair, bell_gap_set);
}

static void bell_set_gap(int *slot, const char *name, int ms) {
    if (ms < BELL_GAP_MIN_MS || ms > BELL_GAP_MAX_MS) {
        printk("bells: %s gap %d ms REFUSED (allowed %d..%d)\n",
               name, ms, BELL_GAP_MIN_MS, BELL_GAP_MAX_MS);
        return;
    }
    *slot = ms;
    bell_show();
}

static void strike_bells(int n) {
    if (n <= 0 || n > BELL_MAX_COUNT) {
        printk("bells: %d REFUSED (allowed 1..%d)\n", n, BELL_MAX_COUNT);
        return;
    }
    if (strike_width > BELL_MAX_WIDTH_MS) {
        printk("bells: width %d ms REFUSED for a pattern (max %d) — set it with a "
               "single strike first\n", strike_width, BELL_MAX_WIDTH_MS);
        return;
    }
    if (!strike_governor()) {
        return;
    }

    printk("bells: %d x %d ms (pair %d, set %d)\n",
           n, strike_width, bell_gap_pair, bell_gap_set);
    for (int i = 0; i < n; i++) {
        strike_pulse(strike_width);
        if (i == n - 1) {
            break;
        }
        /* even index = first of a pair, so the SHORT gap holds the pair together;
           odd index = second of a pair, so the LONG gap separates the pairs. */
        k_msleep((i % 2 == 0) ? bell_gap_pair : bell_gap_set);
    }
    strike_last = k_uptime_get();   /* cooldown runs from the END of the pattern */
    printk("bells: done\n");
}

static void strike_console(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    char line[8];
    int n = 0;
    unsigned char ch;

    if (!device_is_ready(strike_uart)) {
        printk("strike-console: console uart not ready\n");
        return;
    }
    printk("strike-console: <ms>=one strike (1..%d)  b<n>=ring n bells (1..%d)  "
           "p<ms>=pair gap  g<ms>=set gap  s=show\n",
           STRIKE_MAX_MS, BELL_MAX_COUNT);

    for (;;) {
        if (uart_poll_in(strike_uart, &ch) != 0) {
            k_msleep(20);   /* nothing typed; this thread must not spin */
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (n) {
                line[n] = '\0';
                switch (line[0]) {
                case 'b': strike_bells(atoi(line + 1)); break;
                case 'p': bell_set_gap(&bell_gap_pair, "pair", atoi(line + 1)); break;
                case 'g': bell_set_gap(&bell_gap_set,  "set",  atoi(line + 1)); break;
                case 's': bell_show(); break;
                default:  strike_fire(atoi(line)); break;
                }
                n = 0;
            }
        } else if ((ch >= '0' && ch <= '9') ||
                   (n == 0 && (ch == 'b' || ch == 'p' || ch == 'g' || ch == 's'))) {
            if (n < (int)sizeof(line) - 1) {
                line[n++] = (char)ch;
            }
        } else {
            n = 0;   /* anything else abandons the line rather than firing a
                        partially-parsed width */
        }
    }
}

/* Low priority and a small stack: it polls, sleeps, and calls two gpio setters.
   Started by the kernel after all init levels, so the SYS_INIT in gpio_zephyr.c has already
   driven the pin low before this thread can touch it. */
K_THREAD_DEFINE(strike_console_tid, 1024, strike_console, NULL, NULL, NULL, 10, 0, 0);
#endif

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

    /* Before anything that can die: a VM that faults on its first instruction
       must still leave a log segment naming the binary it faulted in (#56 §5.4).
       After the USB-CDC wait above, or on those boards the banner is emitted
       into an unenumerated console and lost — the same "Issue A" reason that
       sleep exists. printk, not printf: the fault path gags newlib stdio. */
    printk("oevm: build-id %s\n", oevm_build_id);

    /* rete#215: validate the retained note before anything can read it, and before the
       remote console starts — a note that was going to be reported must not be lost to a
       crash during mesh bring-up, and a note that is GARBAGE must not survive to be
       reported as a death that never happened. After the build-id line so the boot that
       reports a note is attributable to the binary that reports it. */
    reboot_note_init();
    if (reboot_note_consecutive() > 0) {
        struct RebootNote rn;
        if (reboot_note_read(&rn)) {
            printk("oevm: reboot-note reason=%u mcause=0x%08x mepc=0x%08x ra=0x%08x "
                   "boot=%u up=%ums consec=%u\n",
                   (unsigned)rn.reason, (unsigned)rn.mcause, (unsigned)rn.mepc,
                   (unsigned)rn.ra, (unsigned)rn.boot_id, (unsigned)rn.mono_ms,
                   (unsigned)rn.consecutive);
        }
    }

    /* The remote console, installed HERE — immediately after the build-id and
       before the mesh comes up. Everything printed from now on is buffered and
       goes out once there is a route, so the boot the remote log shows starts at
       the same line the local one does and carries the build id that names the
       binary. Earlier is not possible for the ESP-ROM lines: they are printed by
       the bootloader, before any Zephyr code runs at all. */
    log_udp_start();

#ifdef CONFIG_OEVM_FAULT_TRIGGER
    /* #56 D8: fault on purpose, from a line whose number is the thing under
       test. `volatile` so the compiler cannot treat this as UB it may delete.
       After the build-id banner so the run is attributable — a fault whose log
       segment names no binary is exactly what D4 refuses to symbolize. */
    printk("oevm: faulting on purpose (CONFIG_OEVM_FAULT_TRIGGER)\n");
    {
        volatile int *oevm_fault_target = (volatile int *)0;
        *oevm_fault_target = 1;
    }
#endif

    oevm_millis = zephyr_millis;

    /* rete#215: arm the wedge detector before the VM runs, and AFTER the radio — a node
       that wedges during OpenThread bring-up has not yet declared any timer, so the
       detector answers OBS_COND_NONE and cannot trip on it. Arming earlier would buy nothing
       and would put the first reboot inside a path that has no note to write about it. */
    observer_start();

#ifdef OEVM_MODEL_B
    kernel_boot();   /* ONCE, before load/bootstrap — the order src/oevm_image_main.c uses */
    /* rete#37: the load walks every method once (the verifier the dispatch loop trusts), and
       every deep-sleep wake is a boot, so its cost is printed rather than guessed. */
    uint32_t load_c0 = k_cycle_get_32();
#if FIXED_PARTITION_EXISTS(program_a)
    const uint8_t *prog = NULL;
    size_t prog_size = 0;
    if (!runner_select(&prog, &prog_size)) {
        printk("oevm: no program to run (runner); the console stays up\n");
        return 0;
    }
    load_c0 = k_cycle_get_32();
    int load_rc = oimg_load_blob(prog, prog_size);
#else
    int load_rc = oimg_load_blob(oevm_blob_start, (size_t)(oevm_blob_end - oevm_blob_start));
#endif
    printk("oevm: image loaded and verified (%u us)\n",
           (unsigned)k_cyc_to_us_floor32(k_cycle_get_32() - load_c0));
    if (load_rc != 0) {
        /* Loud: never boot a bad blob silently. The specific check that failed
           was just printed to stderr by the loader. */
        printk("oevm: oimg_load_blob failed\n");
        return 0;
    }
#if FIXED_PARTITION_EXISTS(program_a)
    {
        /* rete#310 / IMAGE-ARTIFACTS.md decision 2: the node names the program it
           runs. Printed for `custos load` on the cable; noted for the ota seam, so
           the kernel's publishApps writes obs/apps/main. The cost is printed
           rather than guessed. */
        int64_t t0 = k_uptime_get();
        uint8_t h[32];
        ota_sha256(prog, (long)prog_size, h);
        ota_note_running_hash(h);
        printk("oevm: program sha256 %s (%u ms)\n", ota_env("OEVM_IMAGE_HASH"),
               (unsigned)(k_uptime_get() - t0));
    }
#endif
#else
    init_solidified_vtables();
    init_reflection_symbols();
#endif

    wire_pulse_gpio();   /* Task 8 (DK): arm BUTTON1 as the pulse edge; no-op on native_sim */
    wire_gpio_outputs();   /* tuvm#82: configure declared outputs INACTIVE; no-op where none */

#if defined(OEVM_SOLENOID_PULSE_MS) && DT_NODE_EXISTS(DT_ALIAS(gpioout))
    /* Immediately after wire_gpio_outputs(), and before any radio bring-up: the
       pin is now driven de-energised rather than floating, and nothing else has
       had a chance to hold the CPU. */
    solenoid_oneshot();
#endif

#if defined(CONFIG_PWM) && DT_NODE_EXISTS(DT_ALIAS(spkren))
    magtag_chime();   /* spike: prove the MagTag speaker path at boot */
#endif

#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
    bell_bringup();   /* tuvm#94 stage 1: enable the DAC pin; nsl does the striking */
#endif

    /* Display bringup is NOT called here: it is lazy, inside display_target_available()
     * — the uc81xx blanking-off pushes a white refresh, so running it on every boot
     * wipes the e-paper between the deep-sleep clock's paints. Wakes that paint reach
     * it through their first display prim; wakes that don't never touch the glass. */

#ifdef CONFIG_NET_L2_OPENTHREAD
    oevm_thread_bringup();   /* shared OT bring-up seam (src/backend/zephyr/thread_bringup.c) */
#endif
#ifdef CONFIG_WIFI
    wifi_bringup();      /* #46: associate to WiFi + DHCPv4 before the VM binds the http listener */
#endif

#ifdef OEVM_MODEL_B
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
