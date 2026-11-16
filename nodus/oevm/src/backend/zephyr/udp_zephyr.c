/* oevm/src/backend/zephyr/udp_zephyr.c
   Zephyr UDP backend: real datagram sockets over the Zephyr net stack. AF_INET6
   (IPv6-first, matching the Thread/6LoWPAN mesh); one socket, dual-use where the
   L2 supports v4-mapped. Non-blocking via ZSOCK_MSG_DONTWAIT on recv. On
   native_sim this talks to the host/loopback; on the nRF52840 it rides
   OpenThread. Replaces udp_posix.c on device — one file, both Zephyr targets.
   (API names target Zephyr/NCS BSD sockets; adjust to the installed SDK if a
   symbol differs — the S2 build is the gate.) */
#include "udp_backend.h"
#include "radio_backend.h"
#include "../../pulse_backend.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/zvfs/eventfd.h>
#include <string.h>
#if defined(CONFIG_NET_L2_OPENTHREAD)
#include <openthread.h>
#include <openthread/thread.h>
#elif defined(CONFIG_WIFI)
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <esp_wifi.h>
#endif

void udp_open(void) { /* net stack is brought up by Kconfig/boot */ }

/* ---- rete#440: the doorbells, one reader thread over zsock_poll ---------------------

   Every registered socket (the VM's, from udp.c basicBind:, and the http listener) is in
   one poll set, served by one thread. zsock_poll is LEVEL-triggered: a socket left in the
   set with an unread datagram would ring forever, which is the spin this exists to
   remove. So a socket that rings is PARKED (dropped from the set) until the VM reads it
   to empty, when udp_receive restores it; the listener restores by re-registering
   (http_poll answering 0). A datagram landing between that empty answer and the restore
   is seen at once on the next poll, because the poll is level-triggered. One socket
   parked never silences another.

   The set changes under a reader blocked in zsock_poll, so an eventfd sits in the set
   and every change writes it. Chosen over net_context receive callbacks, which the
   socket layer already owns (zsock_received_cb), so taking them means reimplementing
   the socket's queue; this costs one eventfd, one fd slot and one thread.

   CAPACITY: the set holds what zsock_poll can take (CONFIG_ZVFS_POLL_MAX) less the
   eventfd, and zephyr/Kconfig sizes ZVFS_POLL_MAX to ZVFS_OPEN_MAX, so every other fd
   the build can open fits. A registration that still finds no room answers -1 and its
   bind fails (udp.c): a socket that never rings is a deaf node. No reader timeout:
   that would be a poll. */
#define NET_BELLS_MAX (CONFIG_ZVFS_POLL_MAX - 1)
BUILD_ASSERT(CONFIG_ZVFS_POLL_MAX >= CONFIG_ZVFS_OPEN_MAX,
             "the Net reader must be able to poll every fd the build can open");
/* Copied from the console tee's thread (log_udp.c: K_THREAD_DEFINE(log_udp_tid, 2048, ...,
   K_PRIO_PREEMPT(10), ...)): stack 2048 B, priority K_PRIO_PREEMPT(10), until measured
   (design, "Degenerate cases shipped"). */
#define NET_READER_STACK 2048
#define NET_READER_PRIO  K_PRIO_PREEMPT(10)

static struct { int fd; int chan; bool parked; } bells[NET_BELLS_MAX];
static int bells_n;
static K_MUTEX_DEFINE(bell_lock);
static int wake_fd = -1;
static bool forget_pending;                 /* udp_close waits for the reader to drop an fd */
static K_SEM_DEFINE(forgot, 0, 1);
K_THREAD_STACK_DEFINE(net_reader_stack, NET_READER_STACK);
static struct k_thread net_reader_thread;

static void reader_wake(void) {
    if (wake_fd >= 0) (void)zvfs_eventfd_write(wake_fd, 1);
}

static int bell_find(int fd) {
    for (int i = 0; i < bells_n; i++) if (bells[i].fd == fd) return i;
    return -1;
}

