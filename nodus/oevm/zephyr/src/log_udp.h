/* Copyright (c) 2026 Ekorau LLC */
/* The remote console (#56 §10, built 2026-08-04 for #102).
 *
 * A board on wallwart power with no data cables is unobservable, and this bench
 * spent an afternoon proving how much that costs: a bell rang the right count at
 * 15:30 and nothing on earth could say whether the RTC alarm or the watchdog had
 * triggered it, because the two differ only by a log line nobody could read.
 *
 * log_udp_start() TEES printk to a UDP datagram per line. It does not replace the
 * UART console — the previous hook is chained, so a board with a cable attached
 * keeps its local console and the #56 collector is unaffected.
 *
 * rete#415: the TARGET is set at run time, from the goal/console cell (prim
 * consoleTee:port:, platform_backend.h), so the custos that asked for a profile is
 * the one that hears it, and two developers on one mesh each hear their own node.
 * Compiled into every image with sockets. Until a target arrives the ring HOLDS the
 * boot (4 KB), which then goes out in one burst, exactly as it used to wait for a
 * route. The last target (or off) is KEPT in RTC memory, so a node waking from deep
 * sleep, or restarting, tees from its first line, before a check-in replays the cell
 * (a window opened from boot may dump before then). With nothing kept, the console
 * holds in the ring until the cell arrives; there is no build-time target (Q2′).
 * Every state SAYS SO at boot, because a silently-inactive remote console is the
 * exact failure class that cost twenty minutes over a missing NTP address.
 */
#ifndef OEVM_LOG_UDP_H
#define OEVM_LOG_UDP_H

#include <stddef.h>

void log_udp_start(void);
/* The VM's console leaf: queue bytes for the tee. Any context; never blocks. */
void log_udp_tee(const char *bytes, size_t n);
/* The profile dump's: queue bytes, waiting (thread context only) while the ring is too
   full for them AND the sender is making progress, so a dump larger than the ring is
   slowed rather than truncated. A sender making no progress (no route) is not waited
   on: the line is dropped and counted, as any overflow is. */
void log_udp_tee_paced(const char *bytes, size_t n);
/* addr6 a literal IPv6 address, len bytes, not terminated; len 0 turns the tee off.
   Answers 1 when the tee now points there (or is off), 0 when addr6 or port is bad. */
int  log_udp_set_target(const char *addr6, size_t len, int port);
/* Wait, at most max_ms, for every queued line to leave: before a deep sleep. */
void log_udp_drain(int max_ms);

#endif /* OEVM_LOG_UDP_H */
