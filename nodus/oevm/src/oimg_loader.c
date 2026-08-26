/* oevm/src/oimg_loader.c */
#include "oevm.h"
#include "oimg.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef OEVM_NO_FILE_IO
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
/* Defined here (zero-initialized) so Model A still links; kernel_boot() fills them. */
oop builtin_registry[BUILTIN_COUNT] = {0};
oop oimg_singleton[3] = {0};

/* Registry version — keep in lockstep with gen_builtins.VERSION (Python).
 * Owned here (oimg_loader.c) so every build that links this TU automatically
 * has the definition; kernel_boot.c no longer needs to define it. */
const uint32_t BUILTIN_REGISTRY_VERSION = 5;

/* ---------------- .oimg header byte layout (CONTRACT) ----------------
 * Mirrors tools/oimg_serialize.py OimgWriter.header_bytes(). All scalars
 * little-endian. W = sizeof(oop) (8 on 64-bit, 4 on 32-bit). Ref-words are
 * W-wide, W-aligned. The ref-word block begins at OFF_ROOT, which is the
 * fixed prefix (20 bytes) rounded up to W.
 *
 * format_version 8 (Milestone A Task 9b compaction): ONE fixed layout — the
 * kernel/serial/supervisor/net groups are always present (0 when the entry
 * takes no such capability) and the header ends at the net word (A+12W,
 * padded). The retired v7 tail word (concurrency capability ref at A+13W;
 * fibers removed in Milestone A Task 5) is gone. v3-v7 headers are neither
 * written by the serializer nor accepted here (images are regenerated
 * per-test; no compat needed).
 *
 * format_version 9 (rete#190 stage 2): Symbol name bytes are INLINE in the
 * record (no separate strbytes allocation, no `name` ref-word). Bumped so a
 * v8 image meets a v9 VM as a loud rejection rather than a silent misread.
 *
 * format_version 10 (rete#247): a classnames section rides at the header
 * tail — classnames_ref (app ref-word) at A+13W pointing at a table of
 * (vtable ref-word, name-symbol ref-word) pairs, one per blob vtable, and
 * classnames_count (u16) at A+14W. Read only by oimg_vtable_name() on the
 * DNU abort path, to name an app-class receiver.
 *
 *   off       width  field
 *   0         4      magic "OIMG"
 *   4         2      format_version (u16, = 10)
 *   6         2      flags (u16)
 *   8         4      registry_version (u32)
 *   12        4      blob_size (u32, = total file size)
 *   16        4      cluster_count (u32)
 *   (pad to W)
 *   A         W      root_ref
 *   A+W       W      init_sel_ref
 *   A+2W      W      main_sel_ref
 *   A+3W      2      init_argc (u16)
 *   A+3W+2    2      entry_nslots (u16)
 *   (header padded to 8)
 *   A+4W      W      kernel_vtable_ref   (Kernel module vtable)
 *   A+5W      W      kernel_init_sel_ref (Kernel init selector sym)
 *   A+6W      2      kernel_nslots (u16) (Kernel instance slots)
 *   (header padded to 8 again)
 *   A+7W      W      serial_on_sel_ref   (ref to Kernel>>serialOn:,
 *                                         0 when entry takes no serial capability)
 *   (header padded to 8 again)
 *   A+8W      W      supervisor_vtable_ref
 *   A+9W      W      supervisor_init_sel_ref (init_usingMirror:)
 *   A+10W     W      supervisor_run_sel_ref  (run:platform:args:debug:)
 *   A+11W     2      supervisor_nslots (u16)
 *   (header padded to 8 again)
 *   A+12W     W      net_on_sel_ref   (ref to Kernel>>netOn:,
 *                                      0 when entry takes no net capability)
 *   (header padded to 8 again)
 *   A+13W     W      classnames_ref   (app ref-word to the classnames table,
 *                                      0 when the image has no blob vtables)
 *   A+14W     2      classnames_count (u16)
 *   (header padded to 8 again)
 * For W==8: A=24, init@32, main@40, init_argc@48, entry_nslots@50,
 *           kernel_vtable_ref@56, kernel_init_sel_ref@64, kernel_nslots@72,
 *           header padded to 80, serial_on_sel_ref@80, header padded to 88,
 *           supervisor_vtable_ref@88, supervisor_init_sel_ref@96,
 *           supervisor_run_sel_ref@104, supervisor_nslots@112, header padded to 120,
 *           net_on_sel_ref@120, header padded to 128.
 * ------------------------------------------------------------------- */
#define OIMG_PREFIX_FIXED 20u
#define ALIGN_UP(n, a) (((n) + (a) - 1u) & ~((a) - 1u))

static uintptr_t img_root_ref          = 0;
static uintptr_t img_init_sel_ref      = 0;
static uintptr_t img_main_sel_ref      = 0;
static uint16_t  img_init_argc         = 0;
static uint16_t  img_entry_nslots      = 0;
/* std-lib step 3b: Kernel module descriptor ref-words.
   At a+4W (vtable), a+5W (init selector); kernel_nslots (u16) at a+6W. */
