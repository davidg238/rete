/* The clock leaves of the board (rete#440 R6a): the NTP seam and the wall-clock RTC.
   Moved verbatim from zephyr/src/main.c; the behaviour is unchanged. */
#include <zephyr/kernel.h>
#include <stdint.h>
#include <string.h>
#include "target_backend.h"

#ifdef CONFIG_SNTP
#include <zephyr/net/sntp.h>        /* tuvm#94 stage 4: sntp_simple for the bells */
#include <zephyr/net/net_ip.h>      /* net_addr_pton, sockaddr_in6 — the literal path */
#endif


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
