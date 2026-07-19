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

#ifdef CONFIG_NET_L2_OPENTHREAD
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>   /* openthread_get_default_context, struct openthread_context */
#include <openthread.h>              /* Zephyr OT module: init/run/get_default_instance/mutex */
#include <openthread/thread.h>       /* otThreadGetDeviceRole, OT_DEVICE_ROLE_* */

/* Rung-7 (Layer 3): bring the OpenThread mesh up for the Zephyr SOCKET layer.
   With CONFIG_OPENTHREAD_MANUAL_START=y the boot SYS_INIT runs openthread_init()
   but NOT openthread_run(); and with NET_CONFIG off nothing calls net_if_up() on
   the OT iface. Root cause found this rung: openthread_run() enables IPv6+Thread
   (the node attaches, so ot-ctl ping + child table work) but NEVER admin-ups the
   Zephyr net_if — so zsock_sendto returns ENETDOWN (errno 115) even though OT is
   attached. openthread_run() + net_if_up() together fix it. Mirrors ns-zephyr's
   app-owned bring-up (app/src/main_b.c), adapted to the NCS v3.2.4 API. */
static void thread_bringup(void) {
    (void)openthread_init();                        /* idempotent: L2 inited at boot */
    int rr = openthread_run();                      /* enable IPv6 + Thread -> attach */
    struct openthread_context *ctx = openthread_get_default_context();
    struct net_if *iface = ctx ? ctx->iface : net_if_get_default();
    int ur = iface ? net_if_up(iface) : -1;         /* THE fix: admin-up the OT net_if */
    printk("thread: run=%d net_if_up=%d, joining...\n", rr, ur);

    otInstance *inst = openthread_get_default_instance();
    for (int i = 0; i < 300 && inst; i++) {         /* wait up to ~30s for attach */
        openthread_mutex_lock();
        otDeviceRole role = otThreadGetDeviceRole(inst);
        openthread_mutex_unlock();
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER) {
            printk("thread: attached role=%d\n", role);
            return;
        }
        k_msleep(100);
    }
    printk("thread: attach timed out, continuing\n");
}
#endif /* CONFIG_NET_L2_OPENTHREAD */

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <string.h>

/* Associate to WPA2 WiFi (2.4 GHz) from the CONFIG_TUVM_WIFI_* symbols, start
   DHCPv4, and wait for an IPv4 lease before the VM binds the listener. Mirrors
   thread_bringup()'s poll-until-ready style. Loud on the console so the operator
   can read the assigned IP and point a browser at it. */
static void wifi_bringup(void) {
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) { printk("wifi: no wifi iface\n"); return; }

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
        struct net_if_addr *ua = &iface->config.ip.ipv4->unicast[0].ipv4;
        if (iface->config.ip.ipv4 &&
            ua->addr_state == NET_ADDR_PREFERRED &&
            ua->address.in_addr.s_addr != 0) {
            net_addr_ntop(AF_INET, &ua->address.in_addr, ipbuf, sizeof ipbuf);
            printk("wifi: up, ip=%s -> http://%s:18080/thermal\n", ipbuf, ipbuf);
            return;
        }
        k_msleep(100);
    }
    printk("wifi: no IPv4 lease after 30s, continuing\n");
}
#endif /* CONFIG_WIFI */

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
   other, mirroring how a driver maps a board pin to an app-level channel. */
#define PULSE_LOGICAL_PIN 13

extern void gpio_target_fire(int pin);   /* src/platform.c — ISR-safe */

static const struct gpio_dt_spec pulse_in =
    GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio), gpios);
static struct gpio_callback pulse_cb;

static void pulse_isr(const struct device *port, struct gpio_callback *cb,
                      uint32_t pins) {
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    gpio_target_fire(PULSE_LOGICAL_PIN);   /* -> pulse_post_isr (non-blocking) */
}

static void wire_pulse_gpio(void) {
    if (!gpio_is_ready_dt(&pulse_in)) {
        printk("pulsegpio device not ready\n");
        return;
    }
    gpio_pin_configure_dt(&pulse_in, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&pulse_in, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&pulse_cb, pulse_isr, BIT(pulse_in.pin));
    gpio_add_callback(pulse_in.port, &pulse_cb);
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
    thread_bringup();    /* Rung-7: enable OT + admin-up the net_if before the VM binds sockets */
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
