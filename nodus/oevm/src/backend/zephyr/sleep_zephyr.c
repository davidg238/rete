/* The sleep leaves of the board (rete#440 R6a): retained RTC fields, the reset cause,
   the DS3231 alarm and deep sleep. Moved verbatim from zephyr/src/main.c; the behaviour
   is unchanged. */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <string.h>
#include "space.h"        /* space_channel_count, OEVM_SPACE_MAX_CHANNELS */
#include "log_udp.h"      /* log_udp_drain */
#include "target_backend.h"

#if DT_NODE_EXISTS(DT_ALIAS(wallclock))
#include <zephyr/drivers/rtc.h>
#endif


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
