#ifndef TUVM_THREAD_BRINGUP_H
#define TUVM_THREAD_BRINGUP_H

/* Bring the OpenThread mesh up for the Zephyr socket layer and BLOCK until the
   MTD attaches (role CHILD/ROUTER) or a ~30 s timeout elapses. ONE shared impl
   (src/thread_bringup.c) for nRF/NCS and ESP32-C6/upstream-4.2 — both expose the
   same no-arg OT API; see thread_bringup.c for why. Only compiled/linked when
   CONFIG_NET_L2_OPENTHREAD=y (this seam lets it re-split if the APIs diverge). */
void tuvm_thread_bringup(void);

#endif /* TUVM_THREAD_BRINGUP_H */
