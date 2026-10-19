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
#include <zephyr/init.h>          /* SYS_INIT — gpio_outputs_early_init, below */
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
#include "target_backend.h"   /* invarlint#53: the leaves this file defines, declared once */

/* OUTSIDE the CONFIG_WIFI block below, because the NTP seam is no longer inside
   it: a Thread node has SNTP and no WiFi. See ntp_target_sync(). */
#ifdef CONFIG_SNTP
#include <zephyr/net/sntp.h>        /* tuvm#94 stage 4: sntp_simple for the bells */
#include <zephyr/net/net_ip.h>      /* net_addr_pton, sockaddr_in6 — the literal path */
#endif

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

/* tuvm#94 stage 4: the device side of the NTP seam. `sntp_simple` does
   init/query/close in one call (include/zephyr/net/sntp.h:161), so the node needs no
   NTP state machine and this seam holds no state between calls — a sync is a
   request/response, not a session.
   We add the caller's tz offset to the epoch and reduce to seconds-since-local-
   midnight because an SMI is 2^30-1 and a Unix epoch (~1.75e9) overflows it. The
   reduction is done HERE, in 64-bit C, rather than in nsl for that reason.
   Any failure answers -1 (-> nil in nsl) so the clock free-runs: the network is
   allowed to improve the time, never to stop the bell.

   THE GUARD IS CONFIG_SNTP, NOT CONFIG_WIFI (changed 2026-08-01). It named the
   wrong dependency: this body needs the SNTP library, and WiFi is merely one way
   to have a network. The two are not the same set, in BOTH directions —
     - prj_esp32.conf (WROOM-32) sets CONFIG_WIFI=y and NOT CONFIG_SNTP, and
       nothing in Zephyr selects SNTP, so that build compiled a call to
       sntp_simple() with no implementation behind it. Only prj_esp32s2_wifi.conf
       ever set both, which is why the seam worked on the MagTag and nowhere else.
     - The C6 on Thread has a perfectly good IPv6 network and no WiFi at all, and
       was handed the -1 stub for that reason alone.
   Gating on the actual dependency fixes both at once. */
#ifdef CONFIG_SNTP
#ifndef OEVM_NTP_SERVER
#define OEVM_NTP_SERVER "pool.ntp.org"
#endif

/* OEVM_NTP_SERVER6 — a LITERAL IPv6 address, and if defined it wins over the
   hostname above. This is the Thread path, and the literal is the point rather
   than a shortcut: the porta mesh's OMR prefix is a ULA (fd77:9957:d9f3:1::/64)
   with no global IPv6 and no DNS64, so a hostname has nothing to resolve
   against. Going straight to a sockaddr also drops CONFIG_DNS_RESOLVER from the
   image, which on a part already carrying an FTD and a 128 KB VM arena is RAM
   worth not spending.
   sntp_simple_addr() fills in port 123 itself when the sockaddr's port is 0
   (net_port_set_default, sntp_simple.c:77), so the port is deliberately left
   unset here rather than byte-swapped by hand. */
/* The date half of the seam, filled in by the sync below and read by ntpDate.
   -1 until a sync has succeeded, which is the same "structural failure" answer
   ntpSyncTz: gives, so a node handles both with one nil check. */
static int ntp_last_date = -1;

int ntp_target_sync(int tz_offset_seconds) {
    struct sntp_time ts;
    int rc;

#ifdef OEVM_NTP_SERVER6
    struct sockaddr_in6 srv = { .sin6_family = AF_INET6, .sin6_port = 0 };

    if (net_addr_pton(AF_INET6, OEVM_NTP_SERVER6, &srv.sin6_addr) < 0) {
        /* A malformed literal is a build-time typo that would otherwise present
           as a silent -1 forever, indistinguishable from an unreachable server. */
        printk("ntp: OEVM_NTP_SERVER6 '%s' is not a valid IPv6 address\n",
               OEVM_NTP_SERVER6);
        return -1;
    }
    rc = sntp_simple_addr((struct sockaddr *)&srv, sizeof(srv), 4000, &ts);
    if (rc < 0) {
        printk("ntp: sntp_simple_addr(%s) rc=%d\n", OEVM_NTP_SERVER6, rc);
        return -1;
    }
#else
    rc = sntp_simple(OEVM_NTP_SERVER, 4000, &ts);   /* 4 s timeout */
    if (rc < 0) { printk("ntp: sntp_simple rc=%d\n", rc); return -1; }
#endif

    /* FLOOR division, not truncation: C's / rounds toward zero, so a pre-1970
       instant would land a day out and take the seconds-of-day with it. Modern
       dates never reach that branch, which is exactly why it would survive
       unnoticed if written wrong. */
    int64_t local = (int64_t)ts.seconds + tz_offset_seconds;
    int64_t days  = (local >= 0) ? (local / 86400) : ((local - 86399) / 86400);
    int64_t sod   = local - days * 86400;               /* seconds of local day */

    /* Howard Hinnant's civil_from_days: days-since-1970 -> y/m/d, exact for any
       Gregorian date and with no libc `time` dependency. gmtime_r would do it, but
       it drags in locale/timezone machinery this seam has no use for — the tz
       offset was already applied above, deliberately, because NTP hands out UTC
       and the timezone is a product fact the caller owns. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                        /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  yy  = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             /* [0, 365]   */
    unsigned mp  = (5 * doy + 2) / 153;                                 /* [0, 11]    */
    unsigned dd  = doy - (153 * mp + 2) / 5 + 1;                        /* [1, 31]    */
    unsigned mm  = (mp < 10) ? (mp + 3) : (mp - 9);                     /* [1, 12]    */
    yy += (mm <= 2);

    ntp_last_date = (int)(yy * 10000 + mm * 100 + dd);

    printk("ntp: ok, local %d sod=%lld\n", ntp_last_date, (long long)sod);
    return (int)sod;
}

