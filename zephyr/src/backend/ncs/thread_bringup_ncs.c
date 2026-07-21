/* NCS (nRF52840) OpenThread bring-up. Moved verbatim from zephyr/src/main.c
   (rung 7). NCS API: openthread_init/run/get_default_instance + api mutex. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread.h>
#include <openthread/thread.h>
#include "../../thread_bringup.h"

void tuvm_thread_bringup(void) {
    (void)openthread_init();
    int rr = openthread_run();
    struct openthread_context *ctx = openthread_get_default_context();
    struct net_if *iface = ctx ? ctx->iface : net_if_get_default();
    int ur = iface ? net_if_up(iface) : -1;
    printk("thread: run=%d net_if_up=%d, joining...\n", rr, ur);

    otInstance *inst = openthread_get_default_instance();
    for (int i = 0; i < 300 && inst; i++) {
        openthread_mutex_lock();
        otDeviceRole role = otThreadGetDeviceRole(inst);
        openthread_mutex_unlock();
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER) {
            printk("thread: attached role=%d\n", role);
            return;
        }
        k_msleep(100);
    }
    printk("thread: attach timed out, continuing\n");
}
