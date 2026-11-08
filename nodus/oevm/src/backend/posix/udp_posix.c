#define _POSIX_C_SOURCE 200809L
#include "udp_backend.h"
#include "udp_posix.h"
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv */
/* POSIX (x86-linux / macOS desktop) UDP backend: real dual-stack sockets. Each
   bound socket is AF_INET6 with IPV6_V6ONLY=0, so one socket receives both IPv6
   and IPv4 (v4-mapped) datagrams. Non-blocking (O_NONBLOCK): udp_receive returns
   -1 when nothing is queued. Pure POSIX; udp_zephyr.c replaces it on device.

   rete#440, the doorbells: a socket table of every registered socket (the VM's and the
   http listener), polled by udp_posix_poll from inside the pulse pump. The host pump
   stays non-blocking (the suite drives a virtual clock), so this is poll() with a zero
   timeout; the behaviour, a bell on arrival and silence until read to empty, is the
   target's. */

/* The host can open far more sockets than this; the bound is the table's, and a
   registration past it fails the bind (udp.c), so nothing is silently deaf. */
#define BELLS_MAX 16
static struct { int fd; int chan; int parked; } bells[BELLS_MAX];
static int bells_n = 0;

static int bell_find(int fd) {
    for (int i = 0; i < bells_n; i++) if (bells[i].fd == fd) return i;
    return -1;
}

int udp_doorbell(int sock, int chan_id) {
    int i = bell_find(sock);
    if (i < 0) {
        if (bells_n >= BELLS_MAX) {
            fprintf(stderr, "net: no doorbell for fd %d (%d registered): bind refused\n", sock, bells_n);
            return -1;
        }
        i = bells_n++;
    }
    bells[i].fd = sock; bells[i].chan = chan_id; bells[i].parked = 0;
    return 0;
}

/* Restore: the VM has read this socket to empty, so it may ring again. */
static void bell_restore(int fd) {
    int i = bell_find(fd);
    if (i >= 0) bells[i].parked = 0;
}

static void bell_forget(int fd) {
    int i = bell_find(fd);
    if (i >= 0) bells[i] = bells[--bells_n];
}

void udp_posix_poll(void) {
    struct pollfd p[BELLS_MAX];
    int idx[BELLS_MAX], n = 0;
    for (int i = 0; i < bells_n; i++) {
        if (bells[i].parked) continue;
        p[n].fd = bells[i].fd; p[n].events = POLLIN; p[n].revents = 0; idx[n++] = i;
    }
    if (n == 0 || poll(p, (nfds_t)n, 0) <= 0) return;
    for (int k = 0; k < n; k++) {
        if (!p[k].revents) continue;
        int i = idx[k];
        bells[i].parked = 1;
        pulse_posix_ring(bells[i].chan, bells[i].fd);
    }
}

void udp_open(void) { /* nothing global to init */ }

int udp_socket_bind(int port) {
    /* rete#380: OEVM_NO_NET makes every bind fail, the way OEVM_NO_BELL / OEVM_NO_NTP /
       OEVM_NO_DISPLAY (platform.c) take a leaf away. A bridged node then runs with
       `PortaBridge socket: nil` (the #274 construction) and checks in nowhere, which is
       what lets the deep-sleep host gate drive a porta node across 40 wakes without a
       porta and without burning two reply budgets per wake. */
    if (getenv("OEVM_NO_NET") != NULL) return -1;
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int off = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);  /* dual-stack */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

int udp_send(int sock, const char *host, int port, const uint8_t *data, int len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;            /* socket is v6; map v4 targets */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_V4MAPPED | AI_ALL | AI_NUMERICSERV;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    ssize_t n = sendto(sock, data, (size_t)len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return (int)n;
}

int udp_receive(int sock, uint8_t *buf, int buf_cap,
                char *host_out, int host_cap, int *sender_port) {
    struct sockaddr_storage src;
    socklen_t slen = sizeof src;
    ssize_t n = recvfrom(sock, buf, (size_t)buf_cap, 0,
                         (struct sockaddr *)&src, &slen);
    if (n < 0) { bell_restore(sock); return -1; }   /* EWOULDBLOCK/EAGAIN -> nothing queued */
    char h[1025];  /* NI_MAXHOST (1025) without depending on that macro */
    int port = 0;
    if (getnameinfo((struct sockaddr *)&src, slen, h, sizeof h,
                    NULL, 0, NI_NUMERICHOST) != 0) h[0] = '\0';
    if (src.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&src;
        port = ntohs(s6->sin6_port);
        /* normalize a v4-mapped sender ::ffff:1.2.3.4 -> 1.2.3.4 */
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            const char *p = strrchr(h, ':');
            if (p) memmove(h, p + 1, strlen(p + 1) + 1);
        }
    } else if (src.ss_family == AF_INET) {
        port = ntohs(((struct sockaddr_in *)&src)->sin_port);
    }
    snprintf(host_out, (size_t)host_cap, "%s", h);
    *sender_port = port;
    return (int)n;
}

void udp_close(int sock) { if (sock >= 0) { bell_forget(sock); close(sock); } }
