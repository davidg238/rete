#ifndef OEVM_UDP_BACKEND_H
#define OEVM_UDP_BACKEND_H
#include <stdint.h>
/* Fixed per-target seam. Exactly one impl is linked per build from
   src/backend/$OEVM_TARGET/: backend/posix/udp_posix.c (real dual-stack POSIX
   sockets) now; backend/zephyr/udp_zephyr.c (Zephyr net / Thread, IPv6) later.
   Addresses are plain strings (IPv4 dotted or IPv6); the backend owns the family. */
void udp_open(void);                       /* idempotent init at boot */
int  udp_socket_bind(int port);            /* fd >=0, or -1; port 0 = ephemeral */
int  udp_send(int sock, const char *host, int port,
              const uint8_t *data, int len);   /* bytes sent, or -1 */
/* Non-blocking. Fills buf (payload), host_out (numeric sender, NUL-terminated),
   *sender_port. Returns payload length (>=0), or -1 when no datagram waiting. */
int  udp_receive(int sock, uint8_t *buf, int buf_cap,
                 char *host_out, int host_cap, int *sender_port);
void udp_close(int sock);
#endif
