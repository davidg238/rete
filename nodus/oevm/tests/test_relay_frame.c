/*
 * test_relay_frame.c — the relay frame codec (src/relay_frame.h) against the golden
 * vector in protocol/fixtures/relay/frame.hex, the same file the relay app's
 * FrameTest.kt reads (protocol/RELAY-BLE.md, rete#439).
 *
 * Standalone, header-only, run from nodus/oevm:
 *   cc -std=c99 -Isrc tests/test_relay_frame.c -o /tmp/test_relay_frame && /tmp/test_relay_frame
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/relay_frame.h"

#define VECTOR "../../protocol/fixtures/relay/frame.hex"
static const char HOST[] = "fd7a:115c:a1e0::1";

static size_t read_vector(uint8_t *out, size_t cap) {
    FILE *f = fopen(VECTOR, "r");
    assert(f != NULL);
    size_t n = 0;
    unsigned b;
    while (n < cap && fscanf(f, "%2x", &b) == 1) out[n++] = (uint8_t)b;
    fclose(f);
    return n;
}

static void test_encode_matches_vector(void) {
    uint8_t want[64], got[64];
    size_t n = read_vector(want, sizeof want);
    int h = relay_frame_put_header(got, sizeof got, 16970, 6969, HOST, strlen(HOST));
    assert(h == RELAY_FRAME_HDR + (int)strlen(HOST));
    memcpy(got + h, "hi", 2);
    assert(n == (size_t)h + 2);
    assert(memcmp(want, got, n) == 0);
}

static void test_parse_vector(void) {
    uint8_t v[64];
    size_t n = read_vector(v, sizeof v);
    struct relay_frame f;
    assert(relay_frame_parse(v, n, &f) == 0);
    assert(f.local_port == 16970 && f.remote_port == 6969);
    assert(f.host_len == strlen(HOST) && memcmp(v + f.host_off, HOST, f.host_len) == 0);
    assert(f.payload_len == 2 && memcmp(v + f.payload_off, "hi", 2) == 0);
}

static void test_malformed(void) {
    struct relay_frame f;
    uint8_t short_hdr[] = {0, 1, 0, 2};
    uint8_t host_past_end[] = {0, 1, 0, 2, 5, 'a'};
    uint8_t empty_host[] = {0, 1, 0, 2, 0};
    assert(relay_frame_parse(short_hdr, sizeof short_hdr, &f) == -1);
    assert(relay_frame_parse(host_past_end, sizeof host_past_end, &f) == -1);
    assert(relay_frame_parse(empty_host, sizeof empty_host, &f) == 0 && f.payload_len == 0);
}

static void test_put_header_refuses(void) {
    uint8_t out[8];
    char long_host[256];
    memset(long_host, 'a', sizeof long_host);
    assert(relay_frame_put_header(out, sizeof out, 1, 2, long_host, 256) == -1);
    assert(relay_frame_put_header(out, sizeof out, 1, 2, HOST, strlen(HOST)) == -1);   /* too small */
}

int main(void) {
    test_encode_matches_vector();
    test_parse_vector();
    test_malformed();
    test_put_header_refuses();
    printf("test_relay_frame: OK\n");
    return 0;
}
