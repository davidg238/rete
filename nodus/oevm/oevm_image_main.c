/* oevm/oevm_image_main.c
 *
 * Task 10 — standalone image VM entry point. Maps a compiled .oimg, wires the
 * firmware kernel (kernel_boot), then bootstraps the entry instance and runs
 * its main:args: — producing the same output as Model A for the same program.
 */
#include "src/oevm.h"
#include "src/ota.h"
#include "src/oimg.h"
#include <stdio.h>
#include <stdlib.h>


extern void kernel_boot(void);
extern void reboot_note_init(void);
/* rete#367 (peer design D14): the board's two boot lines, so a load on a custos
   `--host` board proves a host load the way it proves a board's. "host" is
   platform.c's OEVM_BUILD_ID for a host build. On stderr: a test's expected
   output is its stdout. */
static void say_program_hash(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1), h[32];
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        ota_sha256(buf, n, h);
        fprintf(stderr, "oevm: program sha256 ");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", h[i]);
        fprintf(stderr, "\n");
    }
    free(buf);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <image.oimg>\n", argv[0]); return 2; }
    /* rete#114: mirror zephyr/src/main.c's boot order — validate (or, via the
       host-only OEVM_TEST_PANIC_NOTE seam, plant) the retained note before the
       VM can read it. On the host this is what makes the seam live at all. */
    reboot_note_init();
    fprintf(stderr, "oevm: build-id host\n");
    kernel_boot();                       /* ONCE, before load/bootstrap */
    if (oimg_load(argv[1]) != 0) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }
    say_program_hash(argv[1]);

    oimg_bootstrap(oimg_root());
    return 0;
}