int ntp_target_date(void) { return ntp_last_date; }
#else
/* No SNTP in this image: the symbols must still link, because platform.c's externs
   are unconditional and every Zephyr board shares that translation unit. Same rule
   as the bell pair — guard the BODY, never the definition. */
int ntp_target_sync(int tz_offset_seconds) { (void)tz_offset_seconds; return -1; }
int ntp_target_date(void) { return -1; }
#endif /* CONFIG_SNTP */

/* tuvm#94 stage 5: the RTC-retention seam, device side. On espressif SoCs g_rtc lands
   in the S2's 8 KB rtc_slow memory (0x50000000) via the ESP-IDF RTC_DATA_ATTR macro and
   survives deep sleep; on cold power-on it is LOADED as zeros (magic mismatch -> the
   node's cold-init path runs, guarded by the magic word). The field order is IDENTICAL
   to the host int32 array in platform.c and the node's rtc* index constants (design §3).
   The symbols must link on EVERY board (platform.c's externs are unconditional, one
   shared TU), so the definitions are unconditional and only the espressif placement is
   guarded — on other boards g_rtc is plain .bss, harmless as no non-MagTag board
   deep-sleeps.

   Use RTC_NOINIT_ATTR (from esp_attr.h), NOT RTC_DATA_ATTR: RTC_NOINIT_ATTR's
   `.rtc_noinit` is a NOLOAD section, so the value survives deep sleep AND a restart and
   is UNDEFINED only on true power-loss, which the magic word already guards. (The
   2026-07-24 handoff crash first blamed on the RTC_DATA_ATTR load segment was really
   the loader.c segment-walk bug below — any loaded RTC segment tripped it — but NOLOAD
   retention semantics remain the right fit regardless.) */
#define OEVM_RTC_FIELDS 14  /* 0-9 the app's, 10-13 the kernel's retained profile request (rete#415 Q8) */
#define OEVM_RTC_MAGIC  0x14BE11C0u  /* must fit a 32-bit SMI (31-bit tag): the node
                                        writes it via rtcPut:, and a magnitude >= 2^30
                                        never round-trips on a 32-bit target */
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_attr.h>   /* RTC_NOINIT_ATTR */
static RTC_NOINIT_ATTR int32_t g_rtc[OEVM_RTC_FIELDS];
#else
static int32_t g_rtc[OEVM_RTC_FIELDS];
#endif
int rtc_target_valid(void) { return ((uint32_t)g_rtc[0] == OEVM_RTC_MAGIC) ? 1 : 0; }
int rtc_target_get(int i) { if (i < 0 || i >= OEVM_RTC_FIELDS) return 0; return g_rtc[i]; }
void rtc_target_put(int i, int v) { if (i < 0 || i >= OEVM_RTC_FIELDS) return; g_rtc[i] = v; }

/* rete#233 T10: was this boot the deep-sleep timer? The runner's trial/revert reboot
   (sys_reboot -> RESET_SOFTWARE) and the observer's watchdog leave g_rtc valid, and the
   bell must not strike on them — a bell then is a bell at a meaningless time. hwinfo's
   RESET_LOW_POWER_WAKE is ESP_RST_DEEPSLEEP on this family (hwinfo_esp32.c). Answers 1
   when the cause cannot be read (no hwinfo reset cause, -ENOSYS): the rule bites only
   where the fact is known, and a board that cannot say keeps striking as before. Read
   once — the cause is this boot's and does not change. */
#ifdef CONFIG_HWINFO
#include <zephyr/drivers/hwinfo.h>
int reset_cause_timer_wake(void) {
    static int known = 0, answer = 1;
    if (!known) {
        uint32_t cause = 0;
        if (hwinfo_get_reset_cause(&cause) == 0 && cause != 0)
            answer = (cause & RESET_LOW_POWER_WAKE) ? 1 : 0;
        printk("boot: reset cause 0x%x timer wake %d\n", (unsigned)cause, answer);
        known = 1;
    }
    return answer;
}
#else
int reset_cause_timer_wake(void) { return 1; }
#endif

/* tuvm#100: the WALL-CLOCK seam, device side — an external RTC (DS3231 over I2C)
   read through Zephyr's uniform `rtc` API, so the seam is the API and not the
   part. The board names its RTC with the `wallclock` DT alias; a board that wires
   a different chip (the design's RV-3028-C7 fallback, §99) re-points that alias
   and this function does not change.

   Answers SECONDS SINCE LOCAL MIDNIGHT, computed HERE in C for ntp_target_sync's
   reason: it is the only reduction that fits an SMI, and doing it in nsl would
   mean shipping a 32-bit epoch through a 31-bit tag. The DS3231 holds LOCAL time
   directly (whatever was written to it), so unlike the NTP leaf there is no tz
   offset to apply — see prim_ids.h.

   -1 (-> nil in nsl) for every failure, and for a board with no RTC at all, so
   the clock free-runs: exactly the NTP leaf's contract. Note the symbol is
   defined UNCONDITIONALLY — platform.c's extern is unconditional and every Zephyr
   board shares that translation unit — so the guard is inside the body, never
   around the definition. Same rule as bell/ntp/rtc above.

   device_is_ready() is checked on EVERY call rather than cached at boot. It costs
   a pointer test, and the alternative caches a "yes" taken before the I2C bus was
   necessarily up; a bell that decides once at boot that it has no clock would
   free-run for the rest of its life on a transient. */
