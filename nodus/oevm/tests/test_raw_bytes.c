/* Unit test for RawBytes leaf storage (src/dispatch.c, src/platform.c,
   src/kernel_boot.c — Milestone B Task 3). RawBytes reuses the String C
   layout (struct String: length + inline bytes, pointer-free leaf) but is a
   DISTINCT builtin vtable, so:
     - String does NOT gain `at:put:` (only RawBytesVtable receivers do).
     - GC must trace RawBytes objects the same leaf-shape way it already
       traces String (gc.c trace_one/gc_collect): this test forces a
       collection with a rooted RawBytes + frozen-copy pair live to prove
       that wiring, since a regression here would show up only as silent
       heap corruption, not a compile error.

   Build (Model B link set, mirrors tests/test_platform.c):
     gcc -std=c99 -Isrc tests/test_raw_bytes.c \
         src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/udp.c \
         src/mirror.c src/backend/posix/platform_posix.c \
         src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
         src/oimg_loader.c src/kernel_boot.c -lm -o /tmp/trb && /tmp/trb

   Properties (brief Step 1):
     1. `platform basicNewBytes: 8` -> a RawBytesVtable object, size 8, all
        bytes zero-filled.
     2. write/read roundtrip: `at:put:` then `at:` agree, byte 1 and byte 8
        (1-based indexing).
     3. out-of-bounds index (`at:` and `at:put:`, low 0 and high past-size)
        aborts with the frozen string `index out of bounds` — exact bytes.
     4. out-of-range byte value (`at:put:` with a valid index) aborts with
        the frozen string `byte out of range` — exact bytes.
     5. `platform freezeBytes: raw` yields a DISTINCT object (not the same
        oop) with equal bytes.
     6. writing the original after freezing does not change the frozen copy
        (freeze-is-copy, spec section 6) — and vice versa.
     7. (discipline check, brief self-review) a String receiver must NOT
        gain `at:put:` — sending it still DNUs, proving the RawBytesVtable
        gate in dispatch.c's PRIM_at_put case is exclusive.
     8. (GC landmine check) a rooted RawBytes + its frozen copy survive a
        forced gc_collect() with bytes intact — proves the leaf-tracing
        wiring in gc.c (trace_one) is correct for the new vtable, not just
        for StringVtable. */
#include "../src/oevm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

extern oop  send(oop receiver, oop selector, int argc, oop *args);
extern void kernel_boot(void);
extern void heap_init(void);
extern oop  SymbolVtable;
extern oop  StringVtable;
extern oop  RawBytesVtable;   /* the new builtin vtable this task adds */
extern oop  platform_object(void);

extern void gc_collect(void);
extern int  gc_is_live(oop ptr);
extern void mark_object(oop ptr);
extern void (*gc_additional_roots_hook)(void);

/* Build (and INTERN) a selector Symbol (mirrors tests/test_platform.c's sel()).

   rete#226/#275: symbol_new is THE Symbol constructor — it bakes prim_id from the
   name, and the send path reads that field rather than looking the name up. A
   hand-built Symbol carries an uninitialised prim_id and is never recognised as a
   primitive, which is how this test aborted with "Message 'basicNewBytes:' not
   understood". Never open-code a Symbol.

   The constructor allocates on the GC heap (the old malloc'd Symbols were
   off-heap and so could never be swept), and the collector has no C-stack scan,
   so every selector is interned into g_sels and marked by raw_bytes_roots below.
   mark() ignores NULL and off-heap pointers, so this is safe before the first
   intern. */
#define SEL_MAX 32
static oop g_sels[SEL_MAX];
static int g_nsels = 0;
static oop sel(const char *name) {
    for (int i = 0; i < g_nsels; i++)
        if (strcmp(((struct Symbol *)g_sels[i])->name, name) == 0) return g_sels[i];
    assert(g_nsels < SEL_MAX);
    oop s = symbol_new(name, strlen(name));
    assert(s != NULL);
    g_sels[g_nsels++] = s;
    return s;
}

/* ---- fork-and-expect-abort harness --------------------------------------
   abort() kills the whole process, so a crash property can't run in-process
   alongside the rest of the suite (test_oimg.c's precedent explicitly notes
   the abort() branch "cannot be tested in-process"). Fork instead: the child
   performs exactly one send() and either aborts (expected) or falls through
   to _exit(42) (a bug — no crash happened). The parent captures the child's
   stderr via a pipe and checks both the signal AND the exact frozen text. */
static oop g_recv;
static char g_selname[32];
static int  g_argc;
static oop  g_args[2];

static void do_send_child(void) {
    (void)send(g_recv, sel(g_selname), g_argc, g_args);
    _exit(42);   /* reached only if the expected abort did NOT happen */
}

