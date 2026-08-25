/* oevm/src/backend/zephyr/serial_zephyr.c
   Zephyr serial backend. R6's net-capability program never exercises serial, but
   serial.c is in Model B's link set, so this satisfies the link. Same loopback
   byte FIFO as serial_posix.c (target-neutral pure C). A real UART backend can
   replace this in a later rung when a serial capability is demoed on device. */
#include "serial_backend.h"
#include <stddef.h>
#define SERIAL_FIFO_CAP 4096
static unsigned char fifo[SERIAL_FIFO_CAP];
static size_t head = 0, tail = 0, count = 0;
void serial_open(void) { head = tail = count = 0; }
void serial_write_byte(int b) {
    if (count == SERIAL_FIFO_CAP) { head = (head + 1) % SERIAL_FIFO_CAP; count--; }
    fifo[tail] = (unsigned char)(b & 0xff);
    tail = (tail + 1) % SERIAL_FIFO_CAP; count++;
}
int serial_read_byte(void) {
    if (count == 0) return -1;
    int b = fifo[head];
    head = (head + 1) % SERIAL_FIFO_CAP; count--;
    return b;
}
bool serial_at_end(void) { return count == 0; }