#if DT_NODE_EXISTS(DT_ALIAS(wallclock))
#include <zephyr/drivers/rtc.h>
#endif
/* The floor the unset-clock guard below compares against. A calendar year rather
   than a build date because it must not drift with the build: a device flashed
   once and hung on a wall for a decade keeps this comparison meaningful, whereas
   __DATE__ would start rejecting a correctly-set clock the moment the firmware
   outlived its own build year. Any value between the program's start and the
   present works; it only has to be above the DS3231's power-on 2000. */
#ifndef OEVM_WALLCLOCK_MIN_YEAR
#define OEVM_WALLCLOCK_MIN_YEAR 2020
#endif
int wallclock_target_read(void) {
#if DT_NODE_EXISTS(DT_ALIAS(wallclock))
    static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(wallclock));
    struct rtc_time tm;
    int rc;

    if (!device_is_ready(rtc_dev)) { printk("wallclock: device not ready\n"); return -1; }
    rc = rtc_get_time(rtc_dev, &tm);
    if (rc < 0) { printk("wallclock: rtc_get_time rc=%d\n", rc); return -1; }

    /* THE UNSET-CLOCK GUARD, and why it reads a year to answer a question about
       seconds. rtc_get_time() documents -ENODATA for "time has not been set"
       (include/zephyr/drivers/rtc.h:212) — but THIS DRIVER NEVER RETURNS IT.
       rtc_ds3231_get_time (drivers/rtc/rtc_ds3231.c:339) reads the seven time
       registers and converts, full stop; it never consults the DS3231's own
       oscillator-stop flag. So a part fresh from the factory, or one whose coin
       cell died while mains was off, answers rc=0 with a perfectly well-formed
       00:00:00 — and 00:00:00 is a VALID time, the exact moment eight bells are
       due. Trusting rc alone would make a dead battery ring midnight.

       OSF is the right signal and it is not reachable: rtc_ds3231_get_ctrl_sts()
       is static AND compiled only under CONFIG_RTC_ALARM (line 196), which is off
       here until the ISW wire lands. Making get_time answer -ENODATA when OSF is
       set is an upstream fix, and belongs on the same list as #91-#93/#95.

       Until then the YEAR is the proxy, because it is in the public struct and an
       unset DS3231 reports 2000 (tm_year 100, years-since-1900) while any clock
       someone actually set reports the year they set it to. This is weaker than
       OSF — it cannot catch a part that stopped this year — but it is exactly
       strong enough for the failure that matters on a shelf-stored bell: never
       set, or set once and then power-lost with a flat CR2032. */
    if (tm.tm_year + 1900 < OEVM_WALLCLOCK_MIN_YEAR) {
        printk("wallclock: unset (year %d)\n", tm.tm_year + 1900);
        return -1;
    }
    return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
#else
    return -1;   /* no RTC in this board's devicetree */
#endif
}

/* tuvm#100: the other half of the seam — set the RTC. `yyyymmdd` is a packed
   decimal date (20260731) and `seconds_of_day` is wallclock_target_read's own
   unit, so a set followed by a read is an identity. -1 (-> nil) for no RTC or a
   refused write; 0 on success.

   struct rtc_time is Zephyr's own alias for struct tm, so the two fields that bite
   everywhere else bite here: tm_year is YEARS SINCE 1900 (2026 -> 126) and tm_mon
   is 0-BASED (July -> 6). Getting either wrong writes a plausible-looking wrong
   date that the read side's year guard would then happily accept, so they are
   converted here, once, next to the comment that says why.

   The remaining fields are zeroed rather than left uninitialised: rtc_set_time
   hands the whole struct to the driver, which writes all seven registers, so a
   stack-garbage tm_wday reaches the part. tm_wday is set to 0 rather than
   computed — the DS3231 keeps a day-of-week counter that nothing in this product
   reads, and inventing a value would imply a correctness this seam does not
   provide. */
int wallclock_target_set(int yyyymmdd, int seconds_of_day) {
#if DT_NODE_EXISTS(DT_ALIAS(wallclock))
    static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(wallclock));
    struct rtc_time tm;
    int rc;

    if (seconds_of_day < 0 || seconds_of_day > 86399) return -1;
    if (!device_is_ready(rtc_dev)) { printk("wallclock: device not ready\n"); return -1; }

    memset(&tm, 0, sizeof tm);
    tm.tm_year = (yyyymmdd / 10000) - 1900;          /* years since 1900 */
    tm.tm_mon  = ((yyyymmdd / 100) % 100) - 1;       /* 0-based month */
    tm.tm_mday = yyyymmdd % 100;
    tm.tm_hour = seconds_of_day / 3600;
    tm.tm_min  = (seconds_of_day % 3600) / 60;
    tm.tm_sec  = seconds_of_day % 60;

    rc = rtc_set_time(rtc_dev, &tm);
    if (rc < 0) { printk("wallclock: rtc_set_time rc=%d\n", rc); return -1; }
    printk("wallclock: set %04d-%02d-%02d %02d:%02d:%02d\n",
           yyyymmdd / 10000, ((yyyymmdd / 100) % 100), yyyymmdd % 100,
           tm.tm_hour, tm.tm_min, tm.tm_sec);
    return 0;
