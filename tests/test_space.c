/* Unit test for the channel-space C substrate (src/space.h, src/space.c —
   Milestone C Task 1). This is the storage substrate the nsl reactor
   (Milestone C Tasks 2-3) will drive via 4 platform prims
   (spacePost:value:/spaceRegister:block:owner:/spaceTake:/spaceDequeue); see
   docs/design/2026-07-06-milestone-c-channels-reactor-design.md
   section 6. Every property below calls space_intern/space_post/space_take/
   space_register/space_dequeue DIRECTLY (never via send()/a primitive
   dispatch) — proving the C-callable seam is real, not an artifact of going
   through the VM (property 6 is the explicit statement of this; every other
   property is incidentally the same proof).

   Build (Model-B link set + space.c; small capacity overrides make the
   boundary-fill/overflow properties fast and deterministic — see
   tools/run_c_tests.sh):
     gcc -std=c99 -Isrc -DOEVM_SPACE_MAX_CHANNELS=8 -DOEVM_SPACE_QUEUE_DEPTH=4 \
         tests/test_space.c \
         src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c \
         src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c \
         src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
         src/backend/posix/udp_posix.c src/kernel_boot.c \
         -lm -o /tmp/ts && /tmp/ts

   Properties (brief Step 1):
     1. intern: same path -> same id; different path -> different id;
        "sensor" vs "sensors" distinct (prefix collision, A1-flavored); fill
        to OEVM_SPACE_MAX_CHANNELS is fine at boundary (no abort). This
        property ALSO pre-interns every channel path the rest of the suite
        needs ("a/b", "a", "gc/chan") as part of its boundary-fill, so later
        properties reuse (not grow) the registry — see the `used` accounting
        below.
     2. post/take FIFO: post v1, v2 on "a/b"; take -> v1, take -> v2,
        take -> NULL.
     3. take exact-match: post on "a/b"; take "a/bc" -> NULL (never
        interned); take "a/b" -> the value.
     4. dequeue: register r1 on "a"; post e1 on "a"; dequeue -> Array{e1,
        [r1]}; dequeue -> NULL.
     5. snapshot-at-dequeue (D-C5): post e2, e3 on "a" (r1 already standing
        from property 4); dequeue (gets e2, snapshot [r1]); register r2;
        dequeue (gets e3) -> snapshot [r1, r2], registration order.
     6. C-callable seam: space_post("a", 1, v) — the ISR-pulse-seam proof
        (spec section 6.1) — then dequeue sees it, snapshot still
        [r1, r2] (both reactions fire, fan-out order preserved).
     7. GC: post a heap String value + register a heap String block/owner
        stand-in on "gc/chan", force a full collection, verify all three
        survive (gc_is_live + content intact) — proves gc.c's call to
        space_gc_trace() actually roots space's own tables, since nothing
        else in this test roots them (mirrors test_raw_bytes.c property 8's
        GC-survival scaffolding).
     8. overflow aborts (fork+SIGABRT, test_raw_bytes's exact pattern,
        generalized to call a C function pointer instead of send()):
        registry overflow (the (N+1)th distinct path once property 1 already
        filled the table to OEVM_SPACE_MAX_CHANNELS) aborts
        `channel registry full`; queue overflow (posting once the ring is
        already at OEVM_SPACE_QUEUE_DEPTH) aborts `channel queue full`;
        an OEVM_SPACE_MAX_PATH-byte path aborts `channel path too long`
        (David-ratified dedicated frozen string, post-Task-1; checked
        before registry occupancy, so it wins even on a full table).
     9. dequeue-under-GC-stress (review fix wave): dequeue a HEAP value with
        a heap reaction registered, under OEVM_GC_STRESS — a full collection
        before EVERY allocation, i.e. inside space_dequeue between (and
        during) its two indexable_new calls; exactly the reviewer's repro
        condition, and the condition the boot-gc-asan gate runs the engine
        under. Asserts value oop identity + bytes and snapshot contents
        intact. Guards space_dequeue's rooting discipline: the queue slot
        must stay live (rooting the value via space_gc_trace) until both
        allocations complete, and the snapshot Array must be gc-temp-rooted
        across the second allocation.
        NOTE: OEVM_GC_STRESS is setenv'd at the TOP of main() — gc.c caches
        the env lookup at the first gc_allocate, so the entire test runs
        under allocation-stress (extra coverage for properties 4-7 too);
        the one place this file itself holds a bare heap oop across an
        allocation (property 7's block/owner Strings) uses the same
        gc_temp_root discipline as the engine. */
