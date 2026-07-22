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
#include <zephyr/kernel.h>       /* k_yield / k_msleep, printk */
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/posix/fcntl.h>  /* F_SETFL, O_NONBLOCK (not pulled in by net/socket.h) */
#include <string.h>
#include <errno.h>               /* EAGAIN (partial-send retry) */

static int listen_fd = -1;

/* Send every byte in bounded chunks. Tight TX net-buf pools can't absorb a 4 KB
   body at once, so zsock_send returns partial counts or a transient buffer error
   (EAGAIN/EWOULDBLOCK/ENOMEM) until segments are ACKed and buffers recycle — retry
   those with a short yield (which also gives the net TX path CPU). Returns 0 when
   all n bytes are queued, -1 only on a real peer error. */
#define HTTP_SEND_CHUNK 512
static int send_all(int fd, const char *p, int n) {
    int off = 0, spins = 0;
    while (off < n) {
        int want = n - off; if (want > HTTP_SEND_CHUNK) want = HTTP_SEND_CHUNK;
        int w = (int)zsock_send(fd, p + off, (size_t)want, 0);
        if (w > 0) { off += w; spins = 0; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)) {
            if (++spins > 2000) return -1;    /* wedged ~2 s — give up on this peer */
            k_msleep(1); continue;            /* buffers busy — let TX drain */
        }
        return -1;                            /* peer gone (ENOTCONN/RST) */
    }
    return 0;
}

/* Write all len bytes, non-closing, via send_all's cooperative-yield
   backpressure loop. fd == -1 never occurs on device (no injected sink) ->
   no-op success, matching the old http_emit's fd==-1 short-circuit. */
int http_write(int fd, const char *buf, int len) {
    if (fd == -1) return 0;                  /* no injected sink on device */
    return send_all(fd, buf, len);
}

void http_close(int fd) {
    if (fd != -1) zsock_close(fd);
}

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) return;
    if (http_write(fd, hdr, hlen) == 0 && body_len > 0)
        (void)http_write(fd, body, body_len);
    http_close(fd);
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
    /* Backlog 2, not 4 (#65). A backlogged connection is fully ESTABLISHED and
       pins the RX net_pkts carrying its request until the app accepts it, so the
       backlog depth is a direct claim on CONFIG_NET_PKT_RX_COUNT (8). At 4 the
       backlog alone could pin the whole RX slab, and once it is empty
       zsock_accept() can never succeed again — the deadlock below. This does not
       reduce the number of connections served, only how many may queue
       un-accepted; http_poll() drains one per reactor tick either way. */
    if (zsock_listen(listen_fd, 2) < 0) {
        zsock_close(listen_fd); listen_fd = -1; return -1;
    }
    /* Keep-alive on the LISTENING socket (#65) — this is the deadlock breaker.
       The stuck connections observed on target were idle ESTABLISHED backlog
       entries, and with keep-alive off an idle ESTABLISHED connection has NO
       timer of any kind: nothing was ever going to reap them, so the node stayed
       deaf until a hardware reset.
       Set here rather than after accept() because these connections are never
       accepted — Zephyr copies the keep-alive params from the listening conn when
       an incoming connection reaches ESTABLISHED (tcp.c keep_alive_param_copy),
       so setting it on the listener is what reaches a backlog entry at all.
       After keep_cnt unanswered probes tcp_send_keepalive_probe() calls
       tcp_conn_close(-ETIMEDOUT), which frees the conn and its RX packets WITHOUT
       needing to receive anything — that is what makes it break the loop rather
       than depend on it.
       The Kconfig defaults are useless here (idle 7200 s); a dead peer must be
       reaped in seconds, not hours, so idle/intvl/cnt are set explicitly: first
       probe after 10 s idle, then every 5 s, giving up after 3 => ~25 s to
       reclaim. Best-effort: a build without CONFIG_NET_TCP_KEEPALIVE answers
       -ENOPROTOOPT and the node still runs, just without the reaper. */
    int ka = 1, idle = 10, intvl = 5, cnt = 3;
    if (zsock_setsockopt(listen_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof ka) < 0) {
        printk("http: SO_KEEPALIVE unavailable (errno %d) — no idle-connection reaper\n",
               errno);
    } else {
        zsock_setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof idle);
        zsock_setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        zsock_setsockopt(listen_fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof cnt);
    }
    zsock_fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

/* #65: accept() failures that are NOT "nothing pending". Counted rather than
   ignored because the two were previously indistinguishable — see http_poll. */
static uint32_t accept_fail_count;
uint32_t http_accept_failures(void) { return accept_fail_count; }

int http_is_listening(void) { return listen_fd >= 0; }

void http_poll(void) {
    if (listen_fd < 0) return;
    int fd = zsock_accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        /* EAGAIN/EWOULDBLOCK is the ONLY benign case: nothing pending. The nsl
           reactor loop `[ true ] whileTrue: [ pumpEvents. space drain ]`
           busy-spins with no sleep; on Zephyr that starves the cooperative
           WiFi/TCP driver threads, so responses never flush and new connections
           never get accepted. Yield the CPU here (idle path only) so the net
           stack runs — a device-transport concern, invisible to nsl.

           Anything else is resource exhaustion, and folding it into the same
           early return is what made #65 silent: with the RX packet slab empty
           zsock_accept() cannot succeed, and the node spun here forever
           "accepting nothing" with no crash and no log, indistinguishable from an
           idle node. prj_esp32.conf predicted exactly this in prose for context
           exhaustion; it came true via RX-packet exhaustion.

           Counted and announced (rate-limited to the first and every 1000th, so a
           wedged node says something without flooding a console that may be the
           only way in). Not fatal: the keep-alive reaper armed in http_listen is
           what actually clears the condition, and this is how an operator — or a
           supervisor policy — can see that it happened. */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (accept_fail_count % 1000 == 0)
                printk("http: accept failed errno=%d (n=%u) — listener cannot accept, "
                       "likely RX packet exhaustion\n", errno, accept_fail_count + 1);
            accept_fail_count++;
        }
        k_msleep(1);
        return;
    }
    /* bound the read so a silent client can't wedge the reactor */
    struct zsock_timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    zsock_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[1024];
    int n = (int)zsock_recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) { zsock_close(fd); return; }
    buf[n] = '\0';
    char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
    if (!http_parse_reqline(buf, method, HTTP_METHOD_MAX, path, HTTP_PATH_MAX)) { zsock_close(fd); return; }
    http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
}