#else
    (void)yyyymmdd; (void)seconds_of_day;
    return -1;   /* no RTC in this board's devicetree */
#endif
}

/* shipsBellsV3 §16.4 (#102) — THE RTC SAYS WHEN.
 *
 * Arms the DS3231's ALARM1 for a time-of-day and posts `msg` on `chan_id` when it
 * fires. Alarm1 rather than §16.4's Alarm2 for one reason: Alarm1 has a SECONDS
 * register and Alarm2 does not, and the node wants a lead time measured in seconds
 * (fire at boundary-5 s, count down to the boundary) rather than in whole minutes.
 * Everything else §16.4 settled is unchanged.
 *
 * The mask is the full hour+minute+second match, i.e. ONCE PER DAY, and the node
 * re-arms after every strike. A repeating mask cannot express "every half hour" on
 * this part, and the node must re-arm anyway to move the alarm forward — so the
 * repeating masks buy nothing and cost a second way to be wrong.
 *
 * Delivery is the ORDINARY pulse mailbox. The DS3231 driver takes the INT/SQW edge
 * in a real ISR and hands it to the system workqueue (rtc_ds3231_isw_isr ->
 * k_work_submit), so this callback runs in thread context — but it still only calls
 * pulse_post_isr, because that is the one path the run-to-completion loop drains and
 * it is non-allocating either way.
 *
 * A2F: the driver clears the alarm flag when it reads the state
 * (rtc_ds3231_alarm_is_pending), and clearing A2F/A1F is what DEASSERTS INT/SQW.
 * That matters more than it looks: the line is a LEVEL, not a pulse, so a failed
 * clear leaves it asserted and the NEXT alarm produces no new edge at all. The node
 * survives that through its watchdog; nothing here can.
 */
#if defined(CONFIG_RTC_ALARM) && DT_NODE_EXISTS(DT_ALIAS(wallclock))
#include "pulse_backend.h"           /* pulse_post_isr */

#define OEVM_RTC_ALARM_ID 0          /* DS3231 Alarm1 — the one with a seconds field */

static int oevm_alarm_chan = -1;
static int oevm_alarm_handle = -1;

static void oevm_rtc_alarm_cb(const struct device *dev, uint16_t id, void *user_data)
{
    ARG_UNUSED(dev); ARG_UNUSED(id); ARG_UNUSED(user_data);
    if (oevm_alarm_chan >= 0) {
        pulse_post_isr(oevm_alarm_chan, oevm_alarm_handle);
    }
}

/* The ISW line itself, read-only, purely as an instrument. The driver owns this
 * pin; reading its level cannot disturb it, and it is the one measurement that
 * separates "the RTC never asserted" from "the RTC asserted and nothing noticed".
 * Logical level (ACTIVE_LOW applied): 1 = ASSERTED = physically low. */
static const struct gpio_dt_spec oevm_isw =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(wallclock), isw_gpios, {0});

/* The DS3231 itself on i2c0. The `wallclock` alias names the RTC CHILD; its PARENT
 * is the maxim,ds3231-mfd node that owns the I2C address. Raw register reads only —
 * every WRITE stays the driver's, so this instrument cannot become a second owner. */
#include <zephyr/drivers/i2c.h>
static const struct i2c_dt_spec oevm_ds3231 = I2C_DT_SPEC_GET(DT_PARENT(DT_ALIAS(wallclock)));