static uintptr_t img_kernel_vtable_ref   = 0;
static uintptr_t img_kernel_init_sel_ref = 0;
static uint16_t  img_kernel_nslots       = 0;
/* std-lib step 3c-3b: ref to Kernel>>serialOn: for minting the serial
   capability. 0 when the entry takes no serial. */
static uintptr_t img_serial_on_sel_ref   = 0;
/* in-image debugger step 1: Supervisor descriptor. */
static uintptr_t img_supervisor_vtable_ref   = 0;
static uintptr_t img_supervisor_init_sel_ref = 0;
static uintptr_t img_supervisor_run_sel_ref  = 0;
static uint16_t  img_supervisor_nslots       = 0;
/* R4: UDP capability. 0 when the entry takes no net. */
static uintptr_t img_net_on_sel_ref          = 0;
/* rete#247: classnames section — names app-class receivers on the DNU line. */
static uintptr_t img_classnames_ref          = 0;
static uint16_t  img_classnames_count        = 0;

static uintptr_t read_word(const uint8_t *p) {
    uintptr_t w = 0;
    memcpy(&w, p, sizeof(oop));   /* sizeof(oop) == oop width; little-endian target */
    return w;
}
static uint32_t read_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static uint16_t read_u16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
}

int oimg_load_blob(const uint8_t *base, size_t size) {
    if (size < OIMG_PREFIX_FIXED) {
        fprintf(stderr, "oimg_load_blob: image too small (%zu bytes)\n", size);
        return 1;
    }
    if (memcmp(base, "OIMG", 4) != 0) {
        fprintf(stderr, "oimg_load_blob: bad magic\n"); return 1;
    }
    uint16_t format_version   = read_u16(base + 4);
    uint32_t registry_version = read_u32(base + 8);
    uint32_t blob_size        = read_u32(base + 12);
    if (format_version != 10) {
        fprintf(stderr, "oimg_load_blob: unsupported format_version %u (expected 10)\n", format_version);
        return 1;
    }
    if (registry_version != BUILTIN_REGISTRY_VERSION) {
        fprintf(stderr, "oimg_load_blob: registry_version %u != firmware %u\n",
                registry_version, BUILTIN_REGISTRY_VERSION);
        return 1;
    }
    if (blob_size != size) {
        fprintf(stderr, "oimg_load_blob: blob_size %u != image size %zu\n", blob_size, size);
        return 1;
    }

    size_t W = sizeof(oop);
    size_t a = ALIGN_UP(OIMG_PREFIX_FIXED, W);
    img_root_ref          = read_word(base + a);
    img_init_sel_ref      = read_word(base + a + W);
    img_main_sel_ref      = read_word(base + a + 2 * W);
    img_init_argc         = read_u16(base + a + 3 * W);
    img_entry_nslots      = read_u16(base + a + 3 * W + 2);
    /* v8 fixed layout: every descriptor group is always present (0 when the
       entry takes no such capability), so read them unconditionally. */
    img_kernel_vtable_ref   = read_word(base + a + 4 * W);
    img_kernel_init_sel_ref = read_word(base + a + 5 * W);
    img_kernel_nslots       = read_u16 (base + a + 6 * W);
    img_serial_on_sel_ref   = read_word(base + a + 7 * W);
    img_supervisor_vtable_ref   = read_word(base + a + 8 * W);
    img_supervisor_init_sel_ref = read_word(base + a + 9 * W);
    img_supervisor_run_sel_ref  = read_word(base + a + 10 * W);
    img_supervisor_nslots       = read_u16 (base + a + 11 * W);
    img_net_on_sel_ref          = read_word(base + a + 12 * W);
    img_classnames_ref          = read_word(base + a + 13 * W);
    img_classnames_count        = read_u16 (base + a + 14 * W);
    /* Closed decoding: a classnames table that points outside the blob is a
       malformed image, not a degraded diagnostic. */
    if (img_classnames_count != 0) {
        size_t off = (size_t)(img_classnames_ref >> 2);
        if (img_classnames_ref == 0 ||
            off + (size_t)img_classnames_count * 2 * W > blob_size) {
            fprintf(stderr, "oimg_load_blob: classnames table out of bounds\n");
            return 1;
        }
    }

    oimg_blob_base = base;
    oimg_blob_end  = base + blob_size;
    return 0;
}

/* rete#247: name an app-class vtable from the image's classnames section.
   NULL when `vt` is not a blob vtable or the table has no row for it — the
   DNU printer then falls back to the oop/vtable hex pair. Abort-path only;
   a linear scan over tens of rows is fine. */
const char *oimg_vtable_name(oop vt) {
    if (vt == NULL || oimg_blob_base == NULL || !is_in_blob(vt)) return NULL;
    size_t W = sizeof(oop);
    const uint8_t *tab = oimg_blob_base + (img_classnames_ref >> 2);
    for (uint16_t i = 0; i < img_classnames_count; i++) {
        if (oimg_resolve(read_word(tab + (size_t)2 * i * W)) == vt) {
            struct Symbol *s =
                (struct Symbol *)oimg_resolve(read_word(tab + ((size_t)2 * i + 1) * W));
            return s ? symbol_name(s) : NULL;
        }
    }
    return NULL;
}

