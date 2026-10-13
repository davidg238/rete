#ifndef OEVM_THREAD_BRINGUP_H
#define OEVM_THREAD_BRINGUP_H

/* Bring the OpenThread mesh up for the Zephyr socket layer and BLOCK until the
   MTD attaches (role CHILD/ROUTER) or a ~30 s timeout elapses. ONE shared impl
   (src/thread_bringup.c) for nRF/NCS and ESP32-C6/upstream-4.2 — both expose the
   same no-arg OT API; see thread_bringup.c for why. Only compiled/linked when
   CONFIG_NET_L2_OPENTHREAD=y (this seam lets it re-split if the APIs diverge). */
void oevm_thread_bringup(void);
/* rete#415: free buffers in OpenThread's message pool, or -1 when it cannot say.
   A UDP sendto is accepted before OpenThread queues the message, and a full pool
   drops it there, out of sight of the sender (log_udp.c paces on this). */
int  oevm_thread_free_buffers(void);

#endif /* OEVM_THREAD_BRINGUP_H */