static void net_reader(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    struct zsock_pollfd p[NET_BELLS_MAX + 1];
    for (;;) {
        int n = 0;
        p[n].fd = wake_fd; p[n].events = ZSOCK_POLLIN; p[n].revents = 0; n++;
        k_mutex_lock(&bell_lock, K_FOREVER);
        for (int i = 0; i < bells_n; i++) {
            if (bells[i].parked) continue;
            p[n].fd = bells[i].fd; p[n].events = ZSOCK_POLLIN; p[n].revents = 0; n++;
        }
        if (forget_pending) { forget_pending = false; k_sem_give(&forgot); }
        k_mutex_unlock(&bell_lock);

        if (zsock_poll(p, n, -1) < 0) {
            /* Not a timeout: an fd the set named went bad under it. The set is rebuilt
               from the table, which udp_close has already corrected; yield once so a
               persistent error cannot starve the VM. */
            k_yield();
            continue;
        }
        if (p[0].revents) { zvfs_eventfd_t v; (void)zvfs_eventfd_read(wake_fd, &v); }
        for (int k = 1; k < n; k++) {
            if (!p[k].revents) continue;
            /* POLLNVAL: the fd is no socket any more. Parked without a bell, or the set
               would return at once forever; a re-registration brings it back. */
            bool ring = !(p[k].revents & ZSOCK_POLLNVAL);
            k_mutex_lock(&bell_lock, K_FOREVER);
            int i = bell_find(p[k].fd);
            int chan = -1;
            if (i >= 0 && !bells[i].parked) {
                bells[i].parked = true;
                if (ring) chan = bells[i].chan;
            }
            k_mutex_unlock(&bell_lock);
            if (chan >= 0) pulse_post_isr_value(chan, TAG_SMI(p[k].fd));
        }
    }
}

int udp_doorbell(int sock, int chan_id) {
    k_mutex_lock(&bell_lock, K_FOREVER);
    if (wake_fd < 0) {                      /* the first registration starts the reader */
        wake_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
        if (wake_fd < 0) {
            k_mutex_unlock(&bell_lock);
            printk("net: no eventfd (%d): bind refused\n", wake_fd);
            return -1;
        }
        k_thread_create(&net_reader_thread, net_reader_stack,
                        K_THREAD_STACK_SIZEOF(net_reader_stack), net_reader,
                        NULL, NULL, NULL, NET_READER_PRIO, 0, K_NO_WAIT);
        k_thread_name_set(&net_reader_thread, "net_reader");
    }
    int i = bell_find(sock);
    if (i < 0) {
        if (bells_n >= NET_BELLS_MAX) {
            k_mutex_unlock(&bell_lock);
            printk("net: no doorbell for fd %d (%d registered): bind refused\n", sock, NET_BELLS_MAX);
            return -1;
        }
        i = bells_n++;
    }
    bool changed = bells[i].fd != sock || bells[i].parked || bells[i].chan != chan_id;
    bells[i].fd = sock; bells[i].chan = chan_id; bells[i].parked = false;
    k_mutex_unlock(&bell_lock);
    if (changed) reader_wake();
    return 0;
}

/* Restore: the VM has read sock to empty, so it may ring again. */
static void bell_restore(int sock) {
    bool was = false;
    k_mutex_lock(&bell_lock, K_FOREVER);
    int i = bell_find(sock);
    if (i >= 0 && bells[i].parked) { bells[i].parked = false; was = true; }
    k_mutex_unlock(&bell_lock);
    if (was) reader_wake();
}

/* Forget sock and wait until the reader has rebuilt its set without it, so the fd is
   never closed (and reused) under a poll that still names it. */
static void bell_forget(int sock) {
    k_mutex_lock(&bell_lock, K_FOREVER);
    int i = bell_find(sock);
    if (i < 0) { k_mutex_unlock(&bell_lock); return; }
    bells[i] = bells[--bells_n];
    forget_pending = true;
    k_sem_reset(&forgot);
    k_mutex_unlock(&bell_lock);
    reader_wake();
    (void)k_sem_take(&forgot, K_FOREVER);
}

int udp_socket_bind(int port) {
    int fd = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    /* tuvm#72: an AF_INET6 socket defaults to v6-only, so IPv4 datagrams (a wifi
       node's gw ctl arrives over IPv4) are dropped even with
       CONFIG_NET_IPV4_MAPPING_TO_IPV6=y. Clear V6ONLY so the socket accepts
       v4-mapped (::ffff:a.b.c.d) traffic; the send path already builds mapped
       destinations. Best-effort: on a build without the option it is a no-op and
       the IPv6/Thread path is unaffected. */
    int v6only = 0;
    (void)zsock_setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { zsock_close(fd); return -1; }
    return fd;
}

