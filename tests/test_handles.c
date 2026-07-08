/* Unit test for the M6 C-boundary handle table (src/handles.c). Proves: a handle
   keeps its object alive across a forced GC; get resolves; free releases; and an
   over-full table aborts with the FROZEN string "handle table full".
   Built with a tiny capacity via -DOEVM_HANDLE_TABLE_MAX=4 by run_c_tests.sh. */
#define _POSIX_C_SOURCE 200112L
#include "../src/oevm.h"
#include "../src/handles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

extern void heap_init(void);
extern void kernel_boot(void);
extern oop  StringVtable;
extern oop  gc_allocate(oop vtable, size_t size);
extern void gc_collect(void);
extern int  gc_is_live(oop ptr);

/* fork-and-expect-abort harness (mirrors tests/test_space.c) */
static void (*g_trigger)(void);
static void do_call_child(void) { g_trigger(); _exit(42); }
static void expect_abort(const char *label, const char *expected_err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); _exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); _exit(1); }
    if (pid == 0) { close(pipefd[0]); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]); do_call_child(); }
    close(pipefd[1]);
    char buf[512]; ssize_t total = 0, r;
    while (total < (ssize_t)sizeof(buf) - 1 &&
           (r = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0) total += r;
    buf[total] = '\0'; close(pipefd[0]);
    int status = 0; waitpid(pid, &status, 0);
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)) {
        fprintf(stderr, "FAIL %s: expected SIGABRT, status=%d stderr=[%s]\n", label, status, buf); exit(1); }
    if (expected_err && strstr(buf, expected_err) == NULL) {
        fprintf(stderr, "FAIL %s: expected stderr to contain '%s', got=[%s]\n", label, expected_err, buf); exit(1); }
    printf("test_handles: %s PASS\n", label);
}

static oop make_str(void) {  /* a heap object to pin */
    return gc_allocate(StringVtable, 8);
}

static void trigger_overflow(void) {
    /* OEVM_HANDLE_TABLE_MAX == 4 in this build: 5th live handle overflows. */
    for (int i = 0; i < 5; i++) handle_new(make_str());
}

int main(void) {
    setenv("OEVM_GC_STRESS", "1", 1);
    heap_init();
    kernel_boot();

    /* survives-collection: a pinned object is live after GC; an unpinned one is collectable */
    oop pinned = make_str();
    int h = handle_new(pinned);
    gc_collect();
    assert(gc_is_live(pinned) && "handle-pinned object must survive a collection");
    assert(handle_get(h) == pinned && "handle_get must resolve to the pinned oop");

    /* free releases the pin (get returns NULL for a freed slot) */
    handle_free(h);
    assert(handle_get(h) == NULL && "freed handle must resolve to NULL");

    /* a freed slot is reusable */
    oop again = make_str();
    int h2 = handle_new(again);
    assert(handle_get(h2) == again && "reused slot resolves");
    handle_free(h2);

    /* overflow aborts with the frozen string */
    g_trigger = trigger_overflow;
    expect_abort("overflow", "handle table full");

    printf("test_handles: OK\n");
    return 0;
}
