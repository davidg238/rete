/* Posix (host) HTTP transport hooks: a non-blocking TCP listener + poll +
   socket/stdout emit. All framing/ring/in-flight logic lives in src/http_core.c.
   Single-threaded: http_poll runs on the VM reactor thread inside pumpEvents
   (no accept thread, no mutex). fd == -1 is the injected/test sink -> stdout. */
#define _POSIX_C_SOURCE 200809L
#include "http_backend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>       /* read(), write(), close() */
#include <fcntl.h>        /* fcntl O_NONBLOCK */
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>     /* struct timeval for SO_RCVTIMEO */
#include <netinet/in.h>
#include <arpa/inet.h>

static int listen_fd = -1;

void http_emit(int fd, const char *hdr, int hlen, const char *body, int body_len) {
    if (fd == -1) {                          /* injected/test sink */
        fwrite(hdr, 1, (size_t)hlen, stdout);
        if (body_len > 0) fwrite(body, 1, (size_t)body_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return;
    }
    (void)write(fd, hdr, (size_t)hlen);
    if (body_len > 0) (void)write(fd, body, (size_t)body_len);
    close(fd);
}

int http_listen(int port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    if (listen(listen_fd, 8) < 0) {
        close(listen_fd); listen_fd = -1; return -1;
    }
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);   /* accept() returns EWOULDBLOCK when idle */
    return 0;
}

int http_is_listening(void) { return listen_fd >= 0; }

/* Accept+read at most one pending connection each tick. Non-blocking accept;
   a short SO_RCVTIMEO bounds the read so a connected-but-silent client can never
   wedge the single-threaded reactor. */
void http_poll(void) {
    if (listen_fd < 0) return;
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) return;                      /* EWOULDBLOCK -> nothing pending */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[1024];
    int n = (int)read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';
    char method[HTTP_METHOD_MAX] = {0}, path[HTTP_PATH_MAX] = {0};
    if (!http_parse_reqline(buf, method, HTTP_METHOD_MAX, path, HTTP_PATH_MAX)) { close(fd); return; }
    http_accept_request(fd, method, (int)strlen(method), path, (int)strlen(path));
}
