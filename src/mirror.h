#ifndef OEVM_MIRROR_H
#define OEVM_MIRROR_H
#include "oevm.h"
oop mirror_object(void);                                 /* lazy &MirrorObj singleton */
oop mirror_dispatch(oop selector, int argc, oop *args);  /* confined reflection */
#endif
