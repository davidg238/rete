#ifndef OEVM_UDP_H
#define OEVM_UDP_H
#include "oevm.h"
oop udp_object(void);                              /* native UDP capability singleton */
oop udp_dispatch(oop selector, int argc, oop *args);
#endif