#define _POSIX_C_SOURCE 200112L   /* setenv() under -std=c99 */
#include "../src/oevm.h"
#include "../src/space.h"
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
extern void gc_temp_root_push(oop o);
extern void gc_temp_roots_clear(void);

/* Build a heap String with the given bytes (mirrors platform.c's PRIM_pf_string
   allocation idiom: StringVtable + struct String's length-then-inline-bytes
   layout). Used only by property 7 — a real, GC-relevant heap object stand-in
   for "a posted value" / "a registered block" / "a registered owner". */
static oop make_string(const char *s, size_t n) {
    oop str = gc_allocate(StringVtable, sizeof(struct String) + n);
    ((struct String *)str)->length = n;
    if (n) memcpy(((struct String *)str)->bytes, s, n);
    return str;
}

/* ---- fork-and-expect-abort harness ---------------------------------------
   abort() kills the whole process (tests/test_raw_bytes.c's precedent, itself
   citing test_oimg.c). Generalized here to call a bare C function pointer
   instead of send(), since every space_* entry point is a plain C function —
   there is no VM dispatch anywhere in this test (that absence IS property 6). */
static void (*g_trigger)(void);

static void do_call_child(void) {
    g_trigger();
    _exit(42);   /* reached only if the expected abort did NOT happen */
}

static void expect_abort(const char *label, const char *expected_err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); _exit(1); }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); _exit(1); }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        do_call_child();
        /* unreachable: do_call_child always _exit()s */
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
    printf("test_space: %s PASS\n", label);
}

static void trigger_registry_overflow(void) {
    const char *p = "overflow/never-interned-before";
    space_intern(p, strlen(p));
}

static void trigger_queue_overflow(void) {
    space_post("a", 1, TAG_SMI(999));
}

static void trigger_path_too_long(void) {
    /* A path of exactly OEVM_SPACE_MAX_PATH bytes (default 64; the runner's
       -D overrides don't touch MAX_PATH) — one past the longest storable
       length (MAX_PATH-1). Must abort `channel path too long`, NOT
       `channel registry full`, even though the registry is ALSO full when
       this runs (the length check is deliberately first in space_intern). */
    char p[OEVM_SPACE_MAX_PATH];
    memset(p, 'x', sizeof p);
    p[4] = '/'; p[9] = '/';   /* shaped like a real segmented path */
    space_intern(p, sizeof p);
}