int rtc_alarm_arm(int seconds_of_day, int chan_id, int handle_idx)
{
    static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(wallclock));
    struct rtc_time tm;
    int rc;

    if (seconds_of_day < 0 || seconds_of_day > 86399) return -1;
    if (!device_is_ready(rtc_dev)) { printk("rtcalarm: device not ready\n"); return -1; }

    /* CLEAR A STALE FLAG BEFORE ARMING, and say whether there was one.
     * INT/SQW is a LEVEL, not a pulse: it stays asserted until A1F/A2F is cleared.
     * So a flag left set from any earlier alarm holds the line low, and the next
     * alarm match produces NO NEW FALLING EDGE — the interrupt is armed, correct,
     * and permanently unheard. rtc_alarm_is_pending() clears the flag as it reads
     * it, which is exactly the write needed. Doing this on every arm makes the
     * mode unreachable rather than merely documented. */
    {
        int p0 = rtc_alarm_is_pending(rtc_dev, 0);
        int p1 = rtc_alarm_is_pending(rtc_dev, 1);
        int isw = oevm_isw.port ? gpio_pin_get_dt(&oevm_isw) : -1;
        if (p0 > 0 || p1 > 0 || isw > 0) {
            printk("rtcalarm: pre-arm a1f=%d a2f=%d isw=%d (1=asserted)\n", p0, p1, isw);
        }
        /* The part's own control and status bytes, read raw, because the driver's
         * return codes only say the WRITES were accepted. A1F setting while the
         * line stays high has exactly two explanations — the interrupt output is
         * not enabled, or the wire is not there — and these two bytes tell them
         * apart. ctrl: bit2 INTCN, bit1 A2IE, bit0 A1IE. status: bit1 A2F, bit0 A1F.
         * Bring-up instrument (#102); costs one 2-byte I2C read per arm. */
        if (oevm_ds3231.bus != NULL) {
            uint8_t reg = 0x0E, cs[2] = {0, 0};
            int rc2 = i2c_write_read_dt(&oevm_ds3231, &reg, 1, cs, sizeof cs);
            printk("rtcalarm: ctrl=%02x status=%02x (rc=%d) intcn=%d a1ie=%d\n",
                   cs[0], cs[1], rc2, (cs[0] >> 2) & 1, cs[0] & 1);
        }
    }

    /* Registered before the time is set, so an alarm that somehow fires between the
       two writes has somewhere to go. */
    oevm_alarm_chan = chan_id;
    oevm_alarm_handle = handle_idx;
    rc = rtc_alarm_set_callback(rtc_dev, OEVM_RTC_ALARM_ID, oevm_rtc_alarm_cb, NULL);
    if (rc < 0) { printk("rtcalarm: set_callback rc=%d\n", rc); return -1; }

    memset(&tm, 0, sizeof tm);
    tm.tm_hour = seconds_of_day / 3600;
    tm.tm_min  = (seconds_of_day % 3600) / 60;
    tm.tm_sec  = seconds_of_day % 60;

    rc = rtc_alarm_set_time(rtc_dev, OEVM_RTC_ALARM_ID,
                            RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MINUTE |
                            RTC_ALARM_TIME_MASK_SECOND, &tm);
    if (rc < 0) { printk("rtcalarm: set_time rc=%d\n", rc); return -1; }

    printk("rtcalarm: armed %02d:%02d:%02d isw=%d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
           oevm_isw.port ? gpio_pin_get_dt(&oevm_isw) : -1);
    return 0;
}
#else
int rtc_alarm_arm(int seconds_of_day, int chan_id, int handle_idx)
{
    /* No RTC in this board's devicetree, or an image built without
       CONFIG_RTC_ALARM. Refusing here is what makes the node print
       `trigger=poll ONLY` instead of silently behaving like the polling node —
       the failure class that cost twenty minutes over OEVM_NTP_SERVER6. */
    (void)seconds_of_day; (void)chan_id; (void)handle_idx;
    return -1;
}
#endif

#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#include <esp_sleep.h>
#endif
/* tuvm#94 stage 5: arm a timer wake and enter deep sleep. Direct ESP-IDF HAL call
   because Zephyr's sys_poweroff() arms no wake source (#83); CONFIG_POWEROFF links the
   sleep HAL. Never returns on device — the SoC powers down until the timer fires and
   the VM cold-boots. On a non-espressif board it is a no-op (the seam still links; only
   the MagTag path calls it).

   HISTORY (2026-07-24): enabling CONFIG_POWEROFF crashed the whole image at the
   ESP-boot -> Zephyr handoff, across five flashes and a WiFi-off / no-rom-patch
   isolation ladder. Root cause was NOT here: the sleep HAL's wake stub loads at
   0x40070000 (RTC IRAM), and loader.c's map_rom_segments classified any RTC IRAM/SLOW
   segment as "last", so the IROM/DROM flash-offset fix-up never ran and the MMU mapped
   stale offsets. Fixed in zephyr soc/espressif/common/loader.c (IS_RTC now covers all
   three RTC windows); upstream candidate alongside #91-#93/#95. */
void deep_sleep_target(int seconds) {
#if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32) && defined(CONFIG_POWEROFF)
#if DT_NODE_EXISTS(DT_ALIAS(neopixelpower))
    /* NeoPixel power gate (GPIO21, ACTIVE_LOW) driven inactive so the strip draws
       nothing while asleep (design §10). */
    {
        const struct gpio_dt_spec np = GPIO_DT_SPEC_GET(DT_ALIAS(neopixelpower), gpios);
        gpio_pin_configure_dt(&np, GPIO_OUTPUT_INACTIVE);
    }
#endif
    /* g_rtc retention across the sleep must not depend on the AUTO power-down
       heuristic (it currently keeps RTC slow mem on only because _rtc_slow_length
       links as a bogus nonzero value) — pin the domain ON explicitly. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    /* rete#233 T10: the registry high-water for THIS wake, so OEVM_SPACE_MAX_CHANNELS is
       set from a count. Every path this boot ever interned is still in the table. */
    printk("space: %d of %d channels\n", space_channel_count(), OEVM_SPACE_MAX_CHANNELS);
    /* Last console line of this wake, so a silent board distinguishes "never armed"
       from "armed and never woke". */
    printk("deep sleep %d s\n", seconds);
    /* The UDP console's sender polls its ring every 50 ms (log_udp.c); the sleep below
       kills it in under that, so without a wait the last three lines of every wake —
       `sleep: target`, `space:`, `deep sleep` — never left the board (rete#379 bench note,
       rete#233 T10: the channel count is one of them). Until rete#415 that wait was a
       fixed 150 ms; a profile window cut by this sleep now queues its whole dump
       (~10 KB) just before it, so the wait is a drain: until the ring is empty, at most
       2 s, and nothing at all when no target is set. The 2 s is a guess at a Thread
       node's worst case, to be replaced by the bell's measured drain time (#415 Task 8). */
    log_udp_drain(2000);
    esp_deep_sleep_start();   /* does not return */
#else
    (void)seconds;
#endif
}

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