/* tuvm#72: lazily-opened AF_INET socket used ONLY to send to IPv4 peers. See
   udp_send. Never bound — the stack assigns an ephemeral source port, which is
   all a push needs (the collector keys a node by source ADDRESS, not port). */
static int v4_send_fd = -1;

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    struct in_addr v4;
    /* IPv4 literal FIRST. Sending to a v4-mapped destination (::ffff:a.b.c.d) on
       the AF_INET6 socket is ACCEPTED — zsock_sendto answers the byte count — but
       the datagram never reaches an IPv4 peer: CONFIG_NET_IPV4_MAPPING_TO_IPV6
       maps INBOUND v4 onto the v6 socket, it does NOT translate an outbound
       v4-mapped destination back to IPv4. A wifi node's push therefore vanished
       with every indication of success: the node logged a written result, the
       send reported OK, and the gw never saw a datagram (tuvm#72, WROOM-32).
       Emit genuine IPv4 instead. The RECEIVE path stays AF_INET6 — inbound
       v4-mapped delivery works and the Thread/IPv6 path is untouched. */
    if (zsock_inet_pton(AF_INET, host, &v4) == 1) {
        struct sockaddr_in a4;
        if (v4_send_fd < 0) {
            v4_send_fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (v4_send_fd < 0) return -1;
        }
        memset(&a4, 0, sizeof a4);
        a4.sin_family = AF_INET;
        a4.sin_port = htons((unsigned short)port);
        a4.sin_addr = v4;
        return (int)zsock_sendto(v4_send_fd, data, (size_t)len, 0,
                                 (struct sockaddr *)&a4, sizeof a4);
    }
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_inet_pton(AF_INET6, host, &a.sin6_addr) != 1) return -1;
    return (int)zsock_sendto(sock, data, (size_t)len, 0,
                             (struct sockaddr *)&a, sizeof a);
}

int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    struct sockaddr_in6 src;
    socklen_t slen = sizeof src;
    ssize_t n = zsock_recvfrom(sock, buf, (size_t)buf_cap, ZSOCK_MSG_DONTWAIT,
                               (struct sockaddr *)&src, &slen);
    if (n < 0) { bell_restore(sock); return -1; }  /* EWOULDBLOCK -> nothing queued */
    *sender_port = ntohs(src.sin6_port);
    /* Zephyr's net_addr_ntop ignores `size` in kernel context, so guard the
       buffer ourselves (the POSIX backend bounds via snprintf). INET6_ADDRSTRLEN
       (46) covers the longest IPv6 text form incl. ::ffff:a.b.c.d. */
    if (host_cap < INET6_ADDRSTRLEN ||
        net_addr_ntop(AF_INET6, &src.sin6_addr, host_out, (size_t)host_cap) == NULL) {
        if (host_cap > 0) host_out[0] = '\0';
    } else {
        /* normalize a v4-mapped sender ::ffff:1.2.3.4 -> 1.2.3.4 (best-effort) */
        if (host_out[0] && IN6_IS_ADDR_V4MAPPED(&src.sin6_addr)) {
            const char *p = strrchr(host_out, ':');
            if (p) memmove(host_out, p + 1, strlen(p + 1) + 1);
        }
    }
    return (int)n;
}

void udp_close(int sock) { if (sock >= 0) { bell_forget(sock); zsock_close(sock); } }

/* ---- rete#440 R3 (#458): the link, rung on every change -----------------------------

   Thread: the link is the attach (child, router or leader), told by OpenThread's
   state-changed callback on a role change; the C6 build has no net_mgmt, and the role is
   read from OpenThread itself rather than from the net_if the L2 makes dormant in its own
   callback, so the answer never depends on which of the two ran first. WiFi: the link is
   the station associated with an IPv4 lease, told by net_mgmt (the WiFi builds carry it
   for main.c's #65 events). A build with neither has no link to lose: it answers up and
   never rings. The token is constant (TAG_SMI(0)); the reaction reads net_link_up. */
