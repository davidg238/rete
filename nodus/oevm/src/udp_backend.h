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
   *sender_port. Returns payload length (>=0), or -1 when no datagram waiting, which
   restores the socket's doorbell (udp_doorbell below). */
int  udp_receive(int sock, uint8_t *buf, int buf_cap,
                 char *host_out, int host_cap, int *sender_port);
void udp_close(int sock);
/* rete#440, the doorbell. Ring chan_id once a datagram on sock can be read (never
   before: the reaction reads until empty, so a bell that overtook its payload would
   lose the wakeup), then leave sock alone until udp_receive answers empty on it,
   which restores it. The token is TAG_SMI(sock), a constant per channel, so a burst
   coalesces into one pending wakeup. Registering a sock already registered re-arms
   it: that is how a listener restores (http_poll answering 0). Only the VM's sockets
   (udp.c basicBind:) and the http listener register; udp_close forgets one.
   Answers 0, or -1 when sock cannot have a bell (no room, no reader): a socket that
   never rings is a deaf node, so the caller fails its bind rather than keep it. */
int  udp_doorbell(int sock, int chan_id);
/* rete#440 R3 (#458), the link: whether the link that carries the VM's datagrams is up
   (BLE: the relay's channel open; Thread: attached; WiFi: associated; host: the radio
   on). net_link_doorbell registers sys/net/link's channel; from then on every change of
   the link rings it, token TAG_SMI(0), a constant (design "Arrivals", invariant 2), and
   the reaction reads net_link_up for the state. Registering again re-points it. */
void net_link_doorbell(int chan_id);
int  net_link_up(void);
#endif