#ifndef OEVM_NO_FILE_IO
int oimg_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("oimg_load: open"); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("oimg_load: fstat"); close(fd); return 1; }
    size_t sz = (size_t)st.st_size;
    void *map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { perror("oimg_load: mmap"); return 1; }
    int rc = oimg_load_blob((const uint8_t *)map, sz);
    if (rc != 0) munmap(map, sz);   /* keep the mapping for the process lifetime on success */
    return rc;
}
#endif

oop oimg_root(void) {
    return oimg_resolve(img_root_ref);
}

/* std-lib step 3b: resolve the Kernel module vtable from the loaded image.
   Returns NULL when the image has no Kernel descriptor (format < 3). */
oop oimg_kernel_vtable(void) {
    return oimg_resolve(img_kernel_vtable_ref);
}

oop oimg_bootstrap(oop entry_vtable) {
    extern oop send(oop receiver, oop selector, int argc, oop *args);
    extern oop gc_allocate(oop vtable, size_t size);
    /* oevm#21: pin boot instances as PRECISE GC roots — they are held in C locals
       across the send sequence below, and the conservative C-stack scan that would
       otherwise root them is defeated by ASan's fake-stack, so a GC_STRESS collection
       mid-boot can sweep them and corrupt dispatch. Cleared before every return. */
    extern void gc_temp_root_push(oop o);
    extern void gc_temp_roots_clear(void);

    /* std-lib step 3b: (1) native platform; (2) construct Kernel usingPlatform:
       <native>; (3) hand the Kernel INSTANCE to the app as its `platform`. The
       Kernel signature is fixed by the design (usingPlatform: -> 1 cap param). */
    oop native_platform = platform_object();
    gc_temp_root_push(native_platform);
    oop kernel_inst = gc_allocate(oimg_kernel_vtable(),
                                  sizeof(struct ObjectInstance) + img_kernel_nslots * sizeof(oop));
    gc_temp_root_push(kernel_inst);
    oop kernel_init_args[1] = { native_platform };
    send(kernel_inst, oimg_resolve(img_kernel_init_sel_ref), 1, kernel_init_args);

    oop inst = gc_allocate(entry_vtable,
                           sizeof(struct ObjectInstance) + img_entry_nslots * sizeof(oop));
    gc_temp_root_push(inst);

    /* Mint the 2nd-capability wrapper when the entry declares `serial:` or `net:`.
       At most one of the *_on_sel_ref values is non-zero per entry (serial XOR net);
       both are 0 for entries with no cap parameter. The `concurrency:` capability was
       removed with fibers (Milestone A Task 5). */
    oop second_cap = NULL;
    if (img_init_argc >= 2 && img_serial_on_sel_ref != 0) {
        extern oop serial_object(void);
        oop serial_args[1] = { serial_object() };
        second_cap = send(kernel_inst, oimg_resolve(img_serial_on_sel_ref), 1, serial_args);
    } else if (img_init_argc >= 2 && img_net_on_sel_ref != 0) {
        /* R4: UDP capability. Send #netOn: to kernel_inst with the native udp_object()
           to get a Newspeak Net instance (mirrors the serial pattern). */
        extern oop udp_object(void);
        oop net_args[1] = { udp_object() };
        second_cap = send(kernel_inst, oimg_resolve(img_net_on_sel_ref), 1, net_args);
    }
    gc_temp_root_push(second_cap);   /* live until the entry init send below (oevm#21) */
    oop init_args[3] = { kernel_inst, second_cap, TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);

    /* in-image debugger step 1: when the image carries a Supervisor descriptor,
       mint the Supervisor, grant it the native mirror, and route the app launch
       through it (gated by OEVM_DEBUG). Supervisor-less images have all
       img_supervisor_* refs == 0 (oimg_resolve(0) -> NULL), so fall back to
       the original direct main:args: launch — mirroring the kernel/serial guards. */
    if (img_supervisor_vtable_ref != 0) {
        extern oop mirror_object(void);
        oop supervisor_inst = gc_allocate(oimg_resolve(img_supervisor_vtable_ref),
                                          sizeof(struct ObjectInstance) + img_supervisor_nslots * sizeof(oop));
        gc_temp_root_push(supervisor_inst);
        oop sup_init_args[1] = { mirror_object() };
        send(supervisor_inst, oimg_resolve(img_supervisor_init_sel_ref), 1, sup_init_args);

        oop dbg_flag = getenv("OEVM_DEBUG") ? TAG_SMI(1) : TAG_SMI(0);
        oop run_args[4] = { inst, kernel_inst, TAG_SMI(0), dbg_flag };
        oop res = send(supervisor_inst, oimg_resolve(img_supervisor_run_sel_ref), 4, run_args);
        gc_temp_roots_clear();
        return res;
    } else {
        /* pre-v5 image: original direct launch */
        oop main_args[2] = { kernel_inst, TAG_SMI(0) };
        oop res = send(inst, oimg_resolve(img_main_sel_ref), 2, main_args);
        gc_temp_roots_clear();
        return res;
    }
}
