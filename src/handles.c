/* M6 C-boundary handle table — see src/handles.h. Storage mirrors src/space.c's
   static-table idiom (fixed array, -D capacity, bare-frozen-string abort on full);
   marking mirrors space_gc_trace (iterate live slots, mark_object each). */
#include "handles.h"
#include <stdio.h>
#include <stdlib.h>

extern void mark_object(oop ptr);   /* gc.c:630 — the exported push-only root marker */

struct HandleSlot { oop value; int live; };
static struct HandleSlot handles[OEVM_HANDLE_TABLE_MAX];

int handle_new(oop o) {
    int i;
    for (i = 0; i < OEVM_HANDLE_TABLE_MAX; i++) {
        if (!handles[i].live) {
            handles[i].value = o;
            handles[i].live = 1;
            return i;
        }
    }
    fprintf(stderr, "handle table full\n");
    abort();
    return -1;   /* unreachable; silences -Wreturn-type */
}

oop handle_get(int idx) {
    if (idx < 0 || idx >= OEVM_HANDLE_TABLE_MAX) return NULL;
    if (!handles[idx].live) return NULL;
    return handles[idx].value;
}

void handle_free(int idx) {
    if (idx < 0 || idx >= OEVM_HANDLE_TABLE_MAX) return;
    handles[idx].live = 0;
    handles[idx].value = NULL;
}

void handle_gc_trace(void) {
    int i;
    for (i = 0; i < OEVM_HANDLE_TABLE_MAX; i++)
        if (handles[i].live) mark_object(handles[i].value);
}
