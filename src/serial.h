#ifndef OEVM_SERIAL_H
#define OEVM_SERIAL_H
#include "oevm.h"
oop serial_object(void);                                  /* lazy &SerialObj singleton */
oop serial_dispatch(oop selector, int argc, oop *args);   /* basicReadByte/basicWriteByte:/basicAtEnd */
#endif