#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
#include <zephyr/drivers/dac.h>
/* THE LEGACY IDF HEADER IS NOT AN ACCIDENT. Zephyr's DAC API cannot produce a
   tone at all: drivers/dac/dac_esp32.c exposes exactly channel_setup +
   write_value, and write_value sets ONE 8-bit DC level per call — no buffer, no
   DMA, no continuous mode. The ESP32-S2's hardware cosine generator is reachable
   only through this deprecated header, whose implementation
   (components/driver/deprecated/dac_common_legacy.c) is compiled under
   CONFIG_DAC_ESP32 and whose directory is on the global include path
   (hal/espressif/zephyr/esp32s2/CMakeLists.txt:12,64,209-215). So the PIN is
   enabled through the Zephyr API and the TONE is driven through the legacy one. */
#include "driver/dac.h"
/* THE LOW-LEVEL HEADER IS THE ECHO FIX (tuvm#94 stage 1), not a shortcut. The
   envelope has to change ONE register field — dac_scale1 — and the legacy
   driver's only route to it is dac_cw_generator_config(), which also rewrites the
   frequency divider and RE-STAMPS THE PHASE. Doing that mid-tone is a phase
   discontinuity, i.e. an audible transient, three times per strike; it also
   re-measures the uncalibrated RTC-8M oscillator each call, so the pitch
   micro-shifts too. Both were heard on the bench as an echo and initially assumed
   impossible. dac_ll_cw_set_atten() touches SENS.sar_dac_ctrl2.dac_scale1 and
   nothing else.
   Going under the driver skips its mutex. That is safe HERE and only here: this
   TU is the sole DAC user in the image, and the strike runs on the VM's single
   cooperative thread — there is no second caller to race with. The header is on
   the global include path (hal/espressif/zephyr/esp32s2/CMakeLists.txt:45). */
#include "hal/dac_ll.h"
/* MagTag bell (tuvm#94). The cosine generator on GPIO17 — same pin, same amp as
   the #86 LEDC chime, but reaching what LEDC cannot: 130 Hz-55 kHz (vs LEDC's
   ~1 kHz floor on this part) and four amplitude steps, which is what makes a
   decay envelope possible at all.

   GATE 1 WAS A SWEEP, NOT A NOTE, AND THE SWEEP WAS RIGHT TO BE ONE. The stated
   risk was that a sine would be QUIETER than the square wave #86 played, since a
   square carries far more harmonic energy into a tiny buzzer. Measured
   2026-07-23, the risk was BACKWARDS: 220 and 440 are the good tones and 1760
   sounded bad. A single frequency would have produced a confident wrong
   conclusion either way — the same reason sweeping is what located LEDC's floor
   in #86. */
static const struct gpio_dt_spec bell_en = GPIO_DT_SPEC_GET(DT_ALIAS(spkren), gpios);

/* Set by bell_bringup once the pin is enabled; read by bell_target_available.
   Declared inside the guard, unlike disp_ok — every reference to it is inside the
   same guard, and a file-scope bool on the boards with no speaker would be an
   unused static. */
static bool bell_ok;

/* The bell's fundamental. 440 Hz beat 220 on the bench (gate 2), and it is BELOW
   the ~1 kHz LEDC floor measured in #86 — so this is a pitch the PWM path could
   not have produced at all. */
#define BELL_HZ 440

/* ONE STRIKE. The cosine generator is a pure oscillator with no envelope of its
   own, so what makes this a strike rather than a beep is the four-step amplitude
   ramp — DAC_CW_SCALE_1/2/4/8 is 1, 1/2, 1/4, 1/8 of full scale, i.e. -6 dB a
   step. Four steps is ALL the hardware has; a real bell's continuous exponential
   decay (and its inharmonic partials) needs per-sample synthesis, which is the
   decision this gate exists to inform.

   The steps LENGTHEN as they quieten, which is what an exponential decay does and
   what makes the tail read as ring-out rather than as a staircase. Total 500 ms —
   #86 measured 160 ms as inaudible at rc=0 and 500 ms as loud on this buzzer. */
