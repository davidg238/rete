/* ESP32-C6 OpenThread bring-up against upstream Zephyr 4.2. As of this Zephyr
   version the OpenThread integration was refactored out of
   subsys/net/l2/openthread/ into a standalone module (modules/openthread/) —
   see doc/releases/migration-guide-4.2.rst. The non-deprecated API is
   call-with-no-args (openthread_init/openthread_run/openthread_mutex_lock/
   openthread_mutex_unlock/openthread_get_default_instance, declared in the
   module's <openthread.h>, globally on the include path via
   zephyr_include_directories(include) in modules/openthread/CMakeLists.txt);
   only the net_if is still fetched via the older <zephyr/net/openthread.h>
   openthread_get_default_context()->iface (that field, unlike ->instance,
   is NOT deprecated). Same shape as the NCS impl (which targets this same
   refactored API): enable -> admin-up the net_if -> poll role -> print,
   BLOCKING until the MTD attaches so the VM starts on a joined mesh. */
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
    /* openthread_run() enables IPv6+Thread so the node ATTACHES (ot-ctl ping /
       child table work), but it does NOT admin-up the Zephyr socket net_if —
       so zsock_sendto returns ENETDOWN (errno 115) even while attached.
       net_if_up(ctx->iface) is THE fix. */
    int ur = (ctx && ctx->iface) ? net_if_up(ctx->iface) : -1;
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
