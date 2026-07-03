#define _POSIX_C_SOURCE 200809L
#include "udp_backend.h"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
/* POSIX (x86-linux / macOS desktop) UDP backend: real dual-stack sockets. Each
   bound socket is AF_INET6 with IPV6_V6ONLY=0, so one socket receives both IPv6
   and IPv4 (v4-mapped) datagrams. Non-blocking (O_NONBLOCK): udp_receive returns
   -1 when nothing is queued. Pure POSIX; udp_zephyr.c replaces it on device. */

void udp_open(void) { /* nothing global to init */ }

int udp_socket_bind(int port) {
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
    if (n < 0) return -1;                  /* EWOULDBLOCK/EAGAIN -> nothing queued */
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

void udp_close(int sock) { if (sock >= 0) close(sock); }
