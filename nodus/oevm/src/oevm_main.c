/* Model A appliance build: compile only when -DOEVM_APPLIANCE is set.
 * The default (Model B) build uses oevm_image_main.c instead. */
#ifdef OEVM_APPLIANCE

#include "oevm.h"
#include "oimg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Stub XIP globals — Model A never loads a blob; oimg.h accessors check
 * is_in_blob() first, which returns 0 when blob_base==NULL, so the XIP
 * paths are never taken.  Definitions must exist here because oimg_loader.c
 * is NOT compiled in the appliance build. */
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3]    = {0};

/* rete#247: no blob means no classnames section — an app-class DNU keeps the
 * oop/vtable hex pair in the appliance build. */
const char *oimg_vtable_name(oop vt) { (void)vt; return NULL; }

/* rete#75: no blob means no symbol table — every decoded name is refused. Model A
 * is being retired with this known-broken (#336). */
oop oimg_symbol_lookup(const char *bytes, size_t len) { (void)bytes; (void)len; return NULL; }

extern void init_solidified_vtables(void);
extern void init_reflection_symbols(void);
extern void reboot_note_init(void);
extern oop run_solidified_entry_point(void);
extern void (*gc_additional_roots_hook)(void);

/* Global roots list callback if needed */
static void oevm_roots_callback(void) {
    /* Standard GC roots are automatically scanned by the GC (vm_stack, current_frame, g_active_stack) */
}

int main(void) {
    /* Set roots callback */
    gc_additional_roots_hook = oevm_roots_callback;

    /* rete#114: same boot order as zephyr/src/main.c and oevm_image_main.c —
       the retained note is validated (or planted, host seam) before the VM
       can read it. */
    reboot_note_init();

    /* Boot solidified image */
    init_solidified_vtables();
    init_reflection_symbols();

    /* Execute the solidified entry point */
    oop result = run_solidified_entry_point();
    (void)result;

    return 0;
}

#endif /* OEVM_APPLIANCE */
