/* oevm/src/backend/zephyr/udp_zephyr.c
   Zephyr UDP backend: real datagram sockets over the Zephyr net stack. AF_INET6
   (IPv6-first, matching the Thread/6LoWPAN mesh); one socket, dual-use where the
   L2 supports v4-mapped. Non-blocking via ZSOCK_MSG_DONTWAIT on recv. On
   native_sim this talks to the host/loopback; on the nRF52840 it rides
   OpenThread. Replaces udp_posix.c on device — one file, both Zephyr targets.
   (API names target Zephyr/NCS BSD sockets; adjust to the installed SDK if a
   symbol differs — the S2 build is the gate.) */
#include "udp_backend.h"
#include "../../pulse_backend.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/zvfs/eventfd.h>
#include <string.h>

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
   the socket's queue; this costs one eventfd, one fd slot and one thread whose
   priority and stack copy the console tee's (log_udp.c) until measured (design,
   "Degenerate cases shipped"). The set holds what zsock_poll can take
   (CONFIG_ZVFS_POLL_MAX) less the eventfd; a registration past that is refused
   loudly and that socket never rings. No reader timeout: that would be a poll. */
#define NET_BELLS_MAX (CONFIG_ZVFS_POLL_MAX - 1)
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

void udp_doorbell(int sock, int chan_id) {
    k_mutex_lock(&bell_lock, K_FOREVER);
    if (wake_fd < 0) {                      /* the first registration starts the reader */
        wake_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
        if (wake_fd < 0) {
            k_mutex_unlock(&bell_lock);
            printk("net: no eventfd (%d), no doorbells\n", wake_fd);
            return;
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
            printk("net: no doorbell slot for fd %d (%d registered)\n", sock, NET_BELLS_MAX);
            return;
        }
        i = bells_n++;
    }
    bool changed = bells[i].fd != sock || bells[i].parked || bells[i].chan != chan_id;
    bells[i].fd = sock; bells[i].chan = chan_id; bells[i].parked = false;
    k_mutex_unlock(&bell_lock);
    if (changed) reader_wake();
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
