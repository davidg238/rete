/* Standalone smoke test for the serial backend (loopback FIFO). Not part of the
   conformance runner; build + run manually from the oevm repo root:
     gcc -std=c99 -Isrc tests/test_serial.c src/backend/posix/serial_posix.c -o /tmp/test_serial && /tmp/test_serial
   Links only the backend (the seam funcs) — not serial.c, which needs the full VM. */
#include "../src/serial_backend.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    serial_open();
    /* loopback FIFO: write then read echoes back, in order */
    serial_write_byte('h'); serial_write_byte('i');
    assert(!serial_at_end());
    assert(serial_read_byte() == 'h');
    assert(serial_read_byte() == 'i');
    assert(serial_at_end());
    assert(serial_read_byte() == -1);   /* empty -> -1 */
    printf("test_serial OK\n");
    return 0;
}
