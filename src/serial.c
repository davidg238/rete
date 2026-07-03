#include "serial.h"
#include "serial_backend.h"
#include "oimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern oop VtableVtable;
extern const struct Symbol true_sym_obj, false_sym_obj;

static struct Object SerialObj;
static struct Vtable SerialVtable;
static int serial_initialized = 0;

oop serial_object(void) {
    if (!serial_initialized) {
        serial_open();
        SerialVtable.base.vtable = VtableVtable;
        SerialVtable.delegate = NULL; SerialVtable.bindings = NULL;
        SerialVtable.capacity = 0; SerialVtable.size = 0; SerialVtable.is_nested = 0;
        SerialObj.vtable = (oop)&SerialVtable;
        serial_initialized = 1;
    }
    return (oop)&SerialObj;
}

oop serial_dispatch(oop selector, int argc, oop *args) {
    (void)argc;
    const char *sel = symbol_name((struct Symbol *)selector);
    if (strcmp(sel, "basicReadByte") == 0) {
        int b = serial_read_byte();
        return (b < 0) ? NULL : TAG_SMI(b);          /* nil when empty */
    }
    if (strcmp(sel, "basicWriteByte:") == 0) {
        if (IS_SMI(args[0])) serial_write_byte((int)UNTAG_SMI(args[0]));
        return (oop)&SerialObj;
    }
    if (strcmp(sel, "basicAtEnd") == 0) {
        return serial_at_end() ? (oop)&true_sym_obj : (oop)&false_sym_obj;
    }
    /* Unknown selector on the serial capability: DNU. */
    fprintf(stderr, "VM Error: Serial doesNotUnderstand #%s\n", sel);
    abort();
}