int main(void) {
    /* Force a full collection before EVERY gc_allocate for the whole test
       (property 9's condition, and free stress coverage for 4-7). Must be
       set before the first allocation: gc.c caches the getenv result once
       (gc_stress_cached) at the first gc_allocate call. */
    setenv("OEVM_GC_STRESS", "1", 1);

    heap_init();
    kernel_boot();

    /* --- Property 1: intern same/different/prefix-distinct + boundary-fill.
       Also pre-interns every channel the rest of the suite reuses ("a/b",
       "a", "gc/chan"), so later properties never grow the registry past this
       point — the registry sits at exactly OEVM_SPACE_MAX_CHANNELS from here
       to the end of main(), which property 8 relies on for its overflow. --- */
    {
        int id1  = space_intern("p/same", 6);
        int id1b = space_intern("p/same", 6);
        assert(id1 == id1b && "same path must intern to the same id");

        int id2 = space_intern("p/other", 7);
        assert(id2 != id1 && "different path must intern to a different id");

        int id_sensor  = space_intern("sensor", 6);
        int id_sensors = space_intern("sensors", 7);
        assert(id_sensor != id_sensors &&
               "'sensor' and 'sensors' share a prefix but must be distinct channels");

        /* Reserve the channels properties 2-7 need before filling to the
           boundary, so the fill below can legitimately claim every
           remaining slot. */
        (void)space_intern("a/b", 3);
        (void)space_intern("a", 1);
        (void)space_intern("gc/chan", 7);

        int used = 7;   /* p/same, p/other, sensor, sensors, a/b, a, gc/chan */
        assert(OEVM_SPACE_MAX_CHANNELS >= used &&
               "test assumes room for the fixed real channels the suite needs");
        char fillbuf[32];
        for (int i = used; i < OEVM_SPACE_MAX_CHANNELS; i++) {
            int n = snprintf(fillbuf, sizeof fillbuf, "filler/%d", i);
            space_intern(fillbuf, (size_t)n);   /* boundary: must NOT abort */
        }
        printf("test_space: property 1 (intern: same/different/prefix-distinct/boundary-fill) OK\n");
    }

    /* --- Property 2: post/take FIFO order on "a/b" --- */
    {
        oop v1 = TAG_SMI(401), v2 = TAG_SMI(402);
        space_post("a/b", 3, v1);
        space_post("a/b", 3, v2);
        oop t1 = space_take("a/b", 3);
        assert(t1 == v1 && "take must return the OLDEST queued value first");
        oop t2 = space_take("a/b", 3);
        assert(t2 == v2 && "take must return the second value next");
        oop t3 = space_take("a/b", 3);
        assert(t3 == NULL && "take on a drained channel must answer nil");
        printf("test_space: property 2 (post/take FIFO order) OK\n");
    }

    /* --- Property 3: take exact-match ("a/b" != "a/bc") --- */
    {
        oop v3 = TAG_SMI(403);
        space_post("a/b", 3, v3);
        oop miss = space_take("a/bc", 4);
        assert(miss == NULL && "\"a/bc\" must never match \"a/b\"'s queued event");
        oop hit = space_take("a/b", 3);
        assert(hit == v3 && "the exact-match channel must still have its value");
        printf("test_space: property 3 (take exact-match: \"a/b\" != \"a/bc\") OK\n");
    }

    /* --- Property 4: dequeue answers {value, reaction-block snapshot} --- */
    oop r1_block = TAG_SMI(101);
    oop r1_owner = TAG_SMI(201);
    {
        space_register("a", 1, r1_block, r1_owner);
        oop e1 = TAG_SMI(301);
        space_post("a", 1, e1);

        oop dq1 = space_dequeue();
        assert(dq1 != NULL && !IS_SMI(dq1));
        struct ObjectInstance *dq1i = (struct ObjectInstance *)dq1;
        assert(UNTAG_SMI(instance_slot(dq1i, 0)) == 3 && "outer Array must be 3 slots (value, snapshot, channelPath)");
        assert(instance_slot(dq1i, 1) == e1 && "slot 1 is the posted value");
        oop snap1 = instance_slot(dq1i, 2);
        struct ObjectInstance *snap1i = (struct ObjectInstance *)snap1;
        assert(UNTAG_SMI(instance_slot(snap1i, 0)) == 1 && "one standing reaction");
        assert(instance_slot(snap1i, 1) == r1_block);
        oop chpath1 = instance_slot(dq1i, 3);   /* Milestone D: slot 3 is the channel path String */
        assert(chpath1 != NULL && !IS_SMI(chpath1) && "slot 3 is the channel path String");
        assert(((struct String *)chpath1)->length == 1 && ((struct String *)chpath1)->bytes[0] == 'a'
               && "channel path String is 'a'");

        oop dq_empty = space_dequeue();
        assert(dq_empty == NULL && "dequeue on an empty queue must answer nil");
        printf("test_space: property 4 (dequeue: value + reaction-block snapshot Array) OK\n");
    }

    /* --- Property 5: snapshot-at-dequeue (D-C5) --- */
    oop r2_block = TAG_SMI(102);
    {
        oop r2_owner = TAG_SMI(202);
        oop e2 = TAG_SMI(302), e3 = TAG_SMI(303);
        space_post("a", 1, e2);
        space_post("a", 1, e3);

        /* e2 dequeues BEFORE r2 is registered: its snapshot must be [r1] only. */
        oop dqA = space_dequeue();
        struct ObjectInstance *dqAi = (struct ObjectInstance *)dqA;
        assert(instance_slot(dqAi, 1) == e2);
        oop snapA = instance_slot(dqAi, 2);
        struct ObjectInstance *snapAi = (struct ObjectInstance *)snapA;
        assert(UNTAG_SMI(instance_slot(snapAi, 0)) == 1);
        assert(instance_slot(snapAi, 1) == r1_block);

        space_register("a", 1, r2_block, r2_owner);

        /* e3 dequeues AFTER r2 is registered: its snapshot must be
           [r1, r2], registration order (D-C3/D-C5). */
        oop dqB = space_dequeue();
        struct ObjectInstance *dqBi = (struct ObjectInstance *)dqB;
        assert(instance_slot(dqBi, 1) == e3);
        oop snapB = instance_slot(dqBi, 2);
        struct ObjectInstance *snapBi = (struct ObjectInstance *)snapB;
        assert(UNTAG_SMI(instance_slot(snapBi, 0)) == 2);
        assert(instance_slot(snapBi, 1) == r1_block);
        assert(instance_slot(snapBi, 2) == r2_block);
        printf("test_space: property 5 (snapshot-at-dequeue, D-C5) OK\n");
    }

    /* --- Property 6: the C-callable post seam (spec section 6.1) — a bare
       space_post() call, exactly as an ISR handler would make it, with no
       send()/VM dispatch anywhere in this file (already true of every
       property above; this is the explicit statement of it). --- */
    {
        oop e4 = TAG_SMI(304);
        space_post("a", 1, e4);   /* THE ISR-pulse-seam call */
        oop dqC = space_dequeue();
        assert(dqC != NULL);
        struct ObjectInstance *dqCi = (struct ObjectInstance *)dqC;
        assert(instance_slot(dqCi, 1) == e4);
        oop snapC = instance_slot(dqCi, 2);
        struct ObjectInstance *snapCi = (struct ObjectInstance *)snapC;
        assert(UNTAG_SMI(instance_slot(snapCi, 0)) == 2);
        assert(instance_slot(snapCi, 1) == r1_block);
        assert(instance_slot(snapCi, 2) == r2_block);
        printf("test_space: property 6 (C-callable post seam is the ISR-pulse proof, spec 6.1) OK\n");
    }

    /* --- Property 7: GC — a queued value + registered block/owner rooted
       ONLY through space's own tables (nothing else in this test roots
       them) must survive a full collection intact. Proves gc.c's call to
       space_gc_trace() (added beside the RawBytesVtable/StringVtable root
       marks), not merely that space.c compiles.
       Under OEVM_GC_STRESS this block must itself be GC-clean: each String
       is either handed to the space (rooting it) before the next allocation
       or pinned as a gc temp root across it — the same discipline the
       engine uses (src/udp.c basicReceive:). heap_block stays visible at
       main scope: property 9 reuses it (it remains gc/chan's standing
       reaction). --- */
    const char *blk_s = "block-stand-in";
    oop heap_block = NULL;
    {
        const char *val_s = "sensor-reading-42";
        const char *own_s = "owner-stand-in";
        oop heap_val = make_string(val_s, strlen(val_s));
        space_post("gc/chan", 7, heap_val);       /* rooted via the queue slot */
        heap_block = make_string(blk_s, strlen(blk_s));
        gc_temp_root_push(heap_block);            /* bare local across the next alloc */
        oop heap_owner = make_string(own_s, strlen(own_s));
        space_register("gc/chan", 7, heap_block, heap_owner);  /* both now space-rooted */
        gc_temp_roots_clear();

        gc_collect();

        assert(gc_is_live(heap_val)   && "queued value must survive a collection");
        assert(gc_is_live(heap_block) && "registered block must survive a collection");
        assert(gc_is_live(heap_owner) && "registered owner must survive a collection");
        assert(((struct String *)heap_val)->length == strlen(val_s));
        assert(memcmp(((struct String *)heap_val)->bytes, val_s, strlen(val_s)) == 0
               && "value bytes must be intact post-GC");

        oop taken = space_take("gc/chan", 7);
        assert(taken == heap_val && "take after GC must still return the same (live) oop");
        printf("test_space: property 7 (GC: queued value + registered block/owner survive a collection) OK\n");
    }

    /* --- Property 8: overflow aborts (registry full, queue full, path too
       long) --- */
    {
        /* Registry: property 1 already filled the table to exactly
           OEVM_SPACE_MAX_CHANNELS; one more distinct, never-before-seen path
           must abort. */
        g_trigger = trigger_registry_overflow;
        expect_abort("registry overflow -> \"channel registry full\"", "channel registry full");

        /* Queue: fill the ring to its configured depth (boundary — must NOT
           abort, mirrors property 1's boundary-fill discipline), then one
           more post must abort. */
        for (int i = 0; i < OEVM_SPACE_QUEUE_DEPTH; i++) {
            space_post("a", 1, TAG_SMI(500 + i));
        }
        g_trigger = trigger_queue_overflow;
        expect_abort("queue overflow -> \"channel queue full\"", "channel queue full");

        /* Path too long (David-ratified dedicated frozen string, spec
           section 7 updated post-Task-1): an OEVM_SPACE_MAX_PATH-byte path
           must abort `channel path too long` — and specifically NOT
           `channel registry full`, despite the registry also being full
           here (length is checked first). */
        g_trigger = trigger_path_too_long;
        expect_abort("path too long -> \"channel path too long\"", "channel path too long");

        printf("test_space: property 8 (overflow aborts: registry full, queue full, path too long) OK\n");
    }

    /* --- Property 9 (review fix wave): dequeue a HEAP value under
       OEVM_GC_STRESS (active since main()'s first line). Inside
       space_dequeue, BOTH indexable_new calls collect before allocating; a
       regression to eager slot-clearing (value unrooted across the first
       alloc) or an unpinned snapshot (unrooted across the second alloc)
       sweeps a live object and this property catches it as identity/bytes
       corruption — the reviewer's exact repro. --- */
    {
        /* Drain the SMI events property 8 left filling the ring (posted on
           "a"); space_take never allocates, so this is stress-safe. */
        while (space_take("a", 1) != NULL) {}

        const char *v9_s = "heap-value-dequeued-under-stress";
        oop v9 = make_string(v9_s, strlen(v9_s));
        space_post("gc/chan", 7, v9);   /* rooted via the queue slot; no alloc between */

        oop dq = space_dequeue();       /* collects (at least) twice inside */
        assert(dq != NULL && !IS_SMI(dq));
        struct ObjectInstance *dqi = (struct ObjectInstance *)dq;
        assert(UNTAG_SMI(instance_slot(dqi, 0)) == 3);   /* Milestone D: {value, snapshot, channelPath} */
        oop chpathG = instance_slot(dqi, 3);
        assert(chpathG != NULL && !IS_SMI(chpathG) && "slot 3 is the channel path String");

        oop got = instance_slot(dqi, 1);
        assert(got == v9 && "dequeued value oop must be the posted one (not swept/reused)");
        assert(gc_is_live(got) && "dequeued value must be live post-dequeue");
        assert(((struct String *)got)->length == strlen(v9_s));
        assert(memcmp(((struct String *)got)->bytes, v9_s, strlen(v9_s)) == 0
               && "value bytes must be intact after in-dequeue collections");

        oop snap = instance_slot(dqi, 2);
        assert(snap != NULL && !IS_SMI(snap));
        assert(gc_is_live(snap) && "snapshot Array must survive the second in-dequeue collection");
        struct ObjectInstance *snapi = (struct ObjectInstance *)snap;
        assert(UNTAG_SMI(instance_slot(snapi, 0)) == 1 && "gc/chan has exactly one reaction");
        assert(instance_slot(snapi, 1) == heap_block
               && "snapshot must hold gc/chan's registered heap block");
        assert(((struct String *)heap_block)->length == strlen(blk_s));
        assert(memcmp(((struct String *)heap_block)->bytes, blk_s, strlen(blk_s)) == 0
               && "reaction block bytes must be intact");

        printf("test_space: property 9 (dequeue heap value under OEVM_GC_STRESS, rooting discipline) OK\n");
    }

    printf("test_space: OK\n");
    return 0;
}