static void expect_abort(const char *label, const char *expected_err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); _exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); _exit(1); }
    if (pid == 0) {
        /* The child aborts on purpose: no core dump, and no crash notification. 1, not
           0: with a piped core_pattern (systemd-coredump) the kernel still runs the
           helper at 0, and skips it only at exactly 1. */
        setrlimit(RLIMIT_CORE, &(struct rlimit){1, 1});
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        do_send_child();
        /* unreachable: do_send_child always _exit()s */
    }
    close(pipefd[1]);
    char buf[512];
    ssize_t total = 0, r;
    while (total < (ssize_t)sizeof(buf) - 1 &&
           (r = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += r;
    }
    buf[total] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)) {
        fprintf(stderr, "FAIL %s: expected child to abort via SIGABRT, "
                "got status=%d, captured stderr=[%s]\n", label, status, buf);
        exit(1);
    }
    if (expected_err != NULL && strstr(buf, expected_err) == NULL) {
        fprintf(stderr, "FAIL %s: expected stderr to contain '%s', "
                "got=[%s]\n", label, expected_err, buf);
        exit(1);
    }
    printf("test_raw_bytes: %s PASS\n", label);
}

/* Roots hook for the GC property: keep g_raw/g_frozen alive across a forced
   collection (mirrors tests/test_platform.c's platform_roots). */
static oop g_raw = NULL;
static oop g_frozen = NULL;
static void raw_bytes_roots(void) {
    mark_object(g_raw);
    mark_object(g_frozen);
    /* rete#275: selectors are heap objects now and must survive the collection. */
    for (int i = 0; i < g_nsels; i++) mark_object(g_sels[i]);
}

