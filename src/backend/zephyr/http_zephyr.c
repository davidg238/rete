/* Zephyr HTTP transport hooks: a non-blocking TCP listener over the Zephyr net
   stack (IPv4, WiFi/DHCP), polled from the VM reactor thread. All framing/ring/
   in-flight logic lives in src/http_core.c. Mirrors udp_zephyr.c's non-blocking
   idiom (ZSOCK_MSG_DONTWAIT jitter aside, this uses a bounded SO_RCVTIMEO read
   instead, since a request line — unlike a datagram — can arrive in more than
   one TCP segment). HTTP_METHOD_MAX/HTTP_PATH_MAX come from http_backend.h (one
   definition shared by http_core.c and every backend — see Task 1 review).
   fd == -1 (injected/test) never occurs on device -> http_emit no-ops it.

   SDK reconciliation (~zephyr-esp32, upstream Zephyr, verified by grepping
   include/zephyr/net/socket*.h and posix/fcntl.h):
    - struct zsock_timeval is `#define zsock_timeval timeval` (net/socket_types.h);
      struct timeval itself comes in transitively via net/socket.h's include chain,
      so no extra header is needed for the SO_RCVTIMEO struct.
    - F_SETFL/O_NONBLOCK are NOT pulled in by <zephyr/net/socket.h> on this SDK
      (it never includes posix/fcntl.h) — <zephyr/posix/fcntl.h> is included here
      explicitly for zsock_fcntl(). */
#include "http_backend.h"
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/posix/fcntl.h>  /* F_SETFL, O_NONBLOCK (not pulled in by net/socket.h) */
#include <string.h>
#include <stdio.h>               /* sscanf */

static int listen_fd = -1;

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) return;                    /* no injected sink on device */
    (void)zsock_send(fd, hdr, (size_t)hlen, 0);
    if (body_len > 0) (void)zsock_send(fd, body, (size_t)body_len, 0);
    zsock_close(fd);
}

int http_listen(int port) {
    listen_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) return -1;
    int one = 1;
    zsock_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);   /* listen on the DHCP-assigned addr */
    a.sin_port = htons((unsigned short)port);
    if (zsock_bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        zsock_close(listen_fd); listen_fd = -1; return -1;
    }
    if (zsock_listen(listen_fd, 4) < 0) {
        zsock_close(listen_fd); listen_fd = -1; return -1;
    }
    zsock_fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

void http_poll(void) {
    if (listen_fd < 0) return;
    int fd = zsock_accept(listen_fd, NULL, NULL);
    if (fd < 0) return;                      /* EWOULDBLOCK -> nothing pending */
    /* bound the read so a silent client can't wedge the reactor */
    struct zsock_timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[1024];
    int n = (int)zsock_recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) { zsock_close(fd); return; }
    buf[n] = '\0';
    char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
    if (sscanf(buf, "%7s %63s", method, path) != 2) { zsock_close(fd); return; }
    http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
}
