/* OpenThread bring-up for the Zephyr socket layer. ONE shared impl for every
   OpenThread target (nRF via NCS, ESP32-C6 via upstream 4.2): both toolchains
   expose the SAME no-arg OpenThread API. As of Zephyr 4.2 the OT integration was
   refactored out of subsys/net/l2/openthread/ into a standalone module
   (modules/openthread/, doc/releases/migration-guide-4.2.rst); its <openthread.h>
   — globally on the include path via that module's zephyr_include_directories —
   declares the non-deprecated no-arg calls (openthread_init/openthread_run/
   openthread_mutex_lock/openthread_mutex_unlock/openthread_get_default_instance).
   Only the net_if is fetched via the older <zephyr/net/openthread.h>
   openthread_get_default_context()->iface (that field, unlike ->instance, is not
   deprecated). This was two near-identical board files until 2026-07-20 (#59); the
   board-split's premise — "the C6 can't compile the nRF bring-up" — was false (an
   incomplete header probe), so they were collapsed. Re-split behind the
   thread_bringup.h seam only if NCS and upstream OT APIs actually diverge.

   Shape: enable -> admin-up the net_if -> poll role -> print, BLOCKING until the
   MTD attaches so the VM starts on a joined mesh. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread.h>
#include <openthread/thread.h>
#include <openthread/link.h>
#include "thread_bringup.h"

void tuvm_thread_bringup(void) {
    (void)openthread_init();                        /* idempotent: L2 inited at boot */
    int rr = openthread_run();                       /* enable IPv6 + Thread -> attach */

    struct openthread_context *ctx = openthread_get_default_context();
    struct net_if *iface = ctx ? ctx->iface : net_if_get_default();
    /* openthread_run() enables IPv6+Thread so the node ATTACHES (ot-ctl ping /
       child table work), but it does NOT admin-up the Zephyr socket net_if — so
       zsock_sendto returns ENETDOWN (errno 115) even while attached.
       net_if_up() is THE fix. */
    int ur = iface ? net_if_up(iface) : -1;
    printk("thread: run=%d net_if_up=%d, joining...\n", rr, ur);

    otInstance *inst = openthread_get_default_instance();
    if (inst) {                                      /* DIAG(#59): confirm the seed applied */
        openthread_mutex_lock();
        printk("thread: DIAG name=%s ch=%u panid=0x%04x\n",
               otThreadGetNetworkName(inst), otLinkGetChannel(inst), otLinkGetPanId(inst));
        openthread_mutex_unlock();
    }
    for (int i = 0; i < 300 && inst; i++) {          /* wait up to ~30s for attach */
        openthread_mutex_lock();
        otDeviceRole role = otThreadGetDeviceRole(inst);
        openthread_mutex_unlock();
        if (i % 10 == 0) printk("thread: DIAG role=%d t=%us\n", role, i / 10);  /* DIAG(#59) */
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER) {
            printk("thread: attached role=%d\n", role);
            return;
        }
        k_msleep(100);
    }
    printk("thread: attach timed out, continuing\n");
}
