/* Unit test for the posix pulse backend (src/backend/posix/pulse_posix.c) + the M6
   handle table + space seam, exercising the SAME path the target ISR uses:
     handle_new(value) -> pulse_post_isr(chan_id, idx) -> pulse_pump() -> space_post_id.
   Proves FIFO delivery, that the value reaches the space queue, and that an over-full
   mailbox aborts "pulse mailbox full". Built with -DOEVM_PULSE_MAILBOX_DEPTH=4. */
#define _POSIX_C_SOURCE 200112L
#include "../src/oevm.h"
#include "../src/space.h"
#include "../src/handles.h"
#include "../src/pulse_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

extern void heap_init(void);
extern void kernel_boot(void);

/* space.c internals we assert against */
extern oop space_take(const char *path, size_t len);  /* removes+returns oldest live value, or NULL */

static void (*g_trigger)(void);
static void do_call_child(void) { g_trigger(); _exit(42); }
static void expect_abort(const char *label, const char *expected_err) {
    int pipefd[2]; if (pipe(pipefd)!=0){perror("pipe");_exit(1);}
    pid_t pid = fork(); if (pid<0){perror("fork");_exit(1);}
    if (pid==0){close(pipefd[0]);dup2(pipefd[1],STDERR_FILENO);close(pipefd[1]);do_call_child();}
    close(pipefd[1]); char buf[512]; ssize_t total=0,r;
    while (total<(ssize_t)sizeof(buf)-1 && (r=read(pipefd[0],buf+total,sizeof(buf)-1-total))>0) total+=r;
    buf[total]='\0'; close(pipefd[0]); int status=0; waitpid(pid,&status,0);
    if (!(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT)){fprintf(stderr,"FAIL %s: no SIGABRT status=%d [%s]\n",label,status,buf);exit(1);}
    if (expected_err && strstr(buf,expected_err)==NULL){fprintf(stderr,"FAIL %s: want '%s' got [%s]\n",label,expected_err,buf);exit(1);}
    printf("test_pulse: %s PASS\n", label);
}

static void trigger_mailbox_overflow(void) {
    int c = space_intern("sys/gpio", 8);
    int h = handle_new(TAG_SMI(7));
    for (int i = 0; i < 5; i++) pulse_post_isr(c, h);   /* depth 4 -> 5th overflows */
}

int main(void) {
    heap_init();
    kernel_boot();

    int c = space_intern("sys/gpio", 8);
    int h1 = handle_new(TAG_SMI(11));
    int h2 = handle_new(TAG_SMI(22));

    /* nothing pumped yet: pump on an empty mailbox drains nothing */
    assert(pulse_pump() == 0 && "empty mailbox pumps 0");

    /* post two, pump: both reach the space queue in FIFO order */
    pulse_post_isr(c, h1);
    pulse_post_isr(c, h2);
    assert(pulse_pump() == 2 && "two posted -> two pumped");
    assert(space_take("sys/gpio", 8) == TAG_SMI(11) && "FIFO: first value first");
    assert(space_take("sys/gpio", 8) == TAG_SMI(22) && "FIFO: second value second");
    assert(space_take("sys/gpio", 8) == NULL && "queue drained");

    /* overflow aborts with the frozen string */
    g_trigger = trigger_mailbox_overflow;
    expect_abort("mailbox_overflow", "pulse mailbox full");

    printf("test_pulse: OK\n");
    return 0;
}
