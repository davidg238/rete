/* oevm/oevm_image_main.c
 *
 * Task 10 — standalone image VM entry point. Maps a compiled .oimg, wires the
 * firmware kernel (kernel_boot), then bootstraps the entry instance and runs
 * its main:args: — producing the same output as Model A for the same program.
 */
#include "src/oevm.h"
#include <stdio.h>

int oimg_load(const char *path);
oop oimg_root(void);
oop oimg_bootstrap(oop entry_vtable);

extern void kernel_boot(void);
extern void (*gc_additional_roots_hook)(void);
static void roots_cb(void) {}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <image.oimg>\n", argv[0]); return 2; }
    gc_additional_roots_hook = roots_cb;
    kernel_boot();                       /* ONCE, before load/bootstrap */
    if (oimg_load(argv[1]) != 0) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }

    oimg_bootstrap(oimg_root());
    return 0;
}
