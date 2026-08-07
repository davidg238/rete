#include "platform.h"
#include "platform_backend.h"
#include "prim_ids.h"
#include "oimg.h"
#include "space.h"   /* Milestone C Task 1: channel-space C substrate */
#include "pulse_backend.h"
#include "timer_backend.h"   /* rung 8: the timer seam (virtual clock on host, k_timer on target) */
#include "handles.h"
#include "http.h"
#include "http_backend.h"
#include "sensor_backend.h"  /* AMG8833 thermal rung: the Tier-1 sensor seam */
#include "span.h"            /* tuvm#61: job-latency span slots */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* #74/#165 D3: the build actually running, so a profile reader can pick the
   matching .oimg.methods.json sidecar instead of choosing by hand — a
   mismatched sidecar resolves nothing and looks exactly like a quiet program.
   `tuvm_build_id` is generated fresh on every Zephyr build (git describe +
   a timestamp; see zephyr/cmake/gen_build_id.cmake) and already `extern`'d
   and printed at boot by zephyr/src/main.c. A host build has no such TU, so
   this is a #define split rather than a weak symbol — a weak symbol that
   silently resolved to the wrong definition would be exactly the failure
   mode this field exists to prevent. Not a GNU build-id: a PT_NOTE segment
   over DROM boots the C6 and then emits garbage on the console (#144). */
#ifdef __ZEPHYR__
extern const char tuvm_build_id[];
#define OEVM_BUILD_ID tuvm_build_id
#else
#define OEVM_BUILD_ID "host"
#endif

/* The host virtual-clock timer facility that used to live here was lifted in rung 8
   into the timer seam: src/timer_backend.h + src/backend/posix/timer_posix.c (the
   virtual clock, verbatim) and src/backend/zephyr/timer_zephyr.c (a real k_timer).
   The prims below (85-88 + advanceMillis:) now dispatch through that seam. */

/* Host GPIO mock: registration stores (pin, chan_id, handle_idx); the host-only
   gpioFire: prim simulates an edge by posting to the pulse mailbox — the exact
   (chan_id, handle_idx) an on-target GPIO ISR would post. Fixed table, -D override. */
#ifndef HOST_MAX_GPIO
#define HOST_MAX_GPIO 16
#endif
struct HostGpio { int pin; int chan_id; int handle_idx; int live; };
static struct HostGpio host_gpio[HOST_MAX_GPIO];
static int host_n_gpio = 0;

static void host_gpio_register(int pin, int chan_id, int handle_idx) {
    int i;
    for (i = 0; i < host_n_gpio; i++) {
        if (host_gpio[i].live && host_gpio[i].pin == pin) {   /* re-register same pin: replace */
            handle_free(host_gpio[i].handle_idx);   /* release the outgoing M6 pin — no slot leak */
            host_gpio[i].chan_id = chan_id; host_gpio[i].handle_idx = handle_idx; return;
        }
    }
    for (i = 0; i < host_n_gpio; i++) {
        if (!host_gpio[i].live) {
            host_gpio[i].pin = pin; host_gpio[i].chan_id = chan_id;
            host_gpio[i].handle_idx = handle_idx; host_gpio[i].live = 1; return;
        }
    }
    if (host_n_gpio < HOST_MAX_GPIO) {
        host_gpio[host_n_gpio].pin = pin; host_gpio[host_n_gpio].chan_id = chan_id;
        host_gpio[host_n_gpio].handle_idx = handle_idx; host_gpio[host_n_gpio].live = 1;
        host_n_gpio++;
    } else { fprintf(stderr, "host gpio table full\n"); abort(); }
}
static void host_gpio_fire(int pin) {
    int i;
    for (i = 0; i < host_n_gpio; i++)
        if (host_gpio[i].live && host_gpio[i].pin == pin)
            pulse_post_isr(host_gpio[i].chan_id, host_gpio[i].handle_idx);
}

/* tuvm#82: the host side of the OUTBOUND gpio seam. A pin counts as DECLARED on
   the host if it has been written by gpioSet:to: or registered by
   gpioPin:edge:post:on: — the host analogue of "declared in the board overlay".
   Deliberately a SEPARATE table from host_gpio above: firing an edge and driving
   a line are different things (the interesting chording test sets a level on one
   pin while firing an edge on another), and keeping them apart leaves the proven
   pulse path untouched.
   KNOWN HOST/DEVICE DIVERGENCE: on the host, gpioSet:to: CREATES a slot, so it
   never reports an undeclared pin; on target it fails for any pin the overlay
   omits. A typo'd output pin therefore passes on the host and raises on the
   board. Accepted: there is no overlay on the host to check against. The level
   read has no such gap and is where the raise gets its host coverage. */
#ifndef __ZEPHYR__
#ifndef HOST_MAX_GPIO_LEVEL
#define HOST_MAX_GPIO_LEVEL 16
#endif
struct HostGpioLevel { int pin; int level; int live; };
static struct HostGpioLevel host_gpio_level[HOST_MAX_GPIO_LEVEL];
static int host_n_gpio_level = 0;

static struct HostGpioLevel *host_level_find(int pin) {
    int i;
    for (i = 0; i < host_n_gpio_level; i++)
        if (host_gpio_level[i].live && host_gpio_level[i].pin == pin)
            return &host_gpio_level[i];
    return NULL;
}
static int host_gpio_is_registered(int pin) {
    int i;
    for (i = 0; i < host_n_gpio; i++)
        if (host_gpio[i].live && host_gpio[i].pin == pin) return 1;
    return 0;
}

/* Answers 1 on success, 0 for a pin that cannot be driven (host: table full). */
int gpio_target_set(int pin, int level) {
    struct HostGpioLevel *e = host_level_find(pin);
    if (e) { e->level = level ? 1 : 0; return 1; }
    if (host_n_gpio_level >= HOST_MAX_GPIO_LEVEL) return 0;
    e = &host_gpio_level[host_n_gpio_level++];
    e->pin = pin; e->level = level ? 1 : 0; e->live = 1;
    return 1;
}

/* Answers 0/1 for a declared pin and -1 for an undeclared one. A pin registered
   as a pulse input but never driven reads false — an input that nothing has
   asserted, which is what an idle active-low button looks like. */
int gpio_target_level(int pin) {
    struct HostGpioLevel *e = host_level_find(pin);
    if (e) return e->level;
    if (host_gpio_is_registered(pin)) return 0;
    return -1;
}
#else
/* tuvm#82: on target these live in zephyr/src/main.c, beside the devicetree tables
   they read. Conditional compilation in a shared src/*.c rather than a new backend
   .c pair is deliberate: __ZEPHYR__ is already used this way in src/gc.c and
   src/vm.c, and adding a .c file costs edits to nine enumerated source lists
   across two repos (tuvm#63) for two functions.
   Note the direction is the MIRROR of gpio_target_fire below, which platform.c
   defines and main.c calls inbound. */
extern int gpio_target_set(int pin, int level);
extern int gpio_target_level(int pin);
#endif

/* tuvm#88: the host side of the display seam — a real character grid, not a
   stub that only reports errors. `refresh` PRINTS the non-blank rows, so an nsl
   test asserts on the text that was actually laid down (the existing expect:
   mechanism is the read-back path, which is why no fifth "read the grid" prim
   has to be spent). On target these live in zephyr/src/main.c beside CFB.
   Geometry: the panel is 128x296 px and CFB is configured with font1016 (10 wide
   x 16 tall) — NOT 8x8; CFB ships no 8x8 font, and the `ppt` of 8 that looks like
   a font size is a tiling granularity, not one. That gives 128/10 = 12 columns
   and 296/16 = 18 rows. The numbers are named ONCE, in platform_backend.h
   (OEVM_DISPLAY_ROWS/COLS), and this file plus zephyr/src/main.c both consume
   that single definition — a host/device mismatch here would make a host-green
   test meaningless, so the device side hard-checks its derived grid against
   these same constants at bring-up rather than merely printing them. */
#ifndef __ZEPHYR__
#define HOST_DISPLAY_ROWS OEVM_DISPLAY_ROWS
#define HOST_DISPLAY_COLS OEVM_DISPLAY_COLS
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

/* tuvm#88 TEST SEAM: OEVM_NO_DISPLAY=1 makes the host claim "no display", the
   same shape as OEVM_GC_STRESS in gc.c — cached once so getenv is not re-read
   per call. Without this, `display_target_available()` was hard-coded 1 and the
   design's promised "no display -> #displayUnavailable" host test could not
   exist; a board really can lack the panel (Finding 3), so the host needs a way
   to exercise that path too. */
int display_target_available(void) {
    static int no_display_cached = -1;
    if (no_display_cached < 0)
        no_display_cached = (getenv("OEVM_NO_DISPLAY") != NULL) ? 1 : 0;
    if (no_display_cached) return 0;
    host_display_ensure();
    return 1;
}

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

/* tuvm#94 stage 1: the host side of the bell seam. There is nothing to synthesise
   here — the host has no cosine generator and no speaker — so what the host owes
   an nsl test is the ONE thing it can honestly report: that the seam was reached
   with the count the node meant. It prints, exactly as display_target_refresh
   prints its grid, and for the same reason: the existing expect: mechanism is the
   read-back path, so no second "how many bells did you strike" prim has to be
   spent.
   Deliberately NOT sleeping the ~500 ms a strike takes on target. A host test
   would then cost four seconds to assert on a printed line, and the blocking is a
   property of the hardware envelope, not of the seam — see the note on
   PRIM_pf_bellStrike in prim_ids.h. */
#ifndef __ZEPHYR__
/* Same shape as OEVM_NO_DISPLAY above: OEVM_NO_BELL=1 makes the host claim the
   board has no bell, so the design's promised "no bell -> #bellUnavailable" path
   is testable. Without it available() is hard-coded 1 and the structural-error
   branch is unreachable — a board really can lack the speaker (every non-MagTag
   target in this tree does). Cached once so getenv is not re-read per strike. */
int bell_target_available(void) {
    static int no_bell_cached = -1;
    if (no_bell_cached < 0)
        no_bell_cached = (getenv("OEVM_NO_BELL") != NULL) ? 1 : 0;
    return no_bell_cached ? 0 : 1;
}

int bell_target_strike(int bells) {
    char line[48];
    int len = snprintf(line, sizeof line, "bell: struck %d\n", bells);
    if (len > 0) platform_console_write(line, (size_t)len);
    return 0;
}

/* tuvm#94 stage 4: the host side of the NTP seam. The host has no clock to sync,
   so what it offers instead is DETERMINISM — OEVM_NTP_SECONDS=N makes it answer N,
   which is what lets a suite test drive the catchup arithmetic to an exact expected
   value instead of against wall time. The env value is taken as the POST-offset
   answer (seconds since local midnight), so `tz_offset_seconds` is deliberately
   ignored here: on target the offset is applied to a real epoch, while on host the
   test states the local time it wants directly. OEVM_NO_NTP forces the failure path,
   the same shape as OEVM_NO_BELL / OEVM_NO_DISPLAY, because "the sync failed" is a
   path the node must handle and no host can produce naturally.
   Cached once so getenv is not re-read per sync. */
int ntp_target_sync(int tz_offset_seconds) {
    (void)tz_offset_seconds;   /* host: the env value is already the answer */
    static int mode = -1;      /* -1 unknown, 0 no-ntp, 1 has-seconds */
    static long secs = 0;
    if (mode < 0) {
        if (getenv("OEVM_NO_NTP") != NULL) { mode = 0; }
        else {
            const char *s = getenv("OEVM_NTP_SECONDS");
            if (s != NULL) { mode = 1; secs = atol(s); }
            else { mode = 0; }   /* default: no NTP configured on host */
        }
    }
    if (mode == 0) return -1;
    return (int)(((secs % 86400) + 86400) % 86400);
}

/* tuvm#100 (2026-08-01): the host side of `ntpDate`. Same determinism contract as
   ntp_target_sync above — OEVM_NTP_DATE=20260801 is what a suite test sets when it
   wants to drive the RTC-correction path to an exact expected value.
   Deliberately INDEPENDENT of OEVM_NTP_SECONDS rather than derived from it, so a
   test can set either alone — 20_bell/11 exercises the date-absent path by simply
   omitting this one. On the device the two always come from one response. */
int ntp_target_date(void) {
    static int mode = -1;      /* -1 unknown, 0 no-date, 1 has-date */
    static long date = 0;
    if (mode < 0) {
        const char *s = getenv("OEVM_NTP_DATE");
        if (s != NULL) { mode = 1; date = atol(s); }
        else { mode = 0; }
    }
    if (mode == 0) return -1;
    return (int)date;
}

/* tuvm#100: the host side of the WALL-CLOCK seam. Same shape and same reasoning
   as ntp_target_sync above — the host has no RTC, so what it offers instead is
   DETERMINISM: OEVM_WALLCLOCK_SECONDS=N makes it answer N (seconds since local
   midnight), which is what lets a suite test drive schedule arithmetic to an
   exact expected value instead of against wall time.
   With the variable UNSET the answer is -1 (-> nil), which is the honest default:
   a host really has no DS3231 on it, and that is precisely the "no RTC on this
   board" path a node must handle. So unlike the bell and the display, this seam
   needs no OEVM_NO_* knob to reach its structural-failure branch — absence IS
   the default. Cached once so getenv is not re-read per reading. */
/* The host models a DATE as well as a time-of-day, even though nothing on the
   host reads a calendar. Two reasons, both about keeping the two sides honest:
   wallClockSetDate:time: takes a date because Zephyr's rtc_set_time does, so a
   host that dropped it would let a node pass garbage the device would reject; and
   the unset-clock guard is a YEAR comparison (zephyr/src/main.c), so modelling the
   year is what lets 04 exercise that guard without a DS3231 on the bench. */
#define OEVM_WALLCLOCK_MIN_YEAR 2020   /* keep in step with zephyr/src/main.c */
static int g_wc_set = 0;               /* 0 = never set — the cold, no-RTC default */
static int g_wc_secs = 0;
static int g_wc_year = 0;
static void wallclock_load_once(void) {
    static int loaded = 0;
    const char *s;
    if (loaded) return;
    loaded = 1;
    s = getenv("OEVM_WALLCLOCK_SECONDS");
    if (s == NULL) return;             /* default: no RTC on host */
    /* Setting the variable MEANS "this board has a clock and it has been set", so
       the year is stamped plausible here. Otherwise every existing test would have
       to name a date it does not care about just to get past the year guard, and
       the guard is not what those tests are about. */
    g_wc_set = 1;
    g_wc_secs = (int)(((atol(s) % 86400) + 86400) % 86400);
    g_wc_year = OEVM_WALLCLOCK_MIN_YEAR;
}
int wallclock_target_read(void) {
    wallclock_load_once();
    if (!g_wc_set) return -1;
    if (g_wc_year < OEVM_WALLCLOCK_MIN_YEAR) return -1;   /* mirrors the device guard */
    return g_wc_secs;
}
/* yyyymmdd is packed decimal (20260731). Only the year is inspected — the host has
   no calendar to keep, and the month and day are carried solely so the argument
   shape matches the device's, where they reach a real register. */
int wallclock_target_set(int yyyymmdd, int seconds_of_day) {
    wallclock_load_once();
    if (seconds_of_day < 0 || seconds_of_day > 86399) return -1;
    g_wc_year = yyyymmdd / 10000;
    g_wc_secs = seconds_of_day;
    g_wc_set = 1;
    return 0;
}

/* shipsBellsV3 (#102): the host side of the RTC ALARM seam. The host has no
   DS3231 and no interrupt line, so it REFUSES — and that is the useful host
   behaviour, not a gap: a node written against this seam has to carry the
   fallback that makes a missed alarm survivable, and refusing here is what
   exercises that path in the suite. There is nothing to simulate: the alarm's
   value is that the RTC rather than the MCU decides the instant, and a simulated
   alarm would be the MCU deciding. */
int rtc_alarm_arm(int seconds_of_day, int chan_id, int handle_idx) {
    (void)seconds_of_day; (void)chan_id; (void)handle_idx;
    return -1;
}

/* tuvm#94 stage 5: the host side of the RTC-retention seam. On device this struct
   lives in .rtc.data and survives deep sleep; on host it is a plain int32 array that
   optionally persists to OEVM_RTC_FILE so the node can be re-exec'd N times and the
   ratchet witnessed ACROSS simulated sleeps (Task 7's harness). With no file set the
   array is cold at process start (magic unwritten -> rtcValid false), which is
   exactly the cold-power-on path. A reusable test feature, not a one-off. */
#define OEVM_RTC_FIELDS 10
#define OEVM_RTC_MAGIC  0x14BE11C0u  /* must fit a 32-bit SMI (31-bit tag): the node
                                        writes it via rtcPut:, and a magnitude >= 2^30
                                        never round-trips on a 32-bit target */
static int32_t g_rtc[OEVM_RTC_FIELDS];
static int g_rtc_loaded = 0;
static void rtc_load_once(void) {
    if (g_rtc_loaded) return;
    g_rtc_loaded = 1;
    memset(g_rtc, 0, sizeof g_rtc);          /* cold default: magic mismatch -> invalid */
    const char *p = getenv("OEVM_RTC_FILE");
    if (p) {
        FILE *f = fopen(p, "rb");
        if (f) { (void)fread(g_rtc, sizeof(int32_t), OEVM_RTC_FIELDS, f); fclose(f); }
    }
}
static void rtc_store(void) {
    const char *p = getenv("OEVM_RTC_FILE");
    if (!p) return;                          /* in-memory only */
    FILE *f = fopen(p, "wb");
    if (f) { (void)fwrite(g_rtc, sizeof(int32_t), OEVM_RTC_FIELDS, f); fclose(f); }
}
int rtc_target_valid(void) { rtc_load_once(); return ((uint32_t)g_rtc[0] == OEVM_RTC_MAGIC) ? 1 : 0; }
int rtc_target_get(int i) { rtc_load_once(); if (i < 0 || i >= OEVM_RTC_FIELDS) return 0; return g_rtc[i]; }
void rtc_target_put(int i, int v) { rtc_load_once(); if (i < 0 || i >= OEVM_RTC_FIELDS) return; g_rtc[i] = v; rtc_store(); }

/* tuvm#94 stage 5: host deepSleep: — no real sleep exists, so log the commanded
   duration and exit(0). This makes a host node run TERMINATE (not hang) at its idle
   point, and — with OEVM_RTC_FILE set — leaves the retained state on disk for the
   next re-exec, which is how the ratchet is witnessed across simulated sleeps. */
void deep_sleep_target(int seconds) {
    char line[48];
    int len = snprintf(line, sizeof line, "sleep: arming %d s\n", seconds);
    if (len > 0) platform_console_write(line, (size_t)len);
    exit(0);
}
#else
/* On target these live in zephyr/src/main.c, beside the legacy cosine-generator
   calls and the devicetree nodes they read — same split, same reason as the
   display five above (tuvm#63). Like those, both symbols must exist and link on
   EVERY Zephyr board, including the ones with no speaker: the guard goes inside
   the function bodies, never around the definitions. */
extern int bell_target_available(void);
extern int bell_target_strike(int bells);
/* tuvm#94 stage 4. Same rule as the bell pair above: the symbol must link on EVERY
   Zephyr board, so main.c guards the BODY (a board without CONFIG_WIFI answers -1),
   never the definition. */
extern int ntp_target_sync(int tz_offset_seconds);
/* tuvm#100 (2026-08-01): the date half of the NTP seam. Same linkage rule; it
   answers -1 until a sync has succeeded, so a board with no network is already
   handled without a second guard. */
extern int ntp_target_date(void);
/* tuvm#94 stage 5: RTC-retention leaves live in zephyr/src/main.c (the .rtc.data
   struct is beside the wifi/ntp leaves). Same rule as bell/ntp — the symbols link on
   every Zephyr board; main.c guards bodies, never definitions. */
extern int rtc_target_valid(void);
extern int rtc_target_get(int i);
extern void rtc_target_put(int i, int v);
/* tuvm#94 stage 5: deepSleep: device leaf deferred to a later task (zephyr/src/main.c
   not yet touched); the extern must exist now so this shared translation unit links. */
extern void deep_sleep_target(int seconds);
/* tuvm#100: the wall-clock leaf lives in zephyr/src/main.c beside the other
   devicetree-reading leaves. Same rule as bell/ntp/rtc — the symbol must link on
   EVERY Zephyr board, so main.c guards the BODY (a board with no RTC node in its
   devicetree answers -1), never the definition. */
extern int wallclock_target_read(void);
extern int wallclock_target_set(int yyyymmdd, int seconds_of_day);
/* shipsBellsV3 (#102): the RTC alarm leaf, same rule as the wall-clock one above
   — zephyr/src/main.c guards the BODY (a board with no RTC alias, or an image
   built without CONFIG_RTC_ALARM, answers -1), never the definition, so the
   symbol links on every Zephyr board. */
extern int rtc_alarm_arm(int seconds_of_day, int chan_id, int handle_idx);
#endif

/* Target GPIO-ISR entry (Zephyr rung-2 Task 8): a real on-target GPIO edge
   callback (zephyr/src/main.c) calls this to post the pulse for `pin`, taking
   the exact same slot-lookup -> pulse_post_isr path the host-only gpioFire:
   prim simulates. ISR-safe: pulse_post_isr is non-allocating and non-blocking.
   Non-static so the Zephyr entry point can call it; unused on host/native_sim
   builds (host_gpio_fire serves the sim there), harmless when never called. */
void gpio_target_fire(int pin) { host_gpio_fire(pin); }

extern oop gc_allocate(oop vtable, size_t size);
extern oop StringVtable, SymbolVtable, VtableVtable;
extern oop RawBytesVtable;   /* Milestone B Task 3: RawBytes leaf storage */
extern oop LargeIntegerVtable, FloatVtable;   /* Milestone B Task 4: valueRank: */
extern const struct Symbol true_sym_obj, false_sym_obj;   /* match oevm.h forms */
#define true_symbol  ((oop)&true_sym_obj)
#define false_symbol ((oop)&false_sym_obj)

/* ---- moved from dispatch.c: std-lib step 1: real indexable representation --
   Layout mirrors struct ObjectInstance: slots[0] holds the element count as a
   tagged SMI (so the GC word-scan early-rejects it via IS_SMI), slots[1..n]
   hold elements (1-based). This is the capability-pure backing store that the
   kernel Array class (lib/kernel.nsl) wraps via basicAt:/basicSize. */
static struct Vtable IndexableVtable;

/* One-time init of the indexable vtable. Lazy (works on both boot paths). */
static int indexable_initialized = 0;
static void indexable_init(void) {
    if (indexable_initialized) return;
    IndexableVtable.base.vtable = VtableVtable;
    IndexableVtable.delegate = NULL;
    IndexableVtable.bindings = NULL;
    IndexableVtable.capacity = 0;
    IndexableVtable.size = 0;
    IndexableVtable.is_nested = 0;
    indexable_initialized = 1;
}

/* Allocate a fresh indexable of `n` nil-filled element slots. */
oop indexable_new(intptr_t n) {
    indexable_init();
    if (n < 0) n = 0;
    oop ix = gc_allocate((oop)&IndexableVtable,
                         sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
    struct ObjectInstance *inst = (struct ObjectInstance *)ix;
    inst->slots[0] = TAG_SMI(n);
    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
    return ix;
}

/* ---- moved from dispatch.c: std-lib step 1: the platform capability object -
   A single object whose messages ARE the genuinely-VM-level primitives
   (allocation / identity / I-O). Reached only by receiver identity, so it is
   capability-pure: code that was not handed this object cannot send to it.
   Lazily built so it is available on both boot paths (Model A solidified and
   Model B image), neither of which is guaranteed to call kernel_boot. The
   object + its vtable are static (BSS), not arena-allocated, so they need no
   GC-root registration; revisit only if platform is ever heap-allocated. */
static struct Object PlatformObj;
static struct Vtable PlatformVtable;
static int platform_initialized = 0;

oop platform_object(void) {
    if (!platform_initialized) {
        indexable_init();   /* platform array: depends on the indexable vtable */
        PlatformVtable.base.vtable = VtableVtable;
        PlatformVtable.delegate = NULL;
        PlatformVtable.bindings = NULL;
        PlatformVtable.capacity = 0;
        PlatformVtable.size = 0;
        PlatformVtable.is_nested = 0;
        PlatformObj.vtable = (oop)&PlatformVtable;
        platform_initialized = 1;
    }
    return (oop)&PlatformObj;
}

/* --- Rung 3 Task 3: IEEE-754 binary16 (half) helpers for cborEncodeFloat: ---
   File-local. Used only to test whether a double round-trips through an f16, so
   the encoder can pick the shortest of f16/f32/f64. NaN never reaches these
   (isValue rejects NaN before the prim is dispatched). */

/* Exact binary16 bit pattern -> double. Handles signed zero, subnormals,
   normals, and infinity (mant!=0 in the all-ones exponent yields NaN, which
   never occurs here). Bias 15, 10-bit mantissa. */
static double half_to_double(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp  = (h >> 10) & 0x1f;
    int mant = h & 0x3ff;
    double val;
    if (exp == 0) {
        val = ldexp((double)mant, -24);              /* subnormal (mant==0 -> 0.0) */
    } else if (exp == 0x1f) {
        val = (mant == 0) ? INFINITY : NAN;
    } else {
        val = ldexp((double)(mant + 1024), exp - 25); /* normal: (1.mant)*2^(exp-15) */
    }
    return sign ? -val : val;
}

/* double -> binary16 with round-to-nearest-even. The caller accepts the result
   only when half_to_double() round-trips the double bit-exactly, so for values
   that are NOT representable in binary16 any produced pattern is fine (it gets
   rejected by the exactness gate); representable values must map to their exact
   half. Handles sign, zero, infinity, subnormal/normal ranges and overflow.
   Always writes *out and returns 1. */
static int double_to_half(double d, uint16_t *out) {
    uint64_t u; memcpy(&u, &d, 8);
    uint16_t sign = (uint16_t)((u >> 48) & 0x8000);
    int exp = (int)((u >> 52) & 0x7ff);
    uint64_t mant = u & 0xfffffffffffffULL;      /* 52-bit fraction */

    if (exp == 0x7ff) {                          /* inf / NaN (NaN never reaches here) */
        *out = (uint16_t)(sign | 0x7c00 | (mant ? 0x200 : 0));
        return 1;
    }
    if (exp == 0 && mant == 0) { *out = sign; return 1; }   /* +/-0 */

    /* Build the 53-bit significand and unbiased leading-bit exponent e so that
       |d| = signif * 2^(e - 52), with bit 52 of signif being the leading 1. */
    int e;
    uint64_t signif;
    if (exp == 0) {                              /* subnormal double: no implicit bit */
        e = 1 - 1023;
        signif = mant;
        while (!(signif & (1ULL << 52))) { signif <<= 1; e--; }
    } else {
        e = exp - 1023;
        signif = mant | (1ULL << 52);
    }

    if (e > 15)  { *out = (uint16_t)(sign | 0x7c00); return 1; }  /* overflow -> inf */
    if (e < -24) { *out = sign; return 1; }                       /* underflow -> 0 */

    /* Drop `shift` low bits of the significand to land on the half grid,
       rounding to nearest-even. Normal half: E=e+15, keep 11 significant bits.
       Subnormal half: E field = 0, shift extra so the leading 1 falls into the
       10-bit mantissa. */
    int shift, E;
    if (e >= -14) { shift = 42;              E = e + 15; }  /* 52 - 10 */
    else          { shift = 42 + (-14 - e);  E = 0; }
    uint64_t keep = signif >> shift;
    uint64_t rem  = signif & ((1ULL << shift) - 1);
    uint64_t halfbit = 1ULL << (shift - 1);
    if (rem > halfbit || (rem == halfbit && (keep & 1))) keep++;

    uint16_t hbits;
    if (e >= -14) {                                            /* normal half */
        if (keep & (1ULL << 11)) { E++; keep >>= 1; }          /* rounding carried */
        if (E >= 31) { *out = (uint16_t)(sign | 0x7c00); return 1; }  /* -> inf */
        hbits = (uint16_t)(((unsigned)E << 10) | (uint16_t)(keep & 0x3ff));
    } else {
        /* subnormal: keep in [0, 2^10]; keep==2^10 becomes the smallest normal
           (E=1, mant=0), whose bit pattern is exactly 0x400. */
        hbits = (uint16_t)keep;
    }
    *out = (uint16_t)(sign | hbits);
    return 1;
}

int platform_dispatch(int pid, int argc, oop *args, oop *out) {
    (void)argc;
    switch (pid) {
    case PRIM_ix_array:
        { *out = indexable_new(IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0); return 1; }
    case PRIM_pf_identicalTo:
        { *out = (args[0] == args[1]) ? true_symbol : false_symbol; return 1; }
    case PRIM_pf_hashcode:
        if (IS_SMI(args[0])) { *out = args[0]; return 1; }
        if (args[0] == NULL) { *out = TAG_SMI(0); return 1; }
        { *out = TAG_SMI((intptr_t)(((uintptr_t)args[0]) >> 4)); return 1; }
    case PRIM_pf_print:
        if (args[0] == NULL) {
            platform_console_write("nil", 3);
        } else if (IS_SMI(args[0])) {
            char buf[32];
            int len = snprintf(buf, sizeof buf, "%ld", (long)UNTAG_SMI(args[0]));
            if (len > 0) platform_console_write(buf, (size_t)len);
        } else if (SymbolVtable != NULL &&
                   obj_vtable(args[0]) == SymbolVtable) {
            const char *nm = symbol_name((struct Symbol *)args[0]);
            platform_console_write(nm, strlen(nm));
        }
        { *out = platform_object(); return 1; }
    case PRIM_pf_printNewline:
        platform_console_write("\n", 1);
        platform_console_flush();
        { *out = platform_object(); return 1; }
    case PRIM_pf_basicNewIndexable: {
        /* `platform basicNew: <class> indexable: n` — capability-pure
           indexable allocation. args[0] is the class object: a BoundClass
           wrapper (slot0 = its nested vtable) or a bare (oop)&Vtable. The
           new instance is stamped with that vtable, slot0 = TAG_SMI(n)
           length header, elements 1..n nil. Replaces 3a's magic
           kernel_array_vtable `new:`. */
        oop cls = args[0];
        oop vt = boundclass_is(cls)
                 ? ((struct ObjectInstance *)cls)->slots[0]
                 : cls;
        intptr_t n = IS_SMI(args[1]) ? UNTAG_SMI(args[1]) : 0;
        if (n < 0) n = 0;
        oop a = gc_allocate(vt, sizeof(struct ObjectInstance) + ((size_t)n + 1) * sizeof(oop));
        struct ObjectInstance *inst = (struct ObjectInstance *)a;
        inst->slots[0] = TAG_SMI(n);
        for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
        { *out = a; return 1; }
    }
    case PRIM_pf_string: {
        /* `platform string: anArray` — build an immutable String from a
           Kernel indexable Array of byte-SMIs (0..255). The Toit
           bytes.to-string analog. Byte-granular: no UTF-8 validation
           (dialect #10/#12). GC is non-moving (cf. PRIM_comma), so reading
           `ix` after the single gc_allocate is safe. */
        oop arr = args[0];
        if (arr == NULL || IS_SMI(arr) ||
            !IS_SMI(instance_slot((struct ObjectInstance *)arr, 0))) {
            fprintf(stderr, "VM Error: platform string: expects an indexable Array\n");
            abort();
        }
        struct ObjectInstance *ix = (struct ObjectInstance *)arr;
        intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
        oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        for (intptr_t i = 1; i <= n; i++) {
            oop e = instance_slot(ix, (size_t)i);
            if (!IS_SMI(e)) {
                fprintf(stderr, "VM Error: platform string: element %ld is not a byte SMI\n", (long)i);
                abort();
            }
            intptr_t b = UNTAG_SMI(e);
            if (b < 0 || b > 255) {
                fprintf(stderr, "VM Error: platform string: byte %ld out of range 0..255\n", (long)b);
                abort();
            }
            ((struct String *)s)->bytes[i - 1] = (uint8_t)b;
        }
        { *out = s; return 1; }
    }
    case PRIM_pf_readLine: {
        /* Read one line from stdin (debug transport). Returns a kernel String
           with the newline stripped, or nil at EOF. Uses the same allocation
           idiom as PRIM_pf_string above (StringVtable / struct String). */
        char buf[512];
        int n = platform_debug_readline(buf, (int)sizeof buf);
        if (n < 0) { *out = NULL; return 1; }           /* EOF -> nil */
        oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        if (n) memcpy(((struct String *)s)->bytes, buf, (size_t)n);
        { *out = s; return 1; }
    }
    case PRIM_pf_basicNewBytes: {
        /* `platform basicNewBytes: n` — capability-pure raw byte storage
           allocation (Milestone B Task 3). Same allocation idiom as
           PRIM_pf_string (RawBytesVtable + struct String layout: length +
           inline bytes), but zero-filled rather than copied from an Array,
           and tagged with the DISTINCT RawBytesVtable so String never gains
           at:put: (dispatch.c gates that case on RawBytesVtable only). */
        intptr_t n = IS_SMI(args[0]) ? UNTAG_SMI(args[0]) : 0;
        if (n < 0) n = 0;
        oop s = gc_allocate(RawBytesVtable, sizeof(struct String) + (size_t)n);
        ((struct String *)s)->length = (size_t)n;
        if (n) memset(((struct String *)s)->bytes, 0, (size_t)n);
        { *out = s; return 1; }
    }
    case PRIM_pf_freezeBytes: {
        /* `platform freezeBytes: raw` — freeze-is-copy (message-algebra spec
           section 6): returns a FRESH RawBytes object with identical bytes;
           the original remains independently mutable (a later at:put: on
           either side must not be observed by the other). */
        oop raw = args[0];
        if (raw == NULL || IS_SMI(raw) || RawBytesVtable == NULL ||
            obj_vtable(raw) != RawBytesVtable) {
            fprintf(stderr, "VM Error: platform freezeBytes: expects a RawBytes receiver\n");
            abort();
        }
        struct String *src = (struct String *)raw;
        oop copy = gc_allocate(RawBytesVtable, sizeof(struct String) + src->length);
        ((struct String *)copy)->length = src->length;
        if (src->length) memcpy(((struct String *)copy)->bytes, src->bytes, src->length);
        { *out = copy; return 1; }
    }
    case PRIM_pf_valueRank: {
        /* `platform valueRank: v` — the C-side type oracle for the value
           algebra (Milestone B Task 4). SMI result: nil=0; Boolean=1;
           SMI/LargeInteger=2; Float=3 (NaN=-1); String=4; Symbol=6;
           RawBytes=-2 (internal, never a value); any other heap object=-1
           ("ask the object" — Bytes/List/Map/Record answer their own rank in
           nsl, Task 5). Boolean is tested by singleton identity BEFORE the
           generic SymbolVtable check so rank 1 wins over rank 6, matching
           dispatch.c's predicate fast-paths. */
        oop v = args[0];
        if (v == NULL) { *out = TAG_SMI(0); return 1; }                       /* nil */
        if (v == true_symbol || v == false_symbol) { *out = TAG_SMI(1); return 1; } /* Boolean */
        if (IS_SMI(v)) { *out = TAG_SMI(2); return 1; }                       /* SmallInteger */
        if (LargeIntegerVtable != NULL && obj_vtable(v) == LargeIntegerVtable) { *out = TAG_SMI(2); return 1; }
        if (FloatVtable != NULL && obj_vtable(v) == FloatVtable) {
            double d = ((struct Float *)v)->value;
            *out = (d != d) ? TAG_SMI(-1) : TAG_SMI(3);                      /* NaN -> -1 */
            return 1;
        }
        if (StringVtable != NULL && obj_vtable(v) == StringVtable) { *out = TAG_SMI(4); return 1; }
        if (SymbolVtable != NULL && obj_vtable(v) == SymbolVtable) { *out = TAG_SMI(6); return 1; } /* genuine Symbol */
        if (RawBytesVtable != NULL && obj_vtable(v) == RawBytesVtable) { *out = TAG_SMI(-2); return 1; }
        *out = TAG_SMI(-1);   /* any other heap object: ask it in nsl */
        return 1;
    }
    /* --- Milestone C Task 1: channel-space C substrate (src/space.c). Path
       arguments arrive as `struct String*`, StringVtable-gated the same way
       PRIM_pf_freezeBytes gates its RawBytes receiver above; nsl owns the
       verbs' own validation (value-check, A1 prefix confinement) before it
       ever reaches here (design doc section 6). --- */
    case PRIM_pf_spacePost: {
        /* `platform spacePost: path value: v` — the C-callable enqueue seam
           (design doc section 6.1's ISR-pulse proof). Answers args[1]
           unchanged (mirrors `out:` answering something useful to chain on;
           the capability-answering contract in the verb surface is nsl's). */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spacePost:value: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        space_post((const char *)p->bytes, p->length, args[1]);
        { *out = args[1]; return 1; }
    }
    case PRIM_pf_spaceRegister: {
        /* `platform spaceRegister: path block: aBlock owner: cap` — append a
           standing reaction, owner-tagged (design doc section 6 point 2).
           Answers the platform capability (no other natural receiver-ish
           answer exists here; mirrors PRIM_pf_print's convention). */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceRegister:block:owner: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        space_register((const char *)p->bytes, p->length, args[1], args[2]);
        { *out = platform_object(); return 1; }
    }
    case PRIM_pf_spaceTake: {
        /* `platform spaceTake: path` — remove-if-present-now. Answers a 1-slot
           Array{value} when an event was pulled (the value MAY be nil), or nil
           when the channel is empty — the Array-or-NULL disambiguation nsl's
           take:ifEmpty: unwraps (tuvm#21), the same shape spaceDequeue uses.
           The wrap allocates HERE (thread context), keeping space_take itself
           non-allocating. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceTake: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int found = 0;
        oop v = space_take((const char *)p->bytes, p->length, &found);
        if (!found) { *out = NULL; return 1; }              /* empty stream -> nil */
        oop wrap = indexable_new(1);                         /* Array{value} */
        ((struct ObjectInstance *)wrap)->slots[1] = v;       /* v rooted: no alloc between read and store */
        *out = wrap;
        return 1;
    }
    case PRIM_pf_spaceKind: {
        /* Rung 9 — `platform spaceKind: path` -> SMI 0 unset / 1 stream / 2 cell.
           Does NOT intern the path (asking about a name must not create it — the
           rule space_take already obeys). This is the predicate Channel>>peek:
           ifAbsent: and put:value: guard on, so it is on a control loop's hot
           path: no allocation, an SMI. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceKind: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        { *out = TAG_SMI(space_kind((const char *)p->bytes, p->length)); return 1; }
    }
    case PRIM_pf_spacePeek: {
        /* Rung 9 — `platform spacePeek: path` — the cell's value, NON-destructively
           (unlike spaceTake:, which removes). nil if the path is not a cell; the
           caller has already asked spaceKind:, so nil here means the cell holds
           nil, not that it is absent. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spacePeek: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        { *out = space_peek((const char *)p->bytes, p->length); return 1; }
    }
    case PRIM_pf_spacePut: {
        /* Rung 9 — `platform spacePut: path value: v` — write the cell's slot, then
           fan out through the EXISTING ring so `whenever:` reactions fire (space.c).
           Answers args[1] unchanged, exactly as spacePost:value: does. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spacePut:value: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        space_put((const char *)p->bytes, p->length, args[1]);
        { *out = args[1]; return 1; }
    }
    case PRIM_pf_spaceOverruns: {
        /* tuvm#20 — `platform spaceOverruns: path` -> SMI count of posts shed
           by the full event ring on that exact channel; '' -> the node total.
           Does NOT intern (space_take's rule); a never-interned path answers
           0, which is also the true count. Pull-side observability of the G2
           overflow policy (heapStats precedent); the push side — the periodic
           sys/metrics record — is tuvm#24. */
        oop path = args[0];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceOverruns: expects a String path\n");
            abort();
        }
        struct String *p = (struct String *)path;
        *out = TAG_SMI((intptr_t)space_overruns((const char *)p->bytes, p->length));
        return 1;
    }
    case PRIM_pf_spaceSeq: {
        /* porta space-sync — `platform spaceSeq` -> the space's ONE monotonic
           counter (design D5). No args, no allocation. */
        *out = TAG_SMI((intptr_t)space_seq());
        return 1;
    }
    case PRIM_pf_spaceStamp: {
        /* porta space-sync — `platform spaceStamp` -> that counter, BUMPED.
           See prim_ids.h: reading instead of bumping would let two captured
           stream entries share a seq, which the gateway drops as a duplicate. */
        *out = TAG_SMI((intptr_t)space_stamp());
        return 1;
    }
    case PRIM_pf_spaceChangedSince: {
        /* porta space-sync — `platform spaceChangedSince: cursor under: prefix
           max: n`. args[0] = cursor SMI, args[1] = prefix String, args[2] = max
           SMI. Answers a raw indexable of 3-slot indexables
           {pathString, seqSMI, value}, ascending by seq — empty, never nil, so
           the nsl side loops over basicSize with no special case.

           GC-hazard discipline (space_dequeue's, and for the same reason — this
           collector is PRECISE, and every indexable_new/space_make_string_pub
           below can collect): the OUTER array is allocated first and pushed as a
           temp root, and each row is stored INTO it before that row's own path
           String is allocated. Nothing heap-shaped is ever held only in a C
           local across an allocation. The cell values need no pinning — the
           channel table roots them via space_gc_trace, which is the whole reason
           a cell's value survives the job that wrote it. */
        oop prefix = args[1];
        if (prefix == NULL || IS_SMI(prefix) || StringVtable == NULL ||
            obj_vtable(prefix) != StringVtable) {
            fprintf(stderr, "VM Error: platform spaceChangedSince:under:max: expects a String prefix\n");
            abort();
        }
        struct String *p = (struct String *)prefix;
        intptr_t want = UNTAG_SMI(args[2]);
        if (want < 0) want = 0;
        if (want > OEVM_SPACE_MAX_CHANNELS) want = OEVM_SPACE_MAX_CHANNELS;

        int ids[OEVM_SPACE_MAX_CHANNELS];
        int n = space_changed_since((uint32_t)UNTAG_SMI(args[0]),
                                    (const char *)p->bytes, p->length,
                                    ids, (int)want);

        extern void gc_temp_root_push(oop o);
        extern int gc_temp_roots_count;
        int saved = gc_temp_roots_count;

        oop rows = indexable_new(n);
        gc_temp_root_push(rows);
        for (int i = 0; i < n; i++) {
            oop row = indexable_new(3);
            ((struct ObjectInstance *)rows)->slots[i + 1] = row;   /* rooted now */
            size_t plen = 0;
            const char *path = space_path_of(ids[i], &plen);
            ((struct ObjectInstance *)row)->slots[1] = space_make_string_pub(path, plen);
            ((struct ObjectInstance *)row)->slots[2] =
                TAG_SMI((intptr_t)space_last_seq_of(ids[i]));
            ((struct ObjectInstance *)row)->slots[3] = space_value_of(ids[i]);
        }
        gc_temp_roots_count = saved;
        *out = rows;
        return 1;
    }
    case PRIM_pf_spaceDequeue: {
        /* `platform spaceDequeue` — no args; answers the {value,
           blocksSnapshot, channelPath} 3-slot Array (Milestone D) or nil
           (NULL) if the queue is empty. */
        { *out = space_dequeue(); return 1; }
    }
    case PRIM_pf_protectedFire: {
        /* `platform protectedFire: aBlock value: v` — Milestone D crash
           boundary. args[0] = block, args[1] = value. */
        *out = vm_protected_fire(args[0], args[1]);
        return 1;
    }
    case PRIM_pf_spaceDropReactionsOwnedBy: {
        /* `platform spaceDropReactionsOwnedBy: cap` — Milestone D restart
           sweep. Removes every reaction owned by `cap` across every
           channel, compacting in place. Answers the platform capability
           (mirrors PRIM_pf_spaceRegister's/PRIM_pf_print's convention). */
        space_drop_reactions_owned_by(args[0]);
        *out = platform_object();
        return 1;
    }
    /* --- Milestone D Task 6: the timer seam (backend-dispatched since rung 8 —
       src/timer_backend.h: a virtual clock on host, a real k_timer on target).
       args order mirrors the keyword selector left-to-right
       (spaceRegister:block:owner: convention above): `timerAfter: ms token: tok`
       -> args[0]=ms, args[1]=tok. answers the platform capability where there is
       no more natural answer (same convention as spaceRegister/spaceDropReactions-
       OwnedBy above); monotonicMillis/advanceMillis: answer their own SMI
       result instead. */
    case PRIM_pf_timerAfter:
        timer_backend_after(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerEvery:
        timer_backend_every(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerCancel:
        timer_backend_cancel(UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_monotonicMillis:
        *out = TAG_SMI((long)timer_backend_millis());
        return 1;
    case PRIM_pf_advanceMillis:
        *out = TAG_SMI(timer_backend_advance((uint64_t)UNTAG_SMI(args[0])));
        return 1;
    /* --- Zephyr rung-2 sub-project 1 Task 3: host GPIO prims + pumpEvents.
       gpioPin:edge:post:on: registers a host GPIO slot; args mirror the
       keyword order left-to-right: args[0]=pin, args[1]=edge, args[2]=msg,
       args[3]=chanPath (String). gpioFire: is host-only — it simulates an
       edge by posting straight to the pulse mailbox via the same
       (chan_id, handle_idx) an on-target GPIO ISR would post. pumpEvents
       drains the mailbox into the channel space, answering the fire count. */
    case PRIM_pf_gpioPin: {
        oop path = args[3];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform gpioPin:edge:post:on: expects a String channel\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[2]);                 /* pin the message oop via M6 */
        host_gpio_register((int)UNTAG_SMI(args[0]), chan_id, hidx);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_gpioFire:
        host_gpio_fire((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_cborEncodeInt: {
        /* Encode an Integer (SMI or LargeInteger) as CBOR major 0 (>=0) or major 1
           (<0), shortest form. Range is the ratified +/-(2^64-1): major 0 arg = mag,
           major 1 arg = mag-1 (value = -1-arg). Both args fit uint64. */
        oop x = args[0];
        int major; uint64_t arg;
        if (IS_SMI(x)) {
            intptr_t v = UNTAG_SMI(x);
            if (v >= 0) { major = 0; arg = (uint64_t)v; }
            else        { major = 1; arg = (uint64_t)(-(v + 1)); }   /* -1-arg = v */
        } else if (LargeIntegerVtable && obj_vtable(x) == LargeIntegerVtable) {
            struct LargeInteger *li = (struct LargeInteger *)x;
            if (li->sign >= 0) { major = 0; arg = li->mag; }
            else               { major = 1; arg = li->mag - 1ULL; }  /* mag>=1 */
        } else {
            fprintf(stderr, "VM Error: cborEncodeInt: non-integer receiver\n"); abort();
        }
        uint8_t buf[9]; size_t n;
        unsigned mt = (unsigned)major << 5;
        if (arg < 24)              { buf[0]=(uint8_t)(mt|arg); n=1; }
        else if (arg < 0x100ULL)   { buf[0]=(uint8_t)(mt|24); buf[1]=(uint8_t)arg; n=2; }
        else if (arg < 0x10000ULL) { buf[0]=(uint8_t)(mt|25); buf[1]=(uint8_t)(arg>>8); buf[2]=(uint8_t)arg; n=3; }
        else if (arg < 0x100000000ULL) { buf[0]=(uint8_t)(mt|26);
            for(int s=3;s>=0;s--) buf[4-s]=(uint8_t)(arg>>(8*s)); n=5; }
        else { buf[0]=(uint8_t)(mt|27);
            for(int s=7;s>=0;s--) buf[8-s]=(uint8_t)(arg>>(8*s)); n=9; }
        oop b = gc_allocate(RawBytesVtable, sizeof(struct String) + n);
        ((struct String*)b)->length = n; memcpy(((struct String*)b)->bytes, buf, n);
        *out = b; return 1;
    }
    case PRIM_pf_cborEncodeFloat: {
        /* Encode a Float as CBOR major type 7, the SHORTEST of f16/f32/f64 that
           round-trips the double exactly. GC-safety: read ->value into a local
           BEFORE gc_allocate; no heap oop is touched after the allocation. */
        double d = ((struct Float*)args[0])->value;
        uint8_t buf[9]; size_t n;
        float f = (float)d;
        uint16_t h; int half_ok = double_to_half(d, &h) && half_to_double(h) == d;
        if (half_ok) { buf[0]=0xf9; buf[1]=(uint8_t)(h>>8); buf[2]=(uint8_t)h; n=3; }
        else if ((double)f == d) { buf[0]=0xfa; uint32_t uu; memcpy(&uu,&f,4);
            for(int s=3;s>=0;s--) buf[4-s]=(uint8_t)(uu>>(8*s)); n=5; }
        else { buf[0]=0xfb; uint64_t uu; memcpy(&uu,&d,8);
            for(int s=7;s>=0;s--) buf[8-s]=(uint8_t)(uu>>(8*s)); n=9; }
        oop b = gc_allocate(RawBytesVtable, sizeof(struct String)+n);
        ((struct String*)b)->length=n; memcpy(((struct String*)b)->bytes,buf,n);
        *out=b; return 1;
    }
    case PRIM_pf_cborReadHead: {
        /* Rung 3 Task 4: parse the head of a CBOR item at args[1] in the byte
           buffer args[0]. args: (aBytes, indexSMI). Returns Array{major,arg,next}
           (a fresh 3-elt indexable) or TAG_SMI(-1) if the head or its argument
           runs past the buffer, or the additional-info is 28-31 (reserved /
           indefinite -> rejected). GC-safety: len + all consumed bytes are read
           into locals (arg/major/j) BEFORE any allocating call, so the raw
           `struct String*` is never dereferenced across indexable_new/
           largeint_alloc. */
        struct String *b = (struct String*)args[0];
        size_t len = b->length;
        size_t i = (size_t)UNTAG_SMI(args[1]);
        if (i >= len) { *out = TAG_SMI(-1); return 1; }
        unsigned ib = b->bytes[i]; unsigned major = ib >> 5, ai = ib & 0x1f;
        size_t j = i + 1; uint64_t arg;
        if (ai < 24) arg = ai;
        else if (ai == 24) { if (j+1 > len) { *out=TAG_SMI(-1); return 1; } arg = b->bytes[j]; j += 1; }
        else if (ai == 25) { if (j+2 > len) { *out=TAG_SMI(-1); return 1; } arg = ((uint64_t)b->bytes[j]<<8)|b->bytes[j+1]; j += 2; }
        else if (ai == 26) { if (j+4 > len) { *out=TAG_SMI(-1); return 1; } arg = 0; for (int k=0;k<4;k++) arg = (arg<<8)|b->bytes[j+k]; j += 4; }
        else if (ai == 27) { if (j+8 > len) { *out=TAG_SMI(-1); return 1; } arg = 0; for (int k=0;k<8;k++) arg = (arg<<8)|b->bytes[j+k]; j += 8; }
        else { *out = TAG_SMI(-1); return 1; }   /* ai 28-31: reserved/indefinite */
        /* From here on b (args[0]) may be stale — no further deref. The GC is
           non-moving mark-sweep and does NOT scan the C stack, so a heap oop held
           live across a LATER allocation must be pinned as a temp root first
           (mirrors src/udp.c). `argv` is a heap LargeInteger on the large-arg
           branch (arg > INTPTR_MAX>>1) and is held live across indexable_new(3);
           pin it before that allocation. TAG_SMI pushes are harmless. */
        extern void gc_temp_root_push(oop o);
        extern void gc_temp_roots_clear(void);
        oop argv = (arg <= (uint64_t)(INTPTR_MAX>>1)) ? TAG_SMI((intptr_t)arg)
                                                      : oevm_int_demote(largeint_alloc(+1, arg));
        gc_temp_root_push(argv);
        oop r = indexable_new(3);
        ((struct ObjectInstance*)r)->slots[1] = TAG_SMI((intptr_t)major);
        ((struct ObjectInstance*)r)->slots[2] = argv;
        ((struct ObjectInstance*)r)->slots[3] = TAG_SMI((intptr_t)j);
        gc_temp_roots_clear();
        *out = r; return 1;
    }
    case PRIM_pf_cborIntFromMajor: {
        /* Rung 3 Task 4: rebuild a CBOR integer from its major (0 or 1) and its
           already-decoded unsigned argument. args: (majorSMI, argInteger).
           Returns the Integer, or CBOR_OOR_SENTINEL for the single out-of-range
           value -2^64 (major 1, arg 2^64-1). readHead built argInteger with a
           +sign, so a LargeInteger arg's magnitude is the raw unsigned value.
           GC-safety: the incoming magnitude is copied into a local uint64 before
           the single allocating call. */
        intptr_t major = UNTAG_SMI(args[0]); oop av = args[1];
        uint64_t arg = IS_SMI(av) ? (uint64_t)UNTAG_SMI(av)
                                  : ((struct LargeInteger*)av)->mag;
        if (major == 0) { *out = (arg ? oevm_int_demote(largeint_alloc(+1, arg)) : TAG_SMI(0)); return 1; }
        /* major 1: value = -1-arg; arg==2^64-1 => -2^64, just out of range */
        if (arg == 18446744073709551615ULL) { *out = CBOR_OOR_SENTINEL; return 1; }
        *out = oevm_int_demote(largeint_alloc(-1, arg + 1ULL)); return 1;
    }
    case PRIM_pf_cborReadFloat: {
        /* Rung 3 Task 4: read an f16/f32/f64 at a 0xf9/0xfa/0xfb head byte.
           args: (aBytes, indexSMI). Returns Array{Float,next} or TAG_SMI(-1) if
           the value runs past the buffer / the head byte is not a float head.
           Reuses the file-local half_to_double (Task 3). GC-safety: the decoded
           double d + next-index j are in locals before float_alloc/indexable_new;
           b is not dereferenced after those allocating calls. */
        struct String *b = (struct String*)args[0];
        size_t len = b->length, i = (size_t)UNTAG_SMI(args[1]);
        if (i >= len) { *out = TAG_SMI(-1); return 1; }
        unsigned t = b->bytes[i]; size_t j = i + 1; double d;
        if (t == 0xf9) { if (j+2 > len) { *out=TAG_SMI(-1); return 1; } uint16_t h=(uint16_t)((b->bytes[j]<<8)|b->bytes[j+1]); d=half_to_double(h); j += 2; }
        else if (t == 0xfa) { if (j+4 > len) { *out=TAG_SMI(-1); return 1; } uint32_t u=0; for (int k=0;k<4;k++) u=(u<<8)|b->bytes[j+k]; float f; memcpy(&f,&u,4); d=f; j += 4; }
        else if (t == 0xfb) { if (j+8 > len) { *out=TAG_SMI(-1); return 1; } uint64_t u=0; for (int k=0;k<8;k++) u=(u<<8)|b->bytes[j+k]; memcpy(&d,&u,8); j += 8; }
        else { *out = TAG_SMI(-1); return 1; }
        /* b (args[0]) may be stale past this point — no further deref. `f` is a
           heap Float held live across indexable_new(2); the non-stack-scanning
           mark-sweep GC would sweep it, so pin it as a temp root first (mirrors
           src/udp.c). This fires on EVERY call, not just an edge case. */
        extern void gc_temp_root_push(oop o);
        extern void gc_temp_roots_clear(void);
        oop f = float_alloc(FloatVtable); ((struct Float*)f)->value = d;
        gc_temp_root_push(f);
        oop r = indexable_new(2);
        ((struct ObjectInstance*)r)->slots[1] = f;
        ((struct ObjectInstance*)r)->slots[2] = TAG_SMI((intptr_t)j);
        gc_temp_roots_clear();
        *out = r; return 1;
    }
    case PRIM_pf_utf8Valid: {
        /* Rung 3 Task 4: full UTF-8 well-formedness check on args[0]. Rejects
           overlong encodings, UTF-16 surrogates (U+D800..U+DFFF), code points
           > U+10FFFF, and truncated multi-byte sequences. Returns the #true /
           #false symbol. No allocation, so no GC-safety concern (b/p are used
           only within this case). */
        struct String *b = (struct String*)args[0];
        const uint8_t *p = b->bytes; size_t n = b->length, i = 0;
        while (i < n) { unsigned c = p[i];
            if (c < 0x80) i++;
            else if ((c>>5) == 0x6) { if (i+1>=n || (p[i+1]&0xc0)!=0x80 || c<0xc2) { *out=false_symbol; return 1; } i += 2; }
            else if ((c>>4) == 0xe) { if (i+2>=n || (p[i+1]&0xc0)!=0x80 || (p[i+2]&0xc0)!=0x80) { *out=false_symbol; return 1; }
                if (c==0xe0 && p[i+1]<0xa0) { *out=false_symbol; return 1; } if (c==0xed && p[i+1]>0x9f) { *out=false_symbol; return 1; } i += 3; }
            else if ((c>>3) == 0x1e) { if (i+3>=n || (p[i+1]&0xc0)!=0x80 || (p[i+2]&0xc0)!=0x80 || (p[i+3]&0xc0)!=0x80) { *out=false_symbol; return 1; }
                if (c==0xf0 && p[i+1]<0x90) { *out=false_symbol; return 1; } if (c>0xf4 || (c==0xf4 && p[i+1]>0x8f)) { *out=false_symbol; return 1; } i += 4; }
            else { *out=false_symbol; return 1; } }
        *out = true_symbol; return 1;
    }
    case PRIM_pf_asSymbol: {
        /* Rung 3 Task 6: `platform asSymbol: aString` — build a genuine
           (non-interned) Symbol from a String's bytes, for CBOR tag-39 decode
           (and transitively Record tags, which require `isSymbol`). Mirrors the
           dispatch.c asString/asSymbol Symbol-build idiom (separate malloc'd
           `name`, never freed — a known-minor matching that existing pattern).
           Symbol equality is VALUE-based (printString byteCompare), so this
           non-interned Symbol `=` the image-literal #foo, giving round-trip.
           GC-safety: the String receiver's length + bytes are copied into the
           malloc'd `nm` BEFORE the single gc_allocate(Symbol), so args[0] is
           never dereferenced across (or after) the allocation. */
        struct String *s = (struct String *)args[0];
        size_t len = s->length;
        char *nm = malloc(len + 1);
        if (len) memcpy(nm, s->bytes, len);
        nm[len] = 0;
        oop sym = gc_allocate(SymbolVtable, sizeof(struct Symbol));
        ((struct Symbol *)sym)->name = nm;
        ((struct Symbol *)sym)->length = len;
        *out = sym; return 1;
    }
    case PRIM_pf_heapStats: {
        /* Rung 4 Task 1: `platform heapStats` — snapshot GC heap telemetry
           (src/gc.c oevm_heap_stats) into a 5-slot Array of SMIs, in field order
           [heapSize, liveBytes, highWater, largestFreeBlock, gcCycles]. The nsl
           HeapStats class names them. Every field is < arena size (<= 48 KB on
           the 32-bit DK; the host default 16 MB arena still fits a 32-bit SMI),
           so each is SMI-representable on host and target alike. GC-safety: the
           whole struct is copied into the C local `st` BEFORE the single
           allocating call (indexable_new); the five stores are SMIs (immediates,
           no allocation), so no heap oop is ever held across an allocation and no
           gc_temp_root pin is required. args are ignored (0-arg selector). */
        struct OevmHeapStats st = oevm_heap_stats();
        oop r = indexable_new(5);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)st.heap_size);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)st.live_bytes);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)st.high_water);
        ((struct ObjectInstance *)r)->slots[4] = TAG_SMI((intptr_t)st.largest_free_block);
        ((struct ObjectInstance *)r)->slots[5] = TAG_SMI((intptr_t)st.gc_cycles);
        *out = r; return 1;
    }
    case PRIM_pf_profileStart: {
        /* #165 D1/D6: open a per-method capture window. No-op without the
           profiler built in — deliberately silent rather than an error, so one
           app image runs on both a profile build and a production build. The
           absence is visible where it matters: no `mprof:` lines ever appear. */
#ifdef OEVM_METHOD_PROFILE
        mprof_start();
#endif
        *out = platform_object(); return 1;
    }
    case PRIM_pf_profileStop: {
        /* Closes the window and dumps. The dump goes to stderr, which reaches
           the #56 collector because the console is TEED, not because anything
           here routes it. */
#ifdef OEVM_METHOD_PROFILE
        mprof_stop();
#endif
        *out = platform_object(); return 1;
    }
    case PRIM_pf_buildId: {
        /* #74: the node names its own build so a reader can pick the matching
           .oimg.methods.json sidecar. Unconditional — NOT part of the profiler,
           so it is not gated on OEVM_METHOD_PROFILE like the two cases above.
           Same String-build idiom as spaceChangedSince:under:max: above
           (space_make_string_pub copies bytes+len into a fresh String; no
           allocation happens before OEVM_BUILD_ID's bytes are read, so there is
           nothing to root). */
        *out = space_make_string_pub(OEVM_BUILD_ID, strlen(OEVM_BUILD_ID));
        return 1;
    }
    case PRIM_pf_sysStats: {
        /* tuvm#24: `platform sysStats` — snapshot handle/ring/overrun telemetry
           into a 4-slot Array of SMIs, in field order [handlesLive,
           handlesHighWater, queueHighWater, totalOverruns]. Same GC-safety
           shape as heapStats above: all four scalars are copied into C locals
           BEFORE the single allocating call (indexable_new); the stores are
           SMIs (immediates, no allocation), so no heap oop is held across an
           allocation and no gc_temp_root pin is required. Every value is far
           below SMI range on host and target alike (table/ring/counter
           magnitudes). args ignored (0-arg selector). */
        unsigned hl = handle_live_count();
        unsigned hh = handle_high_water();
        uint32_t qh = space_queue_high_water();
        uint32_t ov = space_overruns("", 0);
        /* tuvm#34: the ISR pulse seam's own shed/coalesce counters, widening this
           record from 4 fields to 6. The seam used to answer a full mailbox with
           k_oops, so there was nothing to report — now that it sheds, a silent shed
           would be strictly worse than the crash it replaced. pulseDrops rising is
           the alarm (the reactor is behind); pulseCoalesced rising is the mechanism
           working (a repeating source folding into one pending wakeup). */
        uint32_t pd = pulse_drops();
        uint32_t pc = pulse_coalesced();
        oop r = indexable_new(6);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)hl);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)hh);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)qh);
        ((struct ObjectInstance *)r)->slots[4] = TAG_SMI((intptr_t)ov);
        ((struct ObjectInstance *)r)->slots[5] = TAG_SMI((intptr_t)pd);
        ((struct ObjectInstance *)r)->slots[6] = TAG_SMI((intptr_t)pc);
        *out = r; return 1;
    }
    case PRIM_pf_pumpEvents: {
        http_poll();                            /* accept/read pending sockets -> request ring */
        /* With an HTTP listener active, pulse must be drained NON-BLOCKING: the
           reactor has to loop back to http_poll every tick, so it cannot sleep in
           pulse_pump's K_FOREVER (zephyr) waiting for a pulse event that never comes.
           A pure pulse/timer node (no listener) keeps the blocking pump and sleeps
           efficiently until its ISR fires. On posix both are non-blocking. (#46) */
        int n = http_is_listening() ? pulse_pump_nb() : pulse_pump();
        timer_backend_poll();                   /* wall-clock host mode: fire due timers here.
                                                   No-op under the default virtual clock and on
                                                   target, whose expiry ISR posts via pulse. */
        http_pump();                            /* drain http request ring at the safe point */
        *out = TAG_SMI(n);
        return 1;
    }
    case PRIM_pf_httpListen: {                 /* platform httpListen: port -> self (binds; http_poll accepts) */
        int port = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        http_listen(port);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpRespond: {                /* httpRespondId: id status: n body: s */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        int st = IS_SMI(args[1]) ? (int)UNTAG_SMI(args[1]) : 500;
        oop body = args[2];
        if (body == NULL || IS_SMI(body) || StringVtable == NULL ||
            obj_vtable(body) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpRespondId:status:body: expects a String body\n");
            abort();
        }
        struct String *b = (struct String *)body;
        http_respond(id, st, (const char *)b->bytes, (int)b->length);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpInject: {                 /* httpInject: method path: p  (test-seam, fd=-1) */
        oop m = args[0], p = args[1];
        if (m == NULL || IS_SMI(m) || obj_vtable(m) != StringVtable ||
            p == NULL || IS_SMI(p) || obj_vtable(p) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpInject:path: expects String args\n");
            abort();
        }
        struct String *ms = (struct String *)m, *ps = (struct String *)p;
        http_accept_request(-1, (const char *)ms->bytes, (int)ms->length,
                                (const char *)ps->bytes, (int)ps->length);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpBegin: {                  /* httpBeginId: id status: st */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        int st = IS_SMI(args[1]) ? (int)UNTAG_SMI(args[1]) : 500;
        http_begin(id, st);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpChunk: {                   /* httpChunkId: id data: s */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        oop data = args[1];
        if (data == NULL || IS_SMI(data) || StringVtable == NULL ||
            obj_vtable(data) != StringVtable) {
            fprintf(stderr, "VM Error: platform httpChunkId:data: expects a String\n");
            abort();
        }
        struct String *s = (struct String *)data;
        http_chunk(id, (const char *)s->bytes, (int)s->length);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_httpEnd: {                      /* httpEndId: id */
        int id = IS_SMI(args[0]) ? (int)UNTAG_SMI(args[0]) : 0;
        http_end(id);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_sensorFrame: {
        /* `platform sensorFrame` — one 8x8 thermal frame as a 64-slot Array of
           centi-degC SMIs, or nil when the device is not ready. GC-safety: the
           backend fills a C buffer BEFORE the single allocating call
           (indexable_new); the 64 stores are SMIs (immediates, no allocation), so
           no heap oop is ever held across an allocation and no gc_temp_root pin is
           required — the same shape as heapStats/sysStats above. args are ignored
           (0-arg selector). */
        int32_t px[OEVM_THERMAL_PIXELS];
        if (sensor_frame_read(px) != 0) { *out = NULL; return 1; }   /* not ready -> nil */
        oop r = indexable_new(OEVM_THERMAL_PIXELS);
        for (int i = 0; i < OEVM_THERMAL_PIXELS; i++)
            ((struct ObjectInstance *)r)->slots[i + 1] = TAG_SMI((intptr_t)px[i]);
        *out = r;
        return 1;
    }
    case PRIM_pf_sensorArmEvent: {
        /* sensorArmEventPost: msg on: 'path' threshold: centi — mirrors
           gpioPin:edge:post:on: exactly: the channel arrives as a String PATH
           (interned to a chan_id here, in thread context) and the message oop is
           pinned via an M6 handle, because an ISR must never touch the VM. The
           trigger handler then only calls pulse_post_isr(chan_id, handle_idx). */
        oop path = args[1];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform sensorArmEventPost:on:threshold: expects a String channel\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[0]);                 /* pin the message oop via M6 */
        int32_t thr = IS_SMI(args[2]) ? (int32_t)UNTAG_SMI(args[2]) : 0;
        sensor_arm_event(chan_id, hidx, thr);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_rtcAlarmAt: {
        /* rtcAlarmAt: secondsOfDay post: msg on: 'path' (#102). Same shape as
           sensorArmEventPost:on:threshold: above and for the same reason: the
           channel arrives as a String PATH, interned here in thread context, and
           the message oop is pinned via an M6 handle because the delivery side
           (the DS3231 driver's workqueue callback) must never touch the VM.
           Answers NIL when the alarm cannot be armed — no RTC on this board, or
           an image built without CONFIG_RTC_ALARM. nsl raises on the nil; the
           node catches it and falls back to its watchdog. */
        oop path = args[2];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform rtcAlarmAt:post:on: expects a String channel\n");
            abort();
        }
        if (!IS_SMI(args[0])) { *out = NULL; return 1; }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[1]);                 /* pin the message oop via M6 */
        if (rtc_alarm_arm((int)UNTAG_SMI(args[0]), chan_id, hidx) != 0) {
            *out = NULL;                                /* nil — could not arm */
            return 1;
        }
        *out = platform_object();
        return 1;
    }
    /* --- tuvm#61 job latency: the span seam. Slots are integers; the name table
       lives in nsl (kernel.nsl's Spans class), so C never learns what slot 3
       means — the timer-seam convention, and the reason no name strings land in
       the WROOM-32's DRAM. All four answer the platform capability except
       spanStats:, which answers its own snapshot (the heapStats/sysStats
       convention). A bad slot index is INERT rather than fatal (src/span.c
       guards), so a miscomputed index in nsl loses samples instead of corrupting
       memory — the right failure mode for telemetry. */
    case PRIM_pf_spanBegin:
        span_begin((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanEnd:
        span_end((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanRecord:
        /* args mirror the keyword selector left-to-right (the
           spaceRegister:block:owner: convention): spanRecord: slot us: n
           -> args[0]=slot, args[1]=microseconds. */
        span_record((int)UNTAG_SMI(args[0]), (uint32_t)UNTAG_SMI(args[1]));
        *out = platform_object();
        return 1;
    case PRIM_pf_spanReset:
        /* tuvm#72: nullary — every slot at once. Answers the platform capability
           like spanBegin:/spanEnd:, not a snapshot. The nsl-side failure counts
           are NOT reachable from here (they live in kernel.nsl's Spans, because a
           failure is an app-level concept this seam cannot detect), so Spans>>resetAll
           clears those itself — see the comment there. */
        span_reset_all();
        *out = platform_object();
        return 1;
    case PRIM_pf_gpioSet: {
        /* tuvm#82: `gpioSet: pin to: aBool` — args[0]=pin (SMI), args[1]=Boolean.
           LOGICAL level: on target this goes through gpio_pin_set_dt, so `true`
           means ACTIVE, and the overlay's GPIO_ACTIVE_LOW flags carry the polarity
           instead of every caller. Answers nil for a pin the board never declared;
           kernel.nsl's GpioPulse turns that nil into a tagged raise. */
        /* is_truthy(), not `== true_symbol`: a user-written `true`/`false` literal
           compiles to a fresh Symbol constant loaded with the image (same name,
           different address from the VM's built-in true_sym_obj/false_sym_obj
           singletons) — the same blob/image-literal divergence is_truthy's own
           comment documents (src/dispatch.c). An identity compare here would
           silently treat every literal `true` argument as false. */
        int lvl = is_truthy(args[1]) ? 1 : 0;
        if (!gpio_target_set((int)UNTAG_SMI(args[0]), lvl)) { *out = NULL; return 1; }
        *out = true_symbol;
        return 1;
    }
    case PRIM_pf_gpioLevel: {
        /* tuvm#82: `gpioLevel: pin` — the LOGICAL level, so `true` reads as
           "button pressed" for an active-low button and "powered" for the
           active-low NeoPixel gate. nil for an undeclared pin (see above). */
        int lvl = gpio_target_level((int)UNTAG_SMI(args[0]));
        if (lvl < 0) { *out = NULL; return 1; }
        *out = lvl ? true_symbol : false_symbol;
        return 1;
    }
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
    case PRIM_pf_bellStrike: {
        /* tuvm#94. nil = no bell on this board (structural); otherwise an SMI
           status the nsl wrapper turns into a tag — the display seam's convention,
           minus the bounds code, because the count is range-checked in nsl (Bell
           in kernel.nsl) where the raise can be written.
           The availability check comes FIRST, before the count is even read, so a
           node probing an unknown board gets #bellUnavailable rather than a
           driver fault it cannot act on. */
        if (!bell_target_available()) { *out = NULL; return 1; }
        *out = TAG_SMI(bell_target_strike((int)UNTAG_SMI(args[0])));
        return 1;
    }
    case PRIM_pf_ntpSync: {
        /* tuvm#94 stage 4. nil = couldn't sync (structural: no WiFi, no route,
           timeout); otherwise the SMI seconds-since-local-midnight. args[0] is the
           tz offset in seconds, passed from nsl because the timezone is a product
           fact rather than a hardware one.
           There is no availability probe to run first — unlike the bell, "this board
           cannot sync" and "this sync did not work" are the same answer to a node
           that is going to free-run either way, so one nil serves both. */
        int r = ntp_target_sync((int)UNTAG_SMI(args[0]));
        if (r < 0) { *out = NULL; return 1; }
        *out = TAG_SMI(r);
        return 1;
    }
    case PRIM_pf_ntpDate: {
        /* tuvm#100. nil = no sync has succeeded this boot; otherwise the SMI
           yyyymmdd of the LAST one. No request is made here — the date came back
           in the response ntpSyncTz: already received. */
        int r = ntp_target_date();
        if (r < 0) { *out = NULL; return 1; }
        *out = TAG_SMI(r);
        return 1;
    }
    case PRIM_pf_rtcValid: {
        /* SMI 0/1; the kernel forwarder coerces to a Boolean, so no true/false oop
           is needed in C. */
        *out = TAG_SMI(rtc_target_valid());
        return 1;
    }
    case PRIM_pf_rtcGet: {
        *out = TAG_SMI(rtc_target_get((int)UNTAG_SMI(args[0])));
        return 1;
    }
    case PRIM_pf_rtcPut: {
        rtc_target_put((int)UNTAG_SMI(args[0]), (int)UNTAG_SMI(args[1]));
        *out = TAG_SMI(0);
        return 1;
    }
    case PRIM_pf_wallClock: {
        /* tuvm#100. nil = no RTC on this board, or it has never been set;
           otherwise the SMI seconds-since-local-midnight. Exactly ntpSync's
           answer shape and, like it, no availability probe runs first: to a node
           that will free-run either way, "this board has no RTC" and "the RTC
           could not be read" are the same answer, so one nil serves both. */
        int r = wallclock_target_read();
        if (r < 0) { *out = NULL; return 1; }
        *out = TAG_SMI(r);
        return 1;
    }
    case PRIM_pf_wallClockSet: {
        /* nil = no RTC, or the write was refused; SMI 0 = set. The range check on
           the time lives in the leaf rather than here so both sides enforce it
           identically — a caller that passes 90000 gets nil, not a part programmed
           with a time that cannot exist. */
        int r = wallclock_target_set((int)UNTAG_SMI(args[0]), (int)UNTAG_SMI(args[1]));
        if (r < 0) { *out = NULL; return 1; }
        *out = TAG_SMI(0);
        return 1;
    }
    case PRIM_pf_deepSleep: {
        /* Never returns (device sleeps; host exits). *out is set for form only. */
        deep_sleep_target((int)UNTAG_SMI(args[0]));
        *out = TAG_SMI(0);
        return 1;
    }
    case PRIM_pf_spanStats: {
        /* Same GC-safety shape as heapStats/sysStats above: every scalar is copied
           into a C local BEFORE the single allocating call (indexable_new), and the
           stores are SMIs (immediates, no allocation), so no heap oop is held
           across an allocation and no gc_temp_root pin is required.
           Every field is SMI-safe by construction: counts and microsecond
           magnitudes are far below 2^30, and sum is pre-split at 1e9 by span.c
           precisely so sumUsLo cannot exceed it (src/span.h). */
        uint32_t st[SPAN_STATS_FIELDS];
        int i;
        oop r;
        span_stats((int)UNTAG_SMI(args[0]), st);
        r = indexable_new(SPAN_STATS_FIELDS);
        for (i = 0; i < SPAN_STATS_FIELDS; i++)
            ((struct ObjectInstance *)r)->slots[i + 1] = TAG_SMI((intptr_t)st[i]);
        *out = r;
        return 1;
    }
    default:
        return 0;
    }
}
