/* oevm/src/backend/zephyr/udp_zephyr.c
   Zephyr UDP backend: real datagram sockets over the Zephyr net stack. AF_INET6
   (IPv6-first, matching the Thread/6LoWPAN mesh); one socket, dual-use where the
   L2 supports v4-mapped. Non-blocking via ZSOCK_MSG_DONTWAIT on recv. On
   native_sim this talks to the host/loopback; on the nRF52840 it rides
   OpenThread. Replaces udp_posix.c on device — one file, both Zephyr targets.
   (API names target Zephyr/NCS BSD sockets; adjust to the installed SDK if a
   symbol differs — the S2 build is the gate.) */
#include "udp_backend.h"
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <string.h>

void udp_open(void) { /* net stack is brought up by Kconfig/boot */ }

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
    if (n < 0) return -1;                          /* EWOULDBLOCK -> nothing queued */
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

void udp_close(int sock) { if (sock >= 0) zsock_close(sock); }