static int link_chan = -1;

static void net_link_ring(void) {
    int c = link_chan;
    if (c >= 0) pulse_post_isr_value(c, TAG_SMI(0));
}

#if defined(CONFIG_NET_L2_OPENTHREAD)
static void net_link_ot_changed(otChangedFlags flags, void *ctx) {
    (void)ctx;
    if (flags & OT_CHANGED_THREAD_ROLE) net_link_ring();
}
static struct openthread_state_changed_callback link_cb = { .otCallback = net_link_ot_changed };

int net_link_up(void) {
    otInstance *inst = openthread_get_default_instance();
    if (inst == NULL) return 0;
    openthread_mutex_lock();
    otDeviceRole role = otThreadGetDeviceRole(inst);
    openthread_mutex_unlock();
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER
        || role == OT_DEVICE_ROLE_LEADER;
}

void net_link_doorbell(int chan_id) {
    if (link_chan < 0) (void)openthread_state_changed_callback_register(&link_cb);
    link_chan = chan_id;
}
#elif defined(CONFIG_WIFI)
static void net_link_event(struct net_mgmt_event_callback *cb, uint64_t ev, struct net_if *iface) {
    (void)cb; (void)ev;
    if (iface == net_if_get_first_wifi()) net_link_ring();
}
static struct net_mgmt_event_callback link_cb;

int net_link_up(void) {
    struct net_if *iface = net_if_get_first_wifi();
    return net_if_is_up(iface) && net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

void net_link_doorbell(int chan_id) {
    if (link_chan < 0) {
        net_mgmt_init_event_callback(&link_cb, net_link_event,
                                     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN |
                                     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL);
        net_mgmt_add_event_callback(&link_cb);
    }
    link_chan = chan_id;
}
#else
int net_link_up(void) { return 1; }
void net_link_doorbell(int chan_id) { link_chan = chan_id; }
#endif

/* ---- rete#440 R3 (#458): the radio, WiFi's body and the radio-less one --------------

   Thread's body is thread_bringup.c's. WiFi: off is the station disconnected and the
   WiFi stopped (esp_wifi_stop, the radio powered down); on is the same association request
   the boot made (radio_wifi_connect), whose connect starts the WiFi again; DHCP follows
   the interface. Stateless: the driver's own status answers "is it on" (inactive is off).
   A build with no radio has nothing to switch off and answers on. */
#if defined(CONFIG_WIFI) && !defined(CONFIG_NET_L2_OPENTHREAD)
int radio_wifi_connect(void) {
    struct net_if *iface = net_if_get_first_wifi();
    struct wifi_connect_req_params p;
    if (iface == NULL) return -ENODEV;
    memset(&p, 0, sizeof p);
    p.ssid        = (const uint8_t *)CONFIG_OEVM_WIFI_SSID;
    p.ssid_length = strlen(CONFIG_OEVM_WIFI_SSID);
    p.psk         = (const uint8_t *)CONFIG_OEVM_WIFI_PSK;
    p.psk_length  = strlen(CONFIG_OEVM_WIFI_PSK);
    p.security    = WIFI_SECURITY_TYPE_PSK;
    p.channel     = WIFI_CHANNEL_ANY;
    p.band        = WIFI_FREQ_BAND_2_4_GHZ;
    p.mfp         = WIFI_MFP_OPTIONAL;
    return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof p);
}

int radio_is_on(void) {
    struct net_if *iface = net_if_get_first_wifi();
    struct wifi_iface_status st = { 0 };
    if (iface == NULL) return 0;
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof st) != 0) return 0;
    return st.state != WIFI_STATE_INACTIVE;
}

void radio_set(int on) {
    struct net_if *iface = net_if_get_first_wifi();
    if (iface == NULL || !on == !radio_is_on()) return;
    if (on) {
        printk("wifi: radio on (%d)\n", radio_wifi_connect());
    } else {
        (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
        printk("wifi: radio off (%d)\n", (int)esp_wifi_stop());
    }
}
#elif !defined(CONFIG_NET_L2_OPENTHREAD)
void radio_set(int on) { (void)on; }
int radio_is_on(void) { return 1; }
#endif
