#ifndef OEVM_PLATFORM_H
#define OEVM_PLATFORM_H
#include "oevm.h"
/* platform_object() is already declared in oevm.h. */
/* Returns 1 and writes *out if pid is a platform primitive; else 0. */
int platform_dispatch(int pid, int argc, oop *args, oop *out);
/* The platform-facility pid range, defined in dispatch.c's enum and shared so
   the hook can pass pid through. (Kept in dispatch.c's enum; see Step 5.) */
#endif