int main(void) {
    heap_init();
    kernel_boot();
    /* rete#275: installed for the WHOLE test, not just the Task 8 GC block —
       selectors are heap objects and must be rooted at every send. */
    gc_additional_roots_hook = raw_bytes_roots;

    oop pf = platform_object();
    assert(pf != NULL);

    /* --- Property 1: alloc n=8, RawBytesVtable, zero-filled --- */
    oop eight = TAG_SMI(8);
    oop raw = send(pf, sel("basicNewBytes:"), 1, &eight);
    assert(raw != NULL && !IS_SMI(raw));
    assert(RawBytesVtable != NULL);
    assert(obj_vtable(raw) == RawBytesVtable);
    /* RawBytes must remain a DISTINCT builtin from String, even though both
       reuse the same C layout. */
    assert(RawBytesVtable != StringVtable);

    oop sz = send(raw, sel("size"), 0, NULL);
    assert(IS_SMI(sz) && UNTAG_SMI(sz) == 8);
    for (intptr_t i = 1; i <= 8; i++) {
        oop idx = TAG_SMI(i);
        oop b = send(raw, sel("at:"), 1, &idx);
        assert(IS_SMI(b) && UNTAG_SMI(b) == 0);
    }
    printf("test_raw_bytes: property 1 (alloc n=8 zero-filled) PASS\n");

    /* --- Property 2: write/read roundtrip byte 1 and byte 8 --- */
    oop w1args[2] = { TAG_SMI(1), TAG_SMI(171) };
    oop w1 = send(raw, sel("at:put:"), 2, w1args);
    assert(IS_SMI(w1) && UNTAG_SMI(w1) == 171);
    oop w8args[2] = { TAG_SMI(8), TAG_SMI(205) };
    oop w8 = send(raw, sel("at:put:"), 2, w8args);
    assert(IS_SMI(w8) && UNTAG_SMI(w8) == 205);

    oop idx1 = TAG_SMI(1);
    oop r1 = send(raw, sel("at:"), 1, &idx1);
    assert(IS_SMI(r1) && UNTAG_SMI(r1) == 171);
    oop idx8 = TAG_SMI(8);
    oop r8 = send(raw, sel("at:"), 1, &idx8);
    assert(IS_SMI(r8) && UNTAG_SMI(r8) == 205);
    /* untouched middle bytes stay zero */
    oop idx4 = TAG_SMI(4);
    oop r4 = send(raw, sel("at:"), 1, &idx4);
    assert(IS_SMI(r4) && UNTAG_SMI(r4) == 0);
    printf("test_raw_bytes: property 2 (write/read roundtrip) PASS\n");

    /* --- Property 3: out-of-bounds index aborts `index out of bounds` --- */
    g_recv = raw; strcpy(g_selname, "at:"); g_argc = 1;
    g_args[0] = TAG_SMI(0);
    expect_abort("at: index 0 (low OOB)", "index out of bounds");

    g_recv = raw; strcpy(g_selname, "at:"); g_argc = 1;
    g_args[0] = TAG_SMI(9);   /* size is 8 */
    expect_abort("at: index 9 (high OOB)", "index out of bounds");

    g_recv = raw; strcpy(g_selname, "at:put:"); g_argc = 2;
    g_args[0] = TAG_SMI(0); g_args[1] = TAG_SMI(1);
    expect_abort("at:put: index 0 (low OOB)", "index out of bounds");

    g_recv = raw; strcpy(g_selname, "at:put:"); g_argc = 2;
    g_args[0] = TAG_SMI(9); g_args[1] = TAG_SMI(1);
    expect_abort("at:put: index 9 (high OOB)", "index out of bounds");

    /* --- Property 4: out-of-range byte value aborts `byte out of range` --- */
    g_recv = raw; strcpy(g_selname, "at:put:"); g_argc = 2;
    g_args[0] = TAG_SMI(1); g_args[1] = TAG_SMI(256);
    expect_abort("at:put: value 256 (high OOR)", "byte out of range");

    g_recv = raw; strcpy(g_selname, "at:put:"); g_argc = 2;
    g_args[0] = TAG_SMI(1); g_args[1] = TAG_SMI(-1);
    expect_abort("at:put: value -1 (low OOR)", "byte out of range");

    /* --- Property 5/6: freezeBytes: is a distinct, independent copy --- */
    oop frozen = send(pf, sel("freezeBytes:"), 1, &raw);
    assert(frozen != NULL && !IS_SMI(frozen));
    assert(frozen != raw && "freezeBytes: must return a fresh object, not self");
    assert(obj_vtable(frozen) == RawBytesVtable);

    oop fsz = send(frozen, sel("size"), 0, NULL);
    assert(IS_SMI(fsz) && UNTAG_SMI(fsz) == 8);
    for (intptr_t i = 1; i <= 8; i++) {
        oop idx = TAG_SMI(i);
        oop a = send(raw, sel("at:"), 1, &idx);
        oop b = send(frozen, sel("at:"), 1, &idx);
        assert(a == b && "frozen copy must start with bytes equal to the original");
    }
    printf("test_raw_bytes: property 5 (freezeBytes: distinct + equal-bytes copy) PASS\n");

    /* Mutate the original; the frozen copy must NOT observe it (and
       vice-versa is implied: they are now different objects entirely). */
    oop mutargs[2] = { TAG_SMI(1), TAG_SMI(99) };
    send(raw, sel("at:put:"), 2, mutargs);
    oop idx1b = TAG_SMI(1);
    oop orig_after = send(raw, sel("at:"), 1, &idx1b);
    assert(IS_SMI(orig_after) && UNTAG_SMI(orig_after) == 99);
    oop frozen_after = send(frozen, sel("at:"), 1, &idx1b);
    assert(IS_SMI(frozen_after) && UNTAG_SMI(frozen_after) == 171
           && "mutating the original must not change the frozen copy");
    printf("test_raw_bytes: property 6 (freeze-is-copy independence) PASS\n");

    /* --- Property 7 (discipline): String must NOT gain at:put: --- */
    {
        /* Build a length-1 String ('A' = 65) via `platform array:` + `platform
           string:`, the existing byte-Array -> String constructor (std-lib
           3c-3a) — reuses machinery this task must not disturb. */
        oop one = TAG_SMI(1);
        oop arr = send(pf, sel("array:"), 1, &one);
        oop putargs[2] = { TAG_SMI(1), TAG_SMI(65) };
        send(arr, sel("basicAt:put:"), 2, putargs);
        oop str = send(pf, sel("string:"), 1, &arr);
        assert(str != NULL && !IS_SMI(str));
        assert(StringVtable != NULL && obj_vtable(str) == StringVtable);

        g_recv = str; strcpy(g_selname, "at:put:"); g_argc = 2;
        g_args[0] = TAG_SMI(1); g_args[1] = TAG_SMI(66);
        expect_abort("String at:put: still DNUs", "not understood");
    }

    /* --- Property 8 (GC landmine): rooted RawBytes objects survive a
       collection with their bytes intact — proves gc.c traces the new
       vtable as a pointer-free leaf, same as String, not via the generic
       word-scan fallback (which would misinterpret the inline bytes as
       oops). --- */
    g_raw = raw;
    g_frozen = frozen;
    gc_collect();
    assert(gc_is_live(g_raw));
    assert(gc_is_live(g_frozen));
    oop post_sz = send(g_raw, sel("size"), 0, NULL);
    assert(IS_SMI(post_sz) && UNTAG_SMI(post_sz) == 8);
    oop post_b1 = send(g_raw, sel("at:"), 1, &idx1b);
    assert(IS_SMI(post_b1) && UNTAG_SMI(post_b1) == 99);
    oop post_fb1 = send(g_frozen, sel("at:"), 1, &idx1b);
    assert(IS_SMI(post_fb1) && UNTAG_SMI(post_fb1) == 171);
    printf("test_raw_bytes: property 8 (GC leaf-tracing survives collection) PASS\n");

    printf("test_raw_bytes: OK\n");
    return 0;
}
