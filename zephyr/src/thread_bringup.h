#ifndef TUVM_THREAD_BRINGUP_H
#define TUVM_THREAD_BRINGUP_H

/* Bring the OpenThread mesh up for the Zephyr socket layer and BLOCK until the
   MTD attaches (role CHILD/ROUTER) or a ~30 s timeout elapses. Board-selected:
   NCS impl (backend/ncs) for nRF, upstream-4.2 impl (backend/zephyr) for the C6.
   Only compiled/linked when CONFIG_NET_L2_OPENTHREAD=y. */
void tuvm_thread_bringup(void);

#endif /* TUVM_THREAD_BRINGUP_H */