static void bell_strike(uint32_t hz) {
    /* The attenuation names come from the HAL (dac_cosine_atten_t), not from the
       legacy DAC_CW_SCALE_* aliases, because dac_ll_cw_set_atten takes that type.
       The two enums are numerically identical (0-3) — this is a typing choice, not
       a behaviour change. */
    static const struct { dac_cosine_atten_t atten; int ms; } envelope[] = {
        { DAC_COSINE_ATTEN_DB_0,   50 },   /* attack: full amplitude, short */
        { DAC_COSINE_ATTEN_DB_6,   90 },
        { DAC_COSINE_ATTEN_DB_12, 140 },
        { DAC_COSINE_ATTEN_DB_18, 220 },   /* tail: quietest, longest */
    };

    /* CONFIGURE ONCE, THEN STEP ATTENUATION ONLY. The frequency, phase and offset
       are set here and never touched again for the life of the strike — that is
       the whole of the echo fix (see the hal/dac_ll.h note above). config()
       brackets the RTC-8M clock with enable/disable to read its frequency, but
       that is refcounted and generator_enable holds a reference, so the clock does
       not drop out mid-strike. */
    dac_cw_config_t cw = {
        .en_ch  = DAC_CHAN_0,
        .scale  = DAC_CW_SCALE_1,
        .phase  = DAC_CW_PHASE_0,
        .freq   = hz,
        .offset = 0,
    };
    (void)dac_cw_generator_config(&cw);
    (void)dac_cw_generator_enable();

    for (size_t i = 0; i < ARRAY_SIZE(envelope); i++) {
        dac_ll_cw_set_atten(DAC_CHAN_0, envelope[i].atten);
        k_msleep(envelope[i].ms);
    }

    /* SILENCE BETWEEN STRIKES HAS TO COME FROM DISABLING THE GENERATOR. The
       quietest step is 1/8 scale, not zero, so leaving it running would leave a
       continuous tone under the whole pattern and destroy the phrasing. */
    (void)dac_cw_generator_disable();
}

/* N BELLS, STRUCK IN PAIRS — the ship's-bell convention: two quick strikes, a
   gap, two more, with a LONE FINAL STROKE for an odd count. Five bells is
   ding-ding … ding-ding … ding, and it is the most informative count to demo
   because it exercises both the pairing and the odd remainder.

   The gap after a strike depends on its position: even index = first of a pair,
   so a short gap holds the pair together; odd index = second of a pair, so a long
   gap separates it from the next. No trailing gap after the final stroke. */
static void bell_strike_pattern(uint32_t hz, int n) {
    printk("bell: %u Hz x%d\n", hz, n);
    for (int i = 0; i < n; i++) {
        bell_strike(hz);
        if (i == n - 1) {
            break;
        }
        k_msleep((i % 2 == 0) ? 160 : 600);
    }
}

/* tuvm#94 stage 1: BRING-UP ONLY — it no longer strikes anything. Gates 1 and 2
   were a boot-time demo (sweep, then three times of day); now that nsl drives the
   bell through PRIM_pf_bellStrike, a boot demo would be ~20 s of bells before the
   node gets a word in, and worse, it would make every reset sound like a working
   node whether or not the seam works. What is left here is exactly the state the
   strike cannot re-derive per call: the pin enable and the availability flag.
   The amp is NOT un-muted here — that is per-strike, below. */
static void bell_bringup(void) {
    /* THE ZEPHYR DAC DEVICE IS DELIBERATELY NOT REQUIRED, AND ITS INIT FAILS.
       drivers/dac/dac_esp32.c:71 does `if (clock_control_on(...) != 0) return
       -EIO`, but clock_control_esp32.c:506 returns -EALREADY when the module
       clock is ALREADY ON — a success condition. So dac_esp32_init reports -EIO,
       the device never becomes ready, and every failure message is swallowed
       because it goes through LOG_ERR while this build has only CONFIG_PRINTK.
       Note drivers/adc/adc_esp32.c:668 calls the very same clock_control_on for
       the very same module (both DAC and ADC hang off ESP32_PERIPH_SARADC_MODULE)
       and DISCARDS the return value — i.e. the ADC driver already tolerates what
       the DAC driver dies on. Upstream bug; not patched locally because this
       Zephyr checkout carries the C6 radio fix and must not accrete casual
       patches.

       It costs nothing to route around: the tone must come from the legacy API
       regardless (Zephyr's DAC API cannot generate one), so taking the pin enable
       from the same place removes a dependency rather than adding a workaround.
       CONFIG_DAC_ESP32 is still required — it is what compiles
       dac_common_legacy.c — hence the devicetree node stays enabled. */
    if (!gpio_is_ready_dt(&bell_en)) {
        printk("bell: speaker-enable pin not ready\n");
        return;
    }

    /* CHANNEL 0, AND THE NAME IS A TRAP. GPIO17 is DAC_CHAN_0
       (soc/esp32s2/include/soc/dac_channel.h: DAC_GPIO17_CHANNEL == DAC_CHAN_0).
       The legacy alias DAC_CHANNEL_1 is deprecated and its VALUE IS 0 — the name
       says "1" and means the first channel. Writing 1 here would silently target
       GPIO18, which goes nowhere on this board: rc=0 and no sound. */
    int rc = dac_output_enable(DAC_CHAN_0);
    printk("bell: dac_output_enable rc=%d\n", rc);
    if (rc) {
        return;   /* never discard an error code on a bring-up path */
    }

    /* Leave the amp MUTED. It is un-muted per strike (bell_target_strike), so a
       node that never rings leaves no idle hiss on the speaker and, more to the
       point, so the 50 ms settle below is paid where it is actually needed. */
    gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_INACTIVE);
    bell_ok = true;
    printk("BELL OK %u Hz\n", (unsigned)BELL_HZ);
}
#endif /* CONFIG_DAC && spkren */

/* tuvm#94 stage 1: the device half of the bell seam. The host half is in
 * src/platform.c under #ifndef __ZEPHYR__. Both symbols must exist and link on
 * EVERY Zephyr board, including the ones with no DAC and no speaker (ESP32, C6,
 * nRF52840-DK/dongle) — src/platform.c declares them extern unconditionally under
 * __ZEPHYR__. So the guard goes INSIDE each body, exactly as the display five do,
 * never around the definitions. On a board with no bell, available() answers 0
 * and strike() is unreachable in practice but still reports a fault rather than
 * silently claiming to have rung. */
