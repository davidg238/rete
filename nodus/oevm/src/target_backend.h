#ifndef OEVM_TARGET_BACKEND_H
#define OEVM_TARGET_BACKEND_H
/* The board facilities the core dispatches prims to (invarlint#53, the Targets card).
   On target the gpio, display, bell, ntp, rtc and wallclock leaves are defined in
   src/backend/zephyr/{gpio,display,bell,clock,sleep}_zephyr.c beside the devicetree nodes
   they read (rete#440 R6a), and timer_target_snapshot in src/backend/zephyr/timer_zephyr.c; on host src/platform.c defines stubs for the leaves
   it uses. Every symbol links on EVERY Zephyr board: a board without the hardware
   guards the BODY, never the definition. Each defining file includes this header, so
   the compiler checks every definition against the one declaration. */
#include <stdint.h>

/* tuvm#82: digital pins. */
int gpio_target_set(int pin, int level);
int gpio_target_level(int pin);
/* Boot wire-up, called from zephyr/src/main.c: arm the declared button inputs, and drive the
   declared outputs inactive. No-ops on a board that declares none. */
void wire_pulse_gpio(void);
void wire_gpio_outputs(void);

/* tuvm#88: the character display. */
int display_target_available(void);
int display_target_clear(void);
int display_target_put(int row, int col, const char *s, int n);
int display_target_refresh(void);
int display_target_refresh_row(int row);

/* the bell (cosine generator). */
int bell_target_available(void);
int bell_target_strike(int bells);
/* Boot bring-up of the bell pin; defined only where CONFIG_DAC and the spkren alias exist,
   and called under the same guard. */
void bell_bringup(void);

/* tuvm#94 stage 4, tuvm#100: NTP; -1 until a sync has succeeded. */
int ntp_target_sync(int tz_offset_seconds);
int ntp_target_date(void);

/* tuvm#94 stage 5: RTC retention. */
int rtc_target_valid(void);
int rtc_target_get(int i);
void rtc_target_put(int i, int v);

/* tuvm#100: the wall clock; -1 on a board with no RTC node. */
int wallclock_target_read(void);
int wallclock_target_set(int yyyymmdd, int seconds_of_day);

/* rete#215: the app's timers, for the observer's cadence. */
int timer_target_snapshot(uint32_t *period_ms, uint32_t *since_ms, int max);

/* tuvm#94 stage 5: deep sleep for `seconds` (host: exits). */
void deep_sleep_target(int seconds);
/* rete#233 T10: 1 when this boot was a timer wake; a board whose hwinfo has no reset
   cause answers 1. */
int reset_cause_timer_wake(void);
/* shipsBellsV3 (#102): arm the RTC alarm; -1 on a board with no RTC alias or an image
   built without CONFIG_RTC_ALARM. */
int rtc_alarm_arm(int seconds_of_day, int chan_id, int handle_idx);

#endif
