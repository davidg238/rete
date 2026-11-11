#ifndef OEVM_RADIO_BACKEND_H
#define OEVM_RADIO_BACKEND_H
/* rete#440 R3 (#458): the radio, one verb across BLE, Thread and WiFi, with the VM live
   (design 2026-11-04, "Radio"; card invarlint/cards/radio.yaml). Exactly one body is
   linked per build: udp_ble.c (BLE: advertising and the relay's connection), thread_bringup.c
   (Thread: OpenThread stopped and run), udp_zephyr.c (WiFi: the station stopped and
   reconnected; a build with no radio to switch answers it cannot), udp_posix.c (host: a
   flag, so the suite can drive it).

   Whether the link is up is Net's (udp_backend.h net_link_*): the radio coming on is not
   the link coming up, and the bridge waits for the second under its link budget.

   A send while the radio is off answers -1, as a send with the link down already does
   (ENOTCONN on BLE, ENETDOWN on sockets and the host). The bridge reads both as a miss. */

/* Switch the radio on (1) or off (0). Idempotent; a build with no radio to switch, or a
   stack that refuses, leaves it as it was, and radio_is_on says which. */
void radio_set(int on);
/* 1 while the radio is on: the answer the kernel's `radioOn:` gives back. */
int radio_is_on(void);

#if defined(CONFIG_WIFI) && !defined(CONFIG_NET_L2_OPENTHREAD)
/* The station's association request from CONFIG_OEVM_WIFI_*, one place for the boot's
   first connect (zephyr/src/main.c wifi_bringup) and every radio_set(1) after an off. */
int radio_wifi_connect(void);
#endif
#endif
