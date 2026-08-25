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
 * Compiled always, active only when the image was built with OEVM_LOG_UDP6 (the
 * same env->define pattern as OEVM_NTP_SERVER6). Absent, this is a no-op that
 * SAYS SO at boot, because a silently-inactive remote console is the exact
 * failure class that cost twenty minutes over a missing NTP address.
 */
#ifndef OEVM_LOG_UDP_H
#define OEVM_LOG_UDP_H

void log_udp_start(void);

#endif /* OEVM_LOG_UDP_H */
