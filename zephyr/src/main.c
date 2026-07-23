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
#include <zephyr/drivers/gpio.h>

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
    uint32_t last_ms;   /* uptime of the last ACCEPTED edge */
    bool     seen;      /* false until the first accepted edge (0 is a real uptime) */
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
        /* Fire on the FIRST edge and swallow the tail, so a press costs no
           latency. Unsigned subtraction is wrap-correct across the ~49.7-day
           k_uptime_get_32() rollover. */
        if (in->seen && (uint32_t)(now - in->last_ms) < OEVM_BUTTON_DEBOUNCE_MS) {
            continue;
        }
        in->seen = true;
        in->last_ms = now;
        gpio_target_fire(pulse_logical_of(in));   /* -> pulse_post_isr (non-blocking) */
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
        gpio_pin_interrupt_configure_dt(&in->spec, GPIO_INT_EDGE_TO_ACTIVE);
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
