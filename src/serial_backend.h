#ifndef OEVM_SERIAL_BACKEND_H
#define OEVM_SERIAL_BACKEND_H
#include <stdbool.h>
/* Fixed per-target seam. Exactly one impl is linked per build from
   src/backend/$OEVM_TARGET/: backend/posix/serial_posix.c (loopback mock) now;
   backend/zephyr/serial_zephyr.c / backend/esp32/serial_esp.c later. */
void serial_open(void);        /* idempotent init at boot */
int  serial_read_byte(void);   /* next byte 0..255, or -1 when no data available */
void serial_write_byte(int b); /* b in 0..255 */
bool serial_at_end(void);      /* true when no byte currently available */
#endif
