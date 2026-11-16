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

   ESP32-C6 CAVEAT (#59, 2026-07-20): this code is correct on stock Zephyr 4.2.0 but
   the C6 will still never attach on it — 4.2.0's drivers/ieee802154/ieee802154_esp32.c
   maps the radio stop() onto esp_ieee802154_disable() (a full teardown) where
   OpenThread only expects sleep. Upstream fixed it in 4.3.x (d66a39f7b5b "ieee802154:
   esp32: Fix start/stop API", plus a9eb90a20e9 for the 802.15.4 MAC universe). Our
   ~/zephyr-esp32 workspace carries those as branch oevm-c6-radio-fix; on a plain
   v4.2.0 checkout the symptom is role stuck DETACHED with a provably correct dataset.

   Shape: enable -> admin-up the net_if -> poll role -> print, BLOCKING until the
   MTD attaches so the VM starts on a joined mesh. */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread.h>
#include <openthread/thread.h>
#include <openthread/message.h>
#include "thread_bringup.h"
#include "radio_backend.h"

void oevm_thread_bringup(void) {
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
    for (int i = 0; i < 300 && inst; i++) {          /* wait up to ~30s for attach */
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

int oevm_thread_free_buffers(void)
{
	struct otInstance *ot = openthread_get_default_instance();
	otBufferInfo info;

	if (ot == NULL) {
		return -1;
	}
	openthread_mutex_lock();
	otMessageGetBufferInfo(ot, &info);
	openthread_mutex_unlock();
	return info.mFreeBuffers == 0xffff ? -1 : info.mFreeBuffers;
}

int oevm_thread_tx_queued(void)
{
	struct otInstance *ot = openthread_get_default_instance();
	otBufferInfo info;

	if (ot == NULL) {
		return 0;
	}
	openthread_mutex_lock();
	otMessageGetBufferInfo(ot, &info);
	openthread_mutex_unlock();
	return info.mIp6Queue.mNumMessages + info.m6loSendQueue.mNumMessages;
}

/* rete#440 R3 (#458): the radio, Thread's body (radio_backend.h). Off is Thread disabled
   (openthread_stop: otThreadSetEnabled false), so the MTD stops polling its parent and the
   802.15.4 radio sleeps; the dataset stays in OpenThread's settings. On is openthread_run,
   which enables Thread again and re-attaches from that dataset. The link follows the role
   (the OT L2 turns the net_if dormant while detached), and Net rings sys/net/link on it
   (udp_zephyr.c net_link_*). Stateless: OpenThread's role is the answer to "is it on". */
void radio_set(int on)
{
	if (!on == !radio_is_on()) {
		return;                                 /* already there: no restart, no log */
	}
	int rc = on ? openthread_run() : openthread_stop();

	printk("thread: radio %s (%d)\n", on ? "on" : "off", rc);
}

int radio_is_on(void)
{
	otInstance *inst = openthread_get_default_instance();
	otDeviceRole role;

	if (inst == NULL) {
		return 0;
	}
	openthread_mutex_lock();
	role = otThreadGetDeviceRole(inst);
	openthread_mutex_unlock();
	return role != OT_DEVICE_ROLE_DISABLED;
}
