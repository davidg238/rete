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
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { zsock_close(fd); return -1; }
    return fd;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((unsigned short)port);
    if (zsock_inet_pton(AF_INET6, host, &a.sin6_addr) != 1) {
        struct in_addr v4;                       /* accept a v4 dotted literal */
        if (zsock_inet_pton(AF_INET, host, &v4) != 1) return -1;
        memset(&a.sin6_addr, 0, sizeof a.sin6_addr);   /* build ::ffff:a.b.c.d */
        a.sin6_addr.s6_addr[10] = 0xff;
        a.sin6_addr.s6_addr[11] = 0xff;
        memcpy(&a.sin6_addr.s6_addr[12], &v4, 4);
    }
    ssize_t n = zsock_sendto(sock, data, (size_t)len, 0,
                             (struct sockaddr *)&a, sizeof a);
    return (int)n;
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