int bell_target_available(void)
{
#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
	return bell_ok ? 1 : 0;
#else
	return 0;   /* no DAC or no speaker-enable pin on this board */
#endif
}

int bell_target_strike(int bells)
{
#if defined(CONFIG_DAC) && DT_NODE_EXISTS(DT_ALIAS(spkren))
	if (!bell_ok) return 2;

	/* THE AMP NEEDS TO SETTLE — the single biggest time sink in #86, where
	 * enabling SPEAKER_SHUTDOWN and driving in the same breath was silent
	 * and looked like a routing bug for three builds. Per strike now, not
	 * once at boot: 50 ms against a ~500 ms strike is noise, and holding the
	 * amp live between strikings buys nothing.
	 *
	 * 440 Hz, SETTLED ON THE BENCH 2026-07-23. Gate 1 swept 220/440/880/1760:
	 * all four audible, 1760 bad. Gate 2 struck the pattern at 440 and 220
	 * back to back and 440 was "far superior". 440 is BELOW LEDC's floor on
	 * this part (#86: 500 Hz -> -EINVAL), so the pitch this bell wants is one
	 * PWM could never have produced — the rung's whole justification. */
	gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_ACTIVE);
	k_msleep(50);
	bell_strike_pattern(BELL_HZ, bells);
	gpio_pin_configure_dt(&bell_en, GPIO_OUTPUT_INACTIVE);   /* re-mute */
	return 0;
#else
	ARG_UNUSED(bells);
	return 2;   /* unreachable in practice: available() already gated this */
#endif
}

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
/* AS EARLY AS THE OS ALLOWS, because a declared output is a load that is OFF only
 * while something drives it that way (2026-07-31, shipsBellsV3 bench).
 *
 * Calling this from main() — where it used to live, and still is, harmlessly — is
 * far too late for anything that can be damaged by being left on. The 12 V strike
 * driver AS WIRED IN JULY 2026 had no base pull-down on its level-shifter, so its
 * coil was energised for every microsecond GPIO18 was not actively held low;
 * main() runs after ALL init levels, after kernel_boot() and after
 * oimg_load_blob(), which on this board is hundreds of milliseconds of coil.
 * (That rig is gone — the 12 V solenoid was retired for a 3.3 V piezo, and that
 * driver DOES have a base pull-down. Read the paragraph below before acting on
 * this one: the two rigs have been conflated twice and it cost a diagnosis both
 * times.)
 *
 * PRE_KERNEL_1 at priority 50: CONFIG_GPIO_INIT_PRIORITY defaults to
 * KERNEL_INIT_PRIORITY_DEFAULT (40, drivers/gpio/Kconfig:48), so 50 is the first
 * slot at which gpio_pin_configure_dt() is legal — the controller exists and
 * nothing else has run.
 *
 * THIS IS A MITIGATION, NOT A FIX, and the distinction matters enough to write
 * down. It cannot help during esptool's download mode, the ROM bootloader, a
 * brown-out, or a CPU that never reaches init at all — and the ROM downloader is
 * not a window at all, it is a STABLE state that waits for a host forever
 * (rete#201: two minutes of continuous bell at 11pm on 2026-08-11).
 *
 * A pull-down at the transistor is the right shape of answer, and IT IS ALREADY
 * FITTED — R2, 47k base-to-GND, on the 3.3 V piezo driver since 2026-08-05. It
 * closes the reset case and it did NOT close the download-mode case, so do not
 * write "only a pull-down fixes this" here again. With a genuinely high-Z pin R2
 * holds the base at 0 V; the bell rang anyway, so in download mode GPIO18 is
 * either driven high or weakly pulled up — and against a ~45k pull-up R1/R2
 * (10k/47k) leave the base at ~1.5 V, hard on. Unresolved, rete#105/#201; the
 * settling measurement is a meter on GPIO18 with the board parked in
 * boot:0x66 DOWNLOAD. See zephyr/boards/shipsBells/2026-07-26-shipsbellsv3-design.md §8:
 * "Never let a crashed VM be the only thing between a GPIO and a burnt coil."
 */
static int gpio_outputs_early_init(void) {
    wire_gpio_outputs();
    return 0;
}
SYS_INIT(gpio_outputs_early_init, PRE_KERNEL_1, 50);
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

extern void (*gc_additional_roots_hook)(void);
extern uint64_t (*oevm_millis)(void);

/* Baked by cmake/gen_build_id.cmake into a generated TU, regenerated every
   build (#56 §5.4). A rodata literal, so `strings zephyr.elf` recovers it —
   which is what makes the ELF archive repairable after the fact. */
extern const char oevm_build_id[];

static void oevm_roots_callback(void) {
    /* Standard GC roots (vm_stack, current_frame, g_active_stack) are scanned
       automatically by gc.c; this gate needs no additional roots. */
}

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
   Started by the kernel after all init levels, so the SYS_INIT above has already
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

    gc_additional_roots_hook = oevm_roots_callback;
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
    oevm_thread_bringup();   /* shared OT bring-up seam (src/thread_bringup.c) */
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
