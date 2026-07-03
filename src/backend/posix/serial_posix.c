#include "serial_backend.h"
#include <stddef.h>
/* POSIX (x86-linux / macOS desktop) serial backend: a loopback byte FIFO
   standing in for a real UART (a dev box has none). write appends, read
   consumes (TX->RX echo). Pure ISO C, no OS coupling; serial_zephyr.c /
   serial_esp.c replace it with a real UART on device. Fixed ring buffer; on
   overflow the oldest unread byte is dropped (host test never nears the bound). */
#define SERIAL_FIFO_CAP 4096
static unsigned char fifo[SERIAL_FIFO_CAP];
static size_t head = 0;   /* next read  */
static size_t tail = 0;   /* next write */
static size_t count = 0;
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
