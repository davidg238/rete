#ifndef OEVM_PLATFORM_H
#define OEVM_PLATFORM_H
#include "oevm.h"
/* platform_object() is already declared in oevm.h. */
/* Returns 1 and writes *out if pid is a platform primitive; else 0. */
int platform_dispatch(int pid, int argc, oop *args, oop *out);
/* An n-slot indexable (struct ObjectInstance: slots[0] = TAG_SMI length, slots[1..n]
   the elements, 1-based). Can GC. */
oop indexable_new(intptr_t n);
/* rete#273 D1: the app's declared cadence (ms), 0 = undeclared. */
uint32_t platform_app_cad_ms(void);
/* The platform-facility pid range, defined in dispatch.c's enum and shared so
   the hook can pass pid through. (Kept in dispatch.c's enum; see Step 5.) */
#endif
