/* oevm/src/relay_frame.h — the relay frame codec, rete#439. The contract is
   protocol/RELAY-BLE.md; udp_ble.c is its one caller on a node, and
   tests/test_relay_frame.c holds it to the golden vector the relay app also tests.

   Header-only and allocation-free, so the host test needs no Zephyr. */
#ifndef OEVM_RELAY_FRAME_H
#define OEVM_RELAY_FRAME_H
#include <stddef.h>
#include <stdint.h>

#define RELAY_FRAME_HDR 5                   /* u16 local | u16 remote | u8 host_len */
#define RELAY_FRAME_HDR_MAX (RELAY_FRAME_HDR + 255)

struct relay_frame {
    uint16_t local_port, remote_port;
    uint8_t host_len;
    size_t host_off, payload_off, payload_len;   /* offsets into the frame */
};

/* Writes the header and host into out; answers the bytes written (the payload goes
   after them), or -1 when the host is longer than 255 or out is too small. */
static inline int relay_frame_put_header(uint8_t *out, size_t cap, uint16_t local_port,
                                         uint16_t remote_port, const char *host, size_t host_len) {
    if (host_len > 255 || cap < RELAY_FRAME_HDR + host_len) return -1;
    out[0] = (uint8_t)(local_port >> 8);  out[1] = (uint8_t)local_port;
    out[2] = (uint8_t)(remote_port >> 8); out[3] = (uint8_t)remote_port;
    out[4] = (uint8_t)host_len;
    for (size_t i = 0; i < host_len; i++) out[RELAY_FRAME_HDR + i] = (uint8_t)host[i];
    return (int)(RELAY_FRAME_HDR + host_len);
}

/* Parses the frame of frame_len bytes starting at buf. Reads only the header and host,
   so buf need hold no more than RELAY_FRAME_HDR_MAX of it. Answers 0, or -1 when the
   frame is malformed (shorter than its header, or its host runs past the end). */
static inline int relay_frame_parse(const uint8_t *buf, size_t frame_len, struct relay_frame *f) {
    if (frame_len < RELAY_FRAME_HDR) return -1;
    f->local_port = (uint16_t)(buf[0] << 8 | buf[1]);
    f->remote_port = (uint16_t)(buf[2] << 8 | buf[3]);
    f->host_len = buf[4];
    if (frame_len < (size_t)RELAY_FRAME_HDR + f->host_len) return -1;
    f->host_off = RELAY_FRAME_HDR;
    f->payload_off = RELAY_FRAME_HDR + f->host_len;
    f->payload_len = frame_len - f->payload_off;
    return 0;
}

#endif
