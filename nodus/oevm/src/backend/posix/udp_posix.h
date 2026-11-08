#ifndef OEVM_UDP_POSIX_H
#define OEVM_UDP_POSIX_H
/* Host-only, rete#440: the two halves of the posix doorbell, one in each file.
   udp_posix_poll (udp_posix.c): poll every registered, unparked socket once, with no
   wait, and ring each readable one. Called by the host pulse pump, so a host
   pumpEvents sees an arrival the way a target's reader does.
   pulse_posix_ring (pulse_posix.c): post the socket's constant token, TAG_SMI(fd), on
   its channel. It lives on the pump's side because a socket file cannot include the
   VM's header (oevm.h declares a `send` that collides with <sys/socket.h>). */
void udp_posix_poll(void);
void pulse_posix_ring(int chan_id, int fd);
#endif
