# Zephyr Pulse-Primitive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the tuvm VM on Zephyr and land the ISR→channel *pulse* primitive end-to-end — a GPIO-edge interrupt posts an event onto an nsl channel, the reactor observes it — proven on `native_sim` (CI gate) and a physical nRF52840 (acceptance bar).

**Architecture:** A GPIO-edge ISR does only an ISR-safe `k_msgq_put` of a `(chan_id, handle_idx)` token; the main loop drains that mailbox at the run-to-completion safe point via a `platform pumpEvents` mechanism prim, resolves the M6 handle table to an oop, and calls the existing `space_post`. The reactor loop stays in nsl (`[ platform pumpEvents. space drain ] repeat`). C never holds a raw oop — it holds an index into a GC-rooted handle table (G3 §M6). The host build exercises the identical handle-table + mailbox path through a simulated-edge prim, so the host AB suite is a true differential test of the real seam.

**Tech Stack:** C99 (tuvm engine), Newspeak/Smalltalk "nsl" (kernel library + tests), Zephyr RTOS via Nordic NCS (`~/ncs` west workspace, SDK `2ac5840438`), boards `native_sim` and `nrf52840dongle/nrf52840`.

**Spec:** `nsl-tuvm/docs/design/2026-07-08-zephyr-pulse-primitive-design.md`

## Global Constraints

Every task's requirements implicitly include these. Values copied verbatim from the spec and codebase conventions:

- **Prim numbering is append-only.** New PRIM ids go at the END of the `dispatch.c` enum AND `PRIM_NAMES[]` (parallel, index `i` → id `i+1`); the id is obtained by **COUNTING** `PRIM_NAMES` entries, never by `previous_#define + 1` (6 predicate slots have no `prim_ids.h` mirror, so arithmetic is wrong). `builtins.def` is NOT touched.
- **C holds M6 handle indices, never raw oops.** The mailbox and ISR slots carry `(chan_id, handle_idx)`.
- **ISRs never touch the VM heap or the Space ring** — only `k_msgq_put`. `space.c` stays irq-free.
- **Fixed-capacity tables, `-D`-overridable, embedded-arena style.** Exhaustion = Tier-2 crash: bare `fprintf(stderr, "<frozen string>\n"); abort();` (no helper). Frozen strings introduced here: `"handle table full"`, `"pulse mailbox full"`.
- **Both execution paths must pass.** The gate is `cd ~/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py` — every `.ns` test is run Path A (solidified) AND Path B (.oimg) and must match its `expect:` block on both.
- **AB runs CLOBBER `tuvm/src/solidified_image.c` + `.stmap`.** Always `cd ~/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap` before any tuvm commit.
- **The full nsl suite (~10 min) is controller-owned** — do not spin it inside a subagent; the controller runs it at task boundaries.
- **Non-moving GC is verified** (`gc.c` mark-and-sweep + coalescing, no relocation) — the handle table roots values; supporting property, not a new assumption.

## File map (what each new/edited file is responsible for)

**Repo `~/workspace/tuvm` (C engine):**
- Create `src/handles.h`, `src/handles.c` — M6 C-boundary handle table (`handle_new/get/free`, `handle_gc_trace`).
- Create `tests/test_handles.c` — handle-table GC-survival + overflow-abort unit test.
- Create `src/pulse_backend.h` — the pulse seam (`pulse_post_isr`, `pulse_pump`).
- Create `src/backend/posix/pulse_posix.c` — host in-process ring impl.
- Create `src/backend/zephyr/pulse_zephyr.c` — `k_msgq` impl (ISR-safe).
- Create `tests/test_pulse.c` — posix pulse round-trip + mailbox-overflow unit test.
- Modify `src/dispatch.c` — append `PRIM_pf_gpioPin`, `PRIM_pf_gpioFire`, `PRIM_pf_pumpEvents` to the enum and `PRIM_NAMES[]`.
- Modify `src/prim_ids.h` — `#define` the three new ids (counted).
- Modify `src/platform.c` — host GPIO slot table + the three `platform_dispatch` cases; include `pulse_backend.h` + `handles.h`.
- Modify `src/gc.c` — one `handle_gc_trace();` call at the root-scan phase (line ~715, beside `space_gc_trace();`).
- Modify `tools/run_c_tests.sh` — add `src/handles.c` + `src/pulse_backend`-posix to `ENG_COMMON`; register `test_handles` and `test_pulse`; add `OEVM_BUILD=asan|ubsan` knob.
- Modify `tools/footprint.sh` — `FOOTPRINT_CONFIG=mcu` cross-compiled ROM/RAM line.
- Create `zephyr/` app (CMakeLists.txt, prj.conf, boards overlay, src/main.c) — the Zephyr bring-up harness (adapted from `ns-zephyr/app`).

**Repo `~/workspace/nsl-tuvm` (kernel library):**
- Modify `lib/kernel.ns` — add nested `class GpioPulse`, three seam forwarders, and a `gpio` accessor.

**Repo `~/workspace/nsl-tests` (test suites):**
- Create `13_pulse/01_pulse_roundtrip.ns` — the host AB gate for the pulse surface.

---

### Task 1: M6 C-boundary handle table

**Files:**
- Create: `~/workspace/tuvm/src/handles.h`
- Create: `~/workspace/tuvm/src/handles.c`
- Create: `~/workspace/tuvm/tests/test_handles.c`
- Modify: `~/workspace/tuvm/src/gc.c` (root-scan phase, ~line 715)
- Modify: `~/workspace/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: `oop`, `mark_object(oop)` (extern from gc.c:630), `IS_SMI` (oevm.h).
- Produces:
  - `int  handle_new(oop o);`  — pin `o`; return a stable index `>= 0`; abort `"handle table full"` when full.
  - `oop  handle_get(int idx);` — return the oop for a live index (NULL if the slot is free / idx out of range).
  - `void handle_free(int idx);` — release the pin (idempotent; ignores out-of-range/free).
  - `void handle_gc_trace(void);` — `mark_object` every live slot.

- [ ] **Step 1: Write the failing test** — `~/workspace/tuvm/tests/test_handles.c`

```c
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
```

- [ ] **Step 2: Create the header** — `~/workspace/tuvm/src/handles.h`

```c
#ifndef OEVM_HANDLES_H
#define OEVM_HANDLES_H
/* M6 C-boundary handle table (G3 decision doc §M6): when C code (the ISR→channel
   pulse seam, timer callbacks, drivers) must hold a reference to a VM object across
   a boundary the GC cannot see (an ISR slot, a k_msgq item), it holds a small stable
   INDEX into this table, never a raw oop. Live slots are marked as GC roots every
   collection (handle_gc_trace, called from gc.c beside space_gc_trace), so the
   referenced object survives; and C never sees a heap address (strengthens §M4).
   Fixed capacity, -D-overridable (embedded-arena style); exhaustion is a Tier-2
   crash with the FROZEN string "handle table full". */
#include "oevm.h"

#ifndef OEVM_HANDLE_TABLE_MAX
#define OEVM_HANDLE_TABLE_MAX 32
#endif

/* Pin `o` and return a stable index in [0, OEVM_HANDLE_TABLE_MAX). Aborts
   "handle table full" when every slot is live. Reuses a freed slot before growing. */
int  handle_new(oop o);
/* Resolve a live index to its oop; NULL for an out-of-range or freed index. */
oop  handle_get(int idx);
/* Release the pin at `idx`; idempotent, ignores out-of-range/already-free. */
void handle_free(int idx);
/* Mark every live slot as a GC root (called from gc_collect's root scan). */
void handle_gc_trace(void);
#endif /* OEVM_HANDLES_H */
```

- [ ] **Step 3: Create the implementation** — `~/workspace/tuvm/src/handles.c`

```c
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
```

- [ ] **Step 4: Hook the trace into GC root-scan** — `~/workspace/tuvm/src/gc.c`

Immediately after the existing `space_gc_trace();` call (~line 715), add:

```c
    /* M6 C-boundary handle table (src/handles.c): live entries are GC roots held
       only in that static table, outside every other root set — trace them. */
    extern void handle_gc_trace(void);
    handle_gc_trace();
```

- [ ] **Step 5: Register the module + test** — `~/workspace/tuvm/tools/run_c_tests.sh`

Add `src/handles.c` to the `ENG_COMMON` link string (so both ENG_A and ENG_B link it):

```bash
ENG_COMMON="src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c src/handles.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c"
```

Add a registration line beside the `test_space` line:

```bash
run test_handles "$ENG_B" -DOEVM_HANDLE_TABLE_MAX=4
```

- [ ] **Step 6: Run the test — verify it passes**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | grep -E 'test_handles|FAIL'`
Expected: `test_handles                 PASS` and no `FAIL` lines.

- [ ] **Step 7: Commit** (restore the clobber-prone image files first, though this task doesn't touch them — habit)

```bash
cd ~/workspace/tuvm
git add src/handles.h src/handles.c tests/test_handles.c src/gc.c tools/run_c_tests.sh
git commit -m "feat(gc): M6 C-boundary handle table (JNI-global-ref roots for the Zephyr seam)"
```

---

### Task 2: Pulse backend seam + posix impl

**Files:**
- Create: `~/workspace/tuvm/src/pulse_backend.h`
- Create: `~/workspace/tuvm/src/backend/posix/pulse_posix.c`
- Create: `~/workspace/tuvm/tests/test_pulse.c`
- Modify: `~/workspace/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: `space_post(const char*, size_t, oop)` (space.h), `space_intern(const char*, size_t)` (space.h), `handle_get(int)` (handles.h).
- Produces:
  - `void pulse_post_isr(int chan_id, int handle_idx);` — ISR-safe, non-allocating: enqueue `(chan_id, handle_idx)` at the mailbox tail; abort `"pulse mailbox full"` when full.
  - `int  pulse_pump(void);` — thread-context: drain every queued item — `space_post_id(chan_id, handle_get(handle_idx))` — and return the count drained. Posix impl is non-blocking (returns 0 immediately when empty); the zephyr impl (Task 6) blocks first.
  - `void space_post_id(int chan_id, oop value);` — NEW thin helper in space.c/space.h: like `space_post` but takes an already-interned channel id (the mailbox stored the id, not the path). See Step 2.

- [ ] **Step 1: Add `space_post_id` to space.c/space.h** (the mailbox carries a chan id, not a path)

In `~/workspace/tuvm/src/space.h`, after the `space_post` declaration (~line 59):

```c
/* Like space_post but the channel is already interned to an id (the ISR-pulse
   mailbox stores ids, not path strings). Aborts "channel queue full" when full. */
void space_post_id(int chan_id, oop value);
```

In `~/workspace/tuvm/src/space.c`, refactor `space_post` to delegate (keep behavior identical). Replace the body of `space_post` and add `space_post_id`:

```c
void space_post_id(int chan_id, oop value) {
    if (q_count >= OEVM_SPACE_QUEUE_DEPTH) {
        fprintf(stderr, "channel queue full\n");
        abort();
    }
    size_t tail = (q_head + q_count) % OEVM_SPACE_QUEUE_DEPTH;
    queue[tail].chan = chan_id;
    queue[tail].value = value;
    queue[tail].live = 1;
    q_count++;
}
void space_post(const char *path, size_t len, oop value) {
    space_post_id(space_intern(path, len), value);
}
```

- [ ] **Step 2: Write the failing test** — `~/workspace/tuvm/tests/test_pulse.c`

```c
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
```

(If `space_take` is not already declared in `space.h`, add `oop space_take(const char *path, size_t len);` there — the agent survey confirms `space_take` exists in space.c; check `space.h` and add the prototype if missing, in the same commit.)

- [ ] **Step 3: Create the seam header** — `~/workspace/tuvm/src/pulse_backend.h`

```c
#ifndef OEVM_PULSE_BACKEND_H
#define OEVM_PULSE_BACKEND_H
/* The ISR→channel pulse seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/pulse_posix.c (in-process ring, non-blocking pump) for the host
   gate; backend/zephyr/pulse_zephyr.c (k_msgq, ISR-safe put, blocking pump) on
   target. An interrupt calls pulse_post_isr ONLY; the run-to-completion main loop
   calls pulse_pump at the safe point. C never holds a raw oop — the mailbox carries
   an M6 handle index, resolved to an oop (handle_get) inside pulse_pump, then handed
   to space_post_id in thread context. See the design spec §2. */
#include <stddef.h>

#ifndef OEVM_PULSE_MAILBOX_DEPTH
#define OEVM_PULSE_MAILBOX_DEPTH 16
#endif

/* ISR-safe: enqueue (chan_id, handle_idx). Non-allocating, non-blocking.
   Aborts "pulse mailbox full" when the mailbox is at capacity. */
void pulse_post_isr(int chan_id, int handle_idx);

/* Thread context, at the run-to-completion safe point. Drains every queued item
   (posix: returns 0 at once when empty; zephyr: blocks for the first item, then
   drains all) and for each calls space_post_id(chan_id, handle_get(handle_idx)).
   Returns the number of items delivered. */
int pulse_pump(void);
#endif /* OEVM_PULSE_BACKEND_H */
```

- [ ] **Step 4: Create the posix impl** — `~/workspace/tuvm/src/backend/posix/pulse_posix.c`

```c
/* Host pulse backend: a plain in-process ring (no irq, no threads). The host GPIO
   mock's simulated-edge prim (platform.c PRIM_pf_gpioFire) calls pulse_post_isr in
   thread context; pulse_pump is non-blocking so the host AB test rig can drive
   [gpioFire; pumpEvents; drain] without hanging. Same code path as the target
   (handle_get -> space_post_id); only the queue mechanism differs. */
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include <stdio.h>
#include <stdlib.h>

struct PulseItem { int chan_id; int handle_idx; };
static struct PulseItem mailbox[OEVM_PULSE_MAILBOX_DEPTH];
static int mb_head = 0, mb_count = 0;

void pulse_post_isr(int chan_id, int handle_idx) {
    if (mb_count >= OEVM_PULSE_MAILBOX_DEPTH) {
        fprintf(stderr, "pulse mailbox full\n");
        abort();
    }
    int tail = (mb_head + mb_count) % OEVM_PULSE_MAILBOX_DEPTH;
    mailbox[tail].chan_id = chan_id;
    mailbox[tail].handle_idx = handle_idx;
    mb_count++;
}

int pulse_pump(void) {
    int drained = 0;
    while (mb_count > 0) {
        struct PulseItem it = mailbox[mb_head];
        mb_head = (mb_head + 1) % OEVM_PULSE_MAILBOX_DEPTH;
        mb_count--;
        space_post_id(it.chan_id, handle_get(it.handle_idx));
        drained++;
    }
    return drained;
}
```

- [ ] **Step 5: Register module + test** — `~/workspace/tuvm/tools/run_c_tests.sh`

Add the posix pulse backend to `ENG_COMMON`:

```bash
ENG_COMMON="src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c src/handles.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
  src/backend/posix/pulse_posix.c"
```

Register the test beside `test_handles`:

```bash
run test_pulse "$ENG_B" -DOEVM_PULSE_MAILBOX_DEPTH=4
```

- [ ] **Step 6: Run the test — verify it passes**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | grep -E 'test_pulse|FAIL'`
Expected: `test_pulse                   PASS` and no `FAIL` lines.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git add src/pulse_backend.h src/backend/posix/pulse_posix.c tests/test_pulse.c src/space.c src/space.h tools/run_c_tests.sh
git commit -m "feat(seam): pulse backend seam + posix in-process ring (ISR-safe post, safe-point pump)"
```

---

### Task 3: Host GPIO prims + `pumpEvents`

**Files:**
- Modify: `~/workspace/tuvm/src/dispatch.c` (enum tail + `PRIM_NAMES[]` tail)
- Modify: `~/workspace/tuvm/src/prim_ids.h` (three `#define`s, counted)
- Modify: `~/workspace/tuvm/src/platform.c` (include headers, host GPIO slot table, three `platform_dispatch` cases)
- Modify: `~/workspace/tuvm/tests/test_pulse.c` (drive the prims via `platform_dispatch`)

**Interfaces:**
- Consumes: `handle_new` (handles.h), `space_intern`, `pulse_post_isr`, `pulse_pump` (Tasks 1–2), `TAG_SMI`/`UNTAG_SMI`/`IS_SMI` (oevm.h), `platform_object()` (platform.c), `StringVtable`, `obj_vtable`, `struct String` (as used by `PRIM_pf_spacePost`).
- Produces three `platform`-receiver selectors, matched by name in `PRIM_NAMES[]`:
  - `platform gpioPin: n edge: e post: msg on: chanPath` — register: `handle_new(msg)`, `space_intern(chanPath)`, store `(pin=n, edge=e, chan_id, handle_idx)` in a host GPIO slot. Answers `platform`.
  - `platform gpioFire: n` — host-only simulated edge: find the slot for pin `n`, `pulse_post_isr(chan_id, handle_idx)`. Answers `platform`. (On target this is a no-op stub; the real ISR posts.)
  - `platform pumpEvents` — `*out = TAG_SMI(pulse_pump())`.

- [ ] **Step 1: Append the three prim ids to the enum + names array** — `~/workspace/tuvm/src/dispatch.c`

At the END of the `enum` (after `PRIM_ensure,` ~line 674):

```c
    /* Zephyr rung-2 sub-project 1: the ISR→channel pulse seam. Appended last —
       no pre-existing id shifts. Ids are the 1-based position in PRIM_NAMES. */
    PRIM_pf_gpioPin,
    PRIM_pf_gpioFire,
    PRIM_pf_pumpEvents,
```

At the END of `PRIM_NAMES[]` (after `"ensure:",` ~line 709), in the SAME order:

```c
    "gpioPin:edge:post:on:",
    "gpioFire:",
    "pumpEvents",
```

- [ ] **Step 2: Count the ids and add the mirrors** — `~/workspace/tuvm/src/prim_ids.h`

Count `PRIM_NAMES[]`: the last pre-existing entry `"ensure:"` is id 93 (`PRIM_ensure`). The three appends are therefore positions 94, 95, 96 — **verify by counting the array, do not assume**. After the `PRIM_ensure` region (or wherever the last mirrors sit, at the end before `#endif`):

```c
/* Zephyr rung-2 sub-project 1: pulse seam prims, ids counted from PRIM_NAMES. */
#define PRIM_pf_gpioPin      94
#define PRIM_pf_gpioFire     95
#define PRIM_pf_pumpEvents   96
```

To confirm the count mechanically:
Run: `cd ~/workspace/tuvm && awk '/static const char \*const PRIM_NAMES/{f=1} f&&/"pumpEvents"/{print NR": pumpEvents"} f&&/};/{f=0}' src/dispatch.c`
Then count entries: `grep -c '"' <(sed -n '/PRIM_NAMES\[\] = {/,/};/p' src/dispatch.c)` is a rough aid — but the authoritative check is that `selector_prim_id` returns the matching `#define` (validated by the Step-6 nsl test failing loudly if the id is wrong).

- [ ] **Step 3: Add the host GPIO facility + three cases** — `~/workspace/tuvm/src/platform.c`

Add includes near the top (beside the existing `space.h` include):

```c
#include "pulse_backend.h"
#include "handles.h"
```

Add the host GPIO slot table beside the HostTimer table (~line 30):

```c
/* Host GPIO mock: registration stores (pin, chan_id, handle_idx); the host-only
   gpioFire: prim simulates an edge by posting to the pulse mailbox — the exact
   (chan_id, handle_idx) an on-target GPIO ISR would post. Fixed table, -D override. */
#ifndef HOST_MAX_GPIO
#define HOST_MAX_GPIO 16
#endif
struct HostGpio { int pin; int chan_id; int handle_idx; int live; };
static struct HostGpio host_gpio[HOST_MAX_GPIO];
static int host_n_gpio = 0;

static void host_gpio_register(int pin, int chan_id, int handle_idx) {
    int i;
    for (i = 0; i < host_n_gpio; i++) {
        if (host_gpio[i].live && host_gpio[i].pin == pin) {   /* re-register same pin: replace */
            host_gpio[i].chan_id = chan_id; host_gpio[i].handle_idx = handle_idx; return;
        }
    }
    for (i = 0; i < host_n_gpio; i++) {
        if (!host_gpio[i].live) {
            host_gpio[i].pin = pin; host_gpio[i].chan_id = chan_id;
            host_gpio[i].handle_idx = handle_idx; host_gpio[i].live = 1; return;
        }
    }
    if (host_n_gpio < HOST_MAX_GPIO) {
        host_gpio[host_n_gpio].pin = pin; host_gpio[host_n_gpio].chan_id = chan_id;
        host_gpio[host_n_gpio].handle_idx = handle_idx; host_gpio[host_n_gpio].live = 1;
        host_n_gpio++;
    } else { fprintf(stderr, "host gpio table full\n"); abort(); }
}
static void host_gpio_fire(int pin) {
    int i;
    for (i = 0; i < host_n_gpio; i++)
        if (host_gpio[i].live && host_gpio[i].pin == pin)
            pulse_post_isr(host_gpio[i].chan_id, host_gpio[i].handle_idx);
}
```

Add the three cases inside `platform_dispatch()` (beside `PRIM_pf_advanceMillis`, following the exact arg/return idiom of the neighboring cases; `gpioPin:edge:post:on:` args mirror the keyword order — `args[0]=n`, `args[1]=edge`, `args[2]=msg`, `args[3]=chanPath`):

```c
    case PRIM_pf_gpioPin: {
        oop path = args[3];
        if (path == NULL || IS_SMI(path) || StringVtable == NULL ||
            obj_vtable(path) != StringVtable) {
            fprintf(stderr, "VM Error: platform gpioPin:edge:post:on: expects a String channel\n");
            abort();
        }
        struct String *p = (struct String *)path;
        int chan_id = space_intern((const char *)p->bytes, p->length);
        int hidx = handle_new(args[2]);                 /* pin the message oop via M6 */
        host_gpio_register((int)UNTAG_SMI(args[0]), chan_id, hidx);
        *out = platform_object();
        return 1;
    }
    case PRIM_pf_gpioFire:
        host_gpio_fire((int)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_pumpEvents:
        *out = TAG_SMI(pulse_pump());
        return 1;
```

- [ ] **Step 4: Extend the C test to drive the prims** — add to `~/workspace/tuvm/tests/test_pulse.c` `main()` before the final `printf("test_pulse: OK\n")`:

```c
    /* drive the real prim path: gpioPin registers, gpioFire simulates the edge,
       pumpEvents drains — the same route the nsl host gate uses. */
    extern int platform_dispatch(int pid, int argc, oop *args, oop *out);
    extern oop platform_object(void);
    {
        /* build a String channel "sys/gpio" the way the VM would; reuse space path */
        oop out = NULL;
        oop reg_args[4];
        /* args: pin=3, edge=#rising(as SMI 1), msg=TAG_SMI(99), chan=String */
        /* Construct a String via the kernel's helper if available; otherwise this
           block is covered indirectly by the nsl gate (Task 5) and may be omitted.
           Keeping the prim exercised at C level requires a String constructor; use
           the same one test_space.c uses to build path strings (see its make_str
           over StringVtable + memcpy of bytes/length). */
        (void)reg_args; (void)out;
    }
```

Note: constructing a `struct String` oop by hand in C is verbose; the authoritative prim-level test is the nsl gate in Task 5 (which runs both A and B paths). If `test_space.c` has a reusable `make_string(const char*)` helper, mirror it here to assert `gpioFire`+`pumpEvents` land `TAG_SMI(99)` on `sys/gpio`; otherwise rely on Task 5. Do not block Task 3 on hand-rolling a String.

- [ ] **Step 5: Build the engine — verify it compiles and existing C tests pass**

Run: `cd ~/workspace/tuvm && tools/run_c_tests.sh 2>&1 | tail -5`
Expected: all lines `PASS`/`SKIP`, no `FAIL`. (This confirms the enum/names/prim_ids additions compile and don't shift ids — `test_space`, `test_platform` still pass.)

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git add src/dispatch.c src/prim_ids.h src/platform.c tests/test_pulse.c
git commit -m "feat(prim): gpioPin:edge:post:on:/gpioFire:/pumpEvents platform prims + host GPIO mock"
```

---

### Task 4: nsl `GpioPulse` capability + seam forwarders

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.ns` (nested class + forwarders + accessor)

**Interfaces:**
- Consumes: the three prims from Task 3 (`platform gpioPin:edge:post:on:`, `platform gpioFire:`, `platform pumpEvents`), `Channel>>resolve:`, `Channel>>out:value:`.
- Produces the nsl surface used by tests and the demo-node:
  - `GpioPulse usingSpace: aSpace seam: aKernel` (class-side factory).
  - `aGpio pin: n edge: e post: aMessage on: aChannel` — registers via the seam; stores nothing in nsl (C owns the slot).
  - `aGpio fire: n` — host/test only: triggers the simulated edge.
  - Kernel forwarders `gpioPin:edge:post:on:`, `gpioFire:`, `pumpEvents`, and a `gpio` accessor.

- [ ] **Step 1: Add the `GpioPulse` class** as a flat nested sibling after `Timers` (~line 1042 in `lib/kernel.ns`):

```smalltalk
  (* Zephyr rung-2 sub-project 1: the ISR→channel pulse capability. Registration
     hands the seam a (pin, edge, message, channel-path); C pins the message via the
     M6 handle table and stores the ISR slot. On target a GPIO edge ISR posts the
     pulse; on host, `fire:` simulates the edge. No nsl-side state — data, not policy. *)
  class GpioPulse = ( | space seam | ) (
    public pin: n edge: e post: aMessage on: aChannel = (
      seam gpioPin: n edge: e post: aMessage on: aChannel rootPath.
      ^ self )
    public fire: n = ( seam gpioFire: n. ^ self )   (* host/test only *)
    setSpace: s seam: c = ( space:: s. seam:: c. ^ self )
  ) : (
    public usingSpace: s seam: c = ( ^ self new setSpace: s seam: c )
  )
```

Note: `aChannel rootPath` yields the fully-resolved path string the prim interns. Confirm `Channel` exposes `rootPath` (the agent survey shows `rootPath = ( ^ root )` at kernel.ns:~610); if it is not `public`, pass `(aChannel channel: '') ` semantics instead by adding a `public rootPath` or by having `pin:edge:post:on:` accept the already-resolved path. Prefer making `rootPath` public if needed (one-line change, additive).

- [ ] **Step 2: Add the seam forwarders** beside the timer forwarders (~line 1256):

```smalltalk
  public gpioPin: n edge: e post: m on: path = ( ^ platform gpioPin: n edge: e post: m on: path )
  public gpioFire: n = ( ^ platform gpioFire: n )
  public pumpEvents = ( ^ platform pumpEvents )
```

- [ ] **Step 3: Add the `gpio` accessor** beside `public timers` (~line 1303):

```smalltalk
  public gpio = ( ^ GpioPulse usingSpace: self space seam: self )
```

- [ ] **Step 4: Sanity-compile the kernel via an existing test** (no new test yet — Task 5 adds it). Run one existing supervision test through both paths to confirm the kernel still compiles:

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/11_supervision/06_timers.ns'))); print(r.run_test_file_oimg(Path('../nsl-tests/11_supervision/06_timers.ns')))"
```
Expected: two `('PASS', ...)` tuples. A parse/compile error in the new class shows here.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/nsl-tuvm
git add lib/kernel.ns
git commit -m "feat(kernel): GpioPulse pulse capability + gpioPin/gpioFire/pumpEvents forwarders"
```

---

### Task 5: Host AB gate — `13_pulse/01_pulse_roundtrip.ns`

**Files:**
- Create: `~/workspace/nsl-tests/13_pulse/01_pulse_roundtrip.ns`

**Interfaces:**
- Consumes: `GpioPulse` (Task 4), `platform space`, `Channel>>whenever:do:`, `Channel>>out:value:`, `platform pumpEvents`, `space drain`.
- Produces: an `expect:`-gated conformance test proving register → simulated edge → pump → reactor observation, on BOTH Path A and Path B.

- [ ] **Step 1: Write the test** — `~/workspace/nsl-tests/13_pulse/01_pulse_roundtrip.ns`

```smalltalk
(*
expect:
observed done
drained 1
finished
*)
(* Zephyr rung-2 sub-project 1 — the ISR→channel pulse path, host differential form.
   Register a GPIO pulse to post #done on 'sensor/alert'; simulate the edge (fire:)
   which routes through the REAL pulse mailbox + M6 handle table; pumpEvents drains
   it (thread-context space_post_id); space drain fans it to the observer. Same C
   path the on-target GPIO ISR uses; only the stimulus (fire: vs a real edge) differs. *)
class PulseRoundtrip packageUsing: manifest = () (
  public main: platform args: args = (
    | space alert gpio drained |
    space:: platform space.
    alert:: space channel: 'sensor/alert'.
    alert whenever: '' do: [ :e | ('observed ' , e printString) out ].

    gpio:: platform gpio.
    gpio pin: 13 edge: #rising post: #done on: alert.

    gpio fire: 13.                                  (* simulated GPIO edge -> mailbox *)
    drained:: platform pumpEvents.                  (* mailbox -> space queue *)
    space drain.                                    (* reactor observes #done *)
    ('drained ' , drained printString) out.
    'finished' out.
  )
) : (
)
```

- [ ] **Step 2: Run the new test on both paths — verify it passes**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/13_pulse/01_pulse_roundtrip.ns'))); print(r.run_test_file_oimg(Path('../nsl-tests/13_pulse/01_pulse_roundtrip.ns')))"
```
Expected: two `('PASS', 'Output matches expectations.')` tuples. If Path A fails but B passes (or vice-versa), the prim id or a solidified-image staleness is the cause — re-run after `git checkout -- src/solidified_image.c src/solidified_image.stmap` in tuvm.

- [ ] **Step 3: Controller runs the FULL suite** (controller-owned, ~10 min — do NOT run inside a subagent):

Run: `cd ~/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -8`
Expected: `Failed: 0`, all gates green, the discovered-file count increased by 1, `13_pulse/01_pulse_roundtrip.ns ... PASS`.

- [ ] **Step 4: Commit** (both repos; restore the tuvm image clobber first)

```bash
cd ~/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
cd ~/workspace/nsl-tests
git add 13_pulse/01_pulse_roundtrip.ns
git commit -m "test(pulse): 13_pulse host gate — register/fire/pump/observe on both paths"
```

---

### Task 6: Zephyr pulse backend (`k_msgq`, ISR-safe)

**Files:**
- Create: `~/workspace/tuvm/src/backend/zephyr/pulse_zephyr.c`

**Interfaces:**
- Consumes: `space_post_id` (space.h), `handle_get` (handles.h), Zephyr `<zephyr/kernel.h>` (`K_MSGQ_DEFINE`, `k_msgq_put`, `k_msgq_get`).
- Produces: the same `pulse_post_isr` / `pulse_pump` seam as posix, but ISR-safe (mailbox = `k_msgq`) and blocking-first in `pulse_pump`.

- [ ] **Step 1: Create the zephyr impl** — `~/workspace/tuvm/src/backend/zephyr/pulse_zephyr.c`

```c
/* Target pulse backend: an ISR-safe k_msgq mailbox (pattern from st-zephyr's
   test-dongle-05-jast/src/hal/zephyr/jast_event.c). A GPIO/timer ISR calls
   pulse_post_isr -> k_msgq_put(K_NO_WAIT) with a (chan_id, handle_idx) token; the
   run-to-completion main loop calls pulse_pump, which BLOCKS for the first item
   (K_FOREVER), then drains the rest (K_NO_WAIT) and, in thread context, resolves the
   M6 handle to an oop and calls space_post_id. space.c stays irq-free. */
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include <zephyr/kernel.h>

struct PulseItem { int chan_id; int handle_idx; };
K_MSGQ_DEFINE(pulse_q, sizeof(struct PulseItem), OEVM_PULSE_MAILBOX_DEPTH, 4);

void pulse_post_isr(int chan_id, int handle_idx) {
    struct PulseItem it = { chan_id, handle_idx };
    if (k_msgq_put(&pulse_q, &it, K_NO_WAIT) != 0) {
        printk("pulse mailbox full\n");
        k_oops();                     /* Tier-2 crash, target equivalent of abort() */
    }
}

int pulse_pump(void) {
    struct PulseItem it;
    int drained = 0;
    /* block for the first event (idle safe point), then drain all currently queued */
    if (k_msgq_get(&pulse_q, &it, K_FOREVER) == 0) {
        space_post_id(it.chan_id, handle_get(it.handle_idx));
        drained++;
        while (k_msgq_get(&pulse_q, &it, K_NO_WAIT) == 0) {
            space_post_id(it.chan_id, handle_get(it.handle_idx));
            drained++;
        }
    }
    return drained;
}
```

- [ ] **Step 2: Verify it compiles against Zephyr headers** (deferred to Task 7's build — this file only compiles inside the west build). For now, a syntax check with a stubbed kernel.h:

Run:
```bash
cd ~/workspace/tuvm && gcc -fsyntax-only -std=c99 -Isrc \
  -D'K_MSGQ_DEFINE(a,b,c,d)=int a' -D'k_msgq_put(a,b,c)=0' -D'k_msgq_get(a,b,c)=0' \
  -D'K_NO_WAIT=0' -D'K_FOREVER=0' -D'printk=printf' -D'k_oops()=abort()' \
  -include stdio.h -include stdlib.h src/backend/zephyr/pulse_zephyr.c && echo SYNTAX-OK
```
Expected: `SYNTAX-OK` (real Zephyr-header compile happens in Task 7).

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm
git add src/backend/zephyr/pulse_zephyr.c
git commit -m "feat(seam): zephyr pulse backend — ISR-safe k_msgq mailbox, blocking pump"
```

---

### Task 7: Zephyr `native_sim` bring-up (CI gate)

**Files:**
- Create: `~/workspace/tuvm/zephyr/CMakeLists.txt`
- Create: `~/workspace/tuvm/zephyr/prj.conf`
- Create: `~/workspace/tuvm/zephyr/src/main.c`
- Create: `~/workspace/tuvm/zephyr/README.md` (build/run commands)

**Interfaces:**
- Consumes: the engine sources, `pulse_zephyr.c`, the nsl image entry point (Model A solidified or Model B `.oimg`), the `[ platform pumpEvents. space drain ] repeat` loop (emitted from the node's `main:` — for the gate, provided by a tiny built-in node or by driving the loop from C after boot).
- Produces: a `native_sim` binary that boots the VM, a one-shot `k_timer` fires a simulated pulse, and the reactor prints the observed message.

**Approach note (honest):** exact CMake/overlay lines are adapted from `ns-zephyr/app` and WILL need one iteration against the local `~/ncs` workspace (SDK `2ac5840438`). Treat the code below as the concrete starting point; the acceptance check is the console output, and the plan expects to adjust `prj.conf`/CMake include paths on first build.

- [ ] **Step 1: Copy the toolchain env** (one-time, per shell):

```bash
source ~/workspace/ns-zephyr/tools/env.sh
# verifies: ZEPHYR_BASE=~/ncs/zephyr, ZEPHYR_SDK_INSTALL_DIR=~/ncs/toolchains/2ac5840438/opt/zephyr-sdk
env | grep -E 'ZEPHYR_BASE|ZEPHYR_SDK|ZEPHYR_TOOLCHAIN'
```

- [ ] **Step 2: Write `prj.conf`** — `~/workspace/tuvm/zephyr/prj.conf` (minimal; console + a generous main stack + heap for the VM arena; NO OpenThread):

```
CONFIG_MAIN_STACK_SIZE=8192
CONFIG_HEAP_MEM_POOL_SIZE=32768
CONFIG_STDOUT_CONSOLE=y
CONFIG_CBPRINTF_FP_SUPPORT=y
CONFIG_TEST=n
```

- [ ] **Step 3: Write `CMakeLists.txt`** — `~/workspace/tuvm/zephyr/CMakeLists.txt` (mirrors `ns-zephyr/app/CMakeLists.txt`: pull engine `.c` from this repo's `src/`, select the zephyr backend, pass the arena size via env because NCS sysbuild swallows `-D`):

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuvm_pulse)

set(TUVM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_sources(app PRIVATE
  src/main.c
  ${TUVM_SRC}/src/vm.c ${TUVM_SRC}/src/gc.c ${TUVM_SRC}/src/dispatch.c
  ${TUVM_SRC}/src/oimg_loader.c ${TUVM_SRC}/src/kernel_boot.c
  ${TUVM_SRC}/src/platform.c ${TUVM_SRC}/src/serial.c ${TUVM_SRC}/src/udp.c
  ${TUVM_SRC}/src/mirror.c ${TUVM_SRC}/src/space.c ${TUVM_SRC}/src/handles.c
  ${TUVM_SRC}/src/solidified_image.c
  ${TUVM_SRC}/src/backend/zephyr/platform_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/serial_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/udp_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/pulse_zephyr.c
)
target_include_directories(app PRIVATE ${TUVM_SRC}/src)
if(DEFINED ENV{OEVM_HEAP_SIZE})
  target_compile_definitions(app PRIVATE OEVM_HEAP_SIZE=$ENV{OEVM_HEAP_SIZE})
endif()
```

- [ ] **Step 4: Write `src/main.c`** — `~/workspace/tuvm/zephyr/src/main.c` (adapted from `ns-zephyr/app/src/main.c`; a one-shot `k_timer` simulates the GPIO edge for the sim gate — Task 8 swaps it for a real GPIO ISR):

```c
/* tuvm on Zephyr — native_sim bring-up gate for the ISR→channel pulse seam.
   Boots the solidified VM (Model A), then a one-shot k_timer fires from ISR context
   into the pulse mailbox (simulating the GPIO edge); the VM's reactor loop drains it.
   On real hardware (Task 8) the k_timer stimulus is replaced by a GPIO edge ISR. */
#include <zephyr/kernel.h>
#include <stdio.h>

extern void init_solidified_vtables(void);
extern void run_solidified_entry_point(void);   /* runs the image's main:, which sets up
                                                    reactions and runs [pumpEvents. drain] repeat */
extern void gc_set_stack_base(void *base);
extern void pulse_post_isr(int chan_id, int handle_idx);
extern int  space_intern(const char *path, size_t len);
extern void handle_new_stub(void);               /* not used directly here; see note */

/* The stimulus: fire a pulse ~1s after boot so the booted node observes one edge.
   NOTE: the (chan_id, handle_idx) must match what the booted node registered. For the
   sim gate the simplest contract is: the node registers pin 13 -> #done on 'sensor/alert'
   via gpioPin:...; on native_sim gpioFire: is unavailable from C easily, so instead the
   node itself schedules the timer via `timers after:` and treats the timer token as the
   pulse. If keeping a raw C stimulus, post directly to the same channel id the node used. */
static void sim_edge(struct k_timer *t) {
    /* Post nothing here if the node drives its own stimulus (preferred). Kept as the
       hook where a real GPIO ISR callback will live in Task 8. */
    ARG_UNUSED(t);
}
K_TIMER_DEFINE(sim_timer, sim_edge, NULL);

int main(void) {
    static char stack_anchor;
    setvbuf(stdout, NULL, _IONBF, 0);
    gc_set_stack_base(&stack_anchor);
    init_solidified_vtables();
    k_timer_start(&sim_timer, K_SECONDS(1), K_NO_WAIT);
    run_solidified_entry_point();   /* blocks in the nsl [pumpEvents. drain] repeat loop */
    return 0;
}
```

**Design reconciliation for the gate stimulus:** the cleanest sim stimulus that needs no extra C is to have the *booted node's* `main:` use the existing host-independent path: register the pulse AND, for the sim, drive the first edge with `gpio fire:` before entering the loop, OR schedule it. The recommended gate node (baked into the solidified image for this build) is:

```smalltalk
class PulseNode packageUsing: manifest = () (
  public main: platform args: args = (
    | space alert gpio |
    space:: platform space.
    alert:: space channel: 'sensor/alert'.
    alert whenever: '' do: [ :e | ('observed ' , e printString) out ].
    gpio:: platform gpio.
    gpio pin: 13 edge: #rising post: #done on: alert.
    gpio fire: 13.                              (* sim: one edge, then run the loop *)
    [ platform pumpEvents. space drain ] repeat  (* the C-owned pump inside nsl loop *)
  )
) : ( )
```

For the native_sim gate this deterministic single-edge form (fire once, then loop) proves the whole path without needing a hardware interrupt; `pumpEvents` will block after the first drain, which on native_sim simply idles — acceptable for the gate (the observed line prints before the block).

- [ ] **Step 5: Build for native_sim**

Run:
```bash
cd ~/workspace/tuvm/zephyr && source ~/workspace/ns-zephyr/tools/env.sh
west build -b native_sim -p always . 2>&1 | tail -20
```
Expected: `west build` completes; `build/zephyr/zephyr.exe` exists. First-build failures are almost always missing `prj.conf` symbols or include paths — fix and rebuild (this is the expected one iteration).

- [ ] **Step 6: Run native_sim — verify the pulse is observed**

Run: `cd ~/workspace/tuvm/zephyr && timeout 5 ./build/zephyr/zephyr.exe 2>&1 | head`
Expected: a line `observed done` in the output.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git add zephyr/CMakeLists.txt zephyr/prj.conf zephyr/src/main.c zephyr/README.md
git commit -m "feat(zephyr): native_sim bring-up gate — VM boot + pulse observed end-to-end"
```

---

### Task 8: nRF52840 acceptance bar (real GPIO ISR)

> **✅ DONE 2026-07-08 — passed on the nRF52840-DK.** Physical BUTTON1 press →
> `observed done` on the DK's uart0/VCOM console. The steps below were drafted
> for the *dongle*; the bench hardware is the *DK*, so the actual recipe differs
> (board `nrf52840dk/nrf52840`; button **P0.11** via a `pulsegpio` alias on the
> existing `button0`, not a hand-rolled P1.06 node; `west flash --runner jlink`
> over the on-board J-Link, not nrfutil DFU; console on uart0/VCOM; a separate
> `prj_dk.conf` + `PulseNodeDK.ns` that drops the sim `gpio fire:`; and a RAM
> retune for the 256 KB part — 128 KB arena, method/prim caches 4096→512). The
> `gpio_target_fire(int)` platform.c entry landed as designed. Authoritative
> recipe: `tuvm/zephyr/README.md` → "Task 8 — nRF52840-DK acceptance bar".

**Files:**
- Modify: `~/workspace/tuvm/zephyr/src/main.c` (real GPIO edge callback for the dongle)
- Create: `~/workspace/tuvm/zephyr/boards/nrf52840dongle_nrf52840.overlay` (button/ALERT pin)

**Interfaces:**
- Consumes: Zephyr `<zephyr/drivers/gpio.h>`, `pulse_post_isr`, the node's registered `(chan_id, handle_idx)`.
- Produces: a physical-edge → console `observed done` on the dongle.

**Approach note:** this task requires the physical nRF52840 dongle and `nrfutil`/`west flash`. The GPIO edge wiring is lifted from `st-zephyr/.../test-dongle-05-jast/port/be_native_button.c`.

- [ ] **Step 1: Add the board overlay** — `~/workspace/tuvm/zephyr/boards/nrf52840dongle_nrf52840.overlay` (expose the on-board button, P1.06 on the dongle, as the edge source):

```dts
/ {
    aliases { pulsegpio = &button0; };
    buttons {
        compatible = "gpio-keys";
        button0: button_0 {
            gpios = <&gpio1 6 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>;
            label = "Pulse edge";
        };
    };
};
```

- [ ] **Step 2: Add the real GPIO ISR** to `zephyr/src/main.c` — replace the `sim_edge`/`K_TIMER` block with a GPIO callback that posts the SAME `(chan_id, handle_idx)` the node registered. Because the node registers via `gpioPin:...`, the C side already stored the slot on target; the ISR looks it up by pin. Add a target `host_gpio_fire`-equivalent that the ISR calls, OR (simpler for the bar) have the ISR post to the interned channel directly with the node's handle. Concretely, wire the Zephyr GPIO callback to call the same registration the `gpioPin:` prim stored — expose `void gpio_target_fire(int pin);` from `platform.c` (guarded `#ifdef` for the zephyr target) mirroring `host_gpio_fire`, and call it from the ISR:

```c
#include <zephyr/drivers/gpio.h>
static const struct gpio_dt_spec pulse_in =
    GPIO_DT_SPEC_GET(DT_ALIAS(pulsegpio), gpios);
static struct gpio_callback pulse_cb;
extern void gpio_target_fire(int pin);   /* platform.c, zephyr target: posts pin's slot */

static void pulse_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    gpio_target_fire(6);   /* P1.06 -> the node's registered pin */
}

static void wire_pulse_gpio(void) {
    gpio_pin_configure_dt(&pulse_in, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&pulse_in, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&pulse_cb, pulse_isr, BIT(pulse_in.pin));
    gpio_add_callback(pulse_in.port, &pulse_cb);
}
```

Call `wire_pulse_gpio();` in `main()` before `run_solidified_entry_point();`, and drop the node's `gpio fire: 13` line (real edges now drive it; keep `pin: 6` in the node to match the overlay).

- [ ] **Step 3: Add `gpio_target_fire` to platform.c** (target build only — reuses the host GPIO slot table, which is present in every build):

```c
/* Target GPIO edge entry: the Zephyr GPIO ISR calls this; posts the pin's slot to
   the pulse mailbox exactly like the host mock's gpioFire:. Same table, same seam. */
void gpio_target_fire(int pin) { host_gpio_fire(pin); }
```

(Make `host_gpio_fire` non-static or add this wrapper directly beneath it in the same file.)

- [ ] **Step 4: Build + flash the dongle**

Run:
```bash
cd ~/workspace/tuvm/zephyr && source ~/workspace/ns-zephyr/tools/env.sh
west build -b nrf52840dongle/nrf52840 -p always . 2>&1 | tail -20
# put the dongle in bootloader mode (press RESET), then:
west flash 2>&1 | tail -10
```
Expected: build + flash succeed.

- [ ] **Step 5: Observe the acceptance bar** — open the dongle's USB-CDC console, press the button:

Run: `cat /dev/ttyACM0` (or the enumerated CDC device) — then press the dongle button.
Expected: `observed done` prints on each press. This is the rung's acceptance bar.

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
git add zephyr/src/main.c zephyr/boards/nrf52840dongle_nrf52840.overlay src/platform.c
git commit -m "feat(zephyr): nRF52840 acceptance bar — real GPIO edge ISR drives the pulse"
```

---

### Task 9: Precursor — `FOOTPRINT_CONFIG=mcu` (tuvm#5)

**Files:**
- Modify: `~/workspace/tuvm/tools/footprint.sh`

**Interfaces:**
- Consumes: `arm-zephyr-eabi-gcc` (from the NCS SDK on PATH after `env.sh`), the engine `.c` set (now including `handles.c`, `pulse_backend` zephyr impl).
- Produces: a real cross-compiled ROM(`text`)/RAM(`data+bss`) line under `FOOTPRINT_CONFIG=mcu`, consumable by `review/tests/t0-gates/ratchet.sh` (which already expects the `mcu` config).

- [ ] **Step 1: Add the mcu branch** to `~/workspace/tuvm/tools/footprint.sh` — after the existing host-proxy build, branch on `FOOTPRINT_CONFIG`:

```bash
# FOOTPRINT_CONFIG=mcu: real arm-zephyr-eabi cross build (host proxy replaced by the
# true-target figure). Requires the NCS SDK on PATH (source ns-zephyr/tools/env.sh).
if [ "${FOOTPRINT_CONFIG:-host}" = "mcu" ]; then
  XCC=${XCC:-arm-zephyr-eabi-gcc}
  MCPU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
  $XCC -Os -std=c99 -Isrc $MCPU -c \
    src/vm.c src/dispatch.c src/gc.c src/kernel_boot.c src/platform.c \
    src/serial.c src/udp.c src/mirror.c src/oimg_loader.c src/space.c src/handles.c \
    src/backend/zephyr/platform_zephyr.c src/backend/zephyr/serial_zephyr.c \
    src/backend/zephyr/udp_zephyr.c src/backend/zephyr/pulse_zephyr.c 2>/dev/null || {
      echo "mcu build failed — is the NCS SDK on PATH? (source ns-zephyr/tools/env.sh)"; exit 2; }
  arm-zephyr-eabi-size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o \
        mirror.o oimg_loader.o space.o handles.o \
        platform_zephyr.o serial_zephyr.o udp_zephyr.o pulse_zephyr.o
  rm -f *.o
  exit 0
fi
```

(Note: `pulse_zephyr.c` needs Zephyr headers, so a standalone `arm-zephyr-eabi-gcc` compile of it will fail on `<zephyr/kernel.h>`. For the footprint line, either exclude `pulse_zephyr.o` and note it, or add `-I$ZEPHYR_BASE/include` from the sourced env. Prefer excluding the two backend files that need Zephyr headers from the *engine* footprint — the ratchet measures the engine core, and the backend seam is measured inside the west build. Adjust the object list accordingly and document the exclusion in a comment.)

- [ ] **Step 2: Run it — verify a real number prints**

Run:
```bash
cd ~/workspace/tuvm && source ~/workspace/ns-zephyr/tools/env.sh
FOOTPRINT_CONFIG=mcu tools/footprint.sh 2>&1 | tail -3
```
Expected: an `arm-zephyr-eabi-size` total line with `text data bss` columns (real Cortex-M4 figures), not the host-proxy line.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm
git add tools/footprint.sh
git commit -m "feat(metrics): FOOTPRINT_CONFIG=mcu — real arm-zephyr-eabi ROM/RAM line (tuvm#5)"
```

---

### Task 10: Precursor — `OEVM_BUILD=asan|ubsan` harness knob

**Files:**
- Modify: `~/workspace/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: the `CC` interpolation point in `run_c_tests.sh` (the only injection point).
- Produces: `OEVM_BUILD=asan` / `OEVM_BUILD=ubsan` prepends the sanitizer flags to `CC`, so the whole C-test suite (incl. `test_handles`, `test_pulse`) runs sanitized.

- [ ] **Step 1: Add the knob** near the top of `~/workspace/tuvm/tools/run_c_tests.sh` (after `CC=${CC:-gcc}`):

```bash
# OEVM_BUILD=asan|ubsan: sanitizer coverage for the ISR-adjacent C (mailbox ring,
# handle table). Injected via CC (the only build knob run() interpolates). Host-only.
case "${OEVM_BUILD:-}" in
  asan)  CC="$CC -fsanitize=address -fno-omit-frame-pointer -g" ;;
  ubsan) CC="$CC -fsanitize=undefined -fno-sanitize-recover=undefined -g" ;;
  "" )   ;;
  * ) echo "unknown OEVM_BUILD='$OEVM_BUILD' (want asan|ubsan)"; exit 2 ;;
esac
```

- [ ] **Step 2: Run the suite under ASan — verify clean**

Run: `cd ~/workspace/tuvm && OEVM_BUILD=asan tools/run_c_tests.sh 2>&1 | grep -E 'FAIL|ERROR|runtime error|AddressSanitizer' || echo 'ASAN CLEAN'`
Expected: `ASAN CLEAN` (all tests PASS with no sanitizer diagnostics). The `expect_abort` fork tests intentionally SIGABRT — confirm those still print `PASS` (ASan does not flag a deliberate abort).

- [ ] **Step 3: Run under UBSan — verify clean**

Run: `cd ~/workspace/tuvm && OEVM_BUILD=ubsan tools/run_c_tests.sh 2>&1 | grep -E 'FAIL|runtime error' || echo 'UBSAN CLEAN'`
Expected: `UBSAN CLEAN`.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm
git add tools/run_c_tests.sh
git commit -m "feat(harness): OEVM_BUILD=asan|ubsan sanitizer knob for the C test suite"
```

---

## Self-review

**Spec coverage** (each spec section → task):
- §2.1 pulse backend seam (`pulse_backend.h`, posix + zephyr impls) → Tasks 2, 6.
- §2.2 M6 handle table → Task 1.
- §2.3 nsl-owned loop + `pumpEvents` mechanism prim → Tasks 3 (prim), 4 (forwarder), 7 (`[...] repeat` in the node).
- §3 GPIO capability nsl surface (`pin:edge:post:on:`) → Task 4; exercised in Tasks 5, 7, 8.
- §4.1 reuse map (ns-zephyr boot, st-zephyr mailbox/GPIO) → Tasks 6, 7, 8.
- §4.2 FOOTPRINT_CONFIG=mcu → Task 9.
- §4.3 OEVM_BUILD=asan|ubsan → Task 10.
- §5 testing: C unit (Tasks 1, 2), nsl host gate (Task 5), native_sim (Task 7), nRF52840 (Task 8).
- §6 constraints: handle-index-not-oop (Tasks 1–3), ISR irq-free (Task 6), nsl-owned loop avoids re-entry (Tasks 4, 7), fixed-capacity frozen-string aborts (Tasks 1, 2, 6), non-moving GC supporting (Task 1 test).

**Known iteration points (not placeholders — flagged honestly):** the exact prim ids in Task 3 Step 2 must be COUNTED from `PRIM_NAMES` at implementation time (the plan states 94–96 as the expected count but instructs to verify); the Zephyr build files in Tasks 7–8 are concrete adaptations of `ns-zephyr`/`st-zephyr` that expect one build-time iteration against the local `~/ncs` workspace; the C-level prim test in Task 3 Step 4 defers authoritative prim coverage to the nsl gate (Task 5) rather than hand-rolling a `struct String`.

**Type/name consistency:** `pulse_post_isr(int,int)`, `pulse_pump()→int`, `space_post_id(int,oop)`, `handle_new(oop)→int`/`handle_get(int)→oop`/`handle_free(int)`/`handle_gc_trace()`, prims `gpioPin:edge:post:on:`/`gpioFire:`/`pumpEvents`, nsl `GpioPulse`/`pin:edge:post:on:`/`fire:`/`gpio` — used consistently across Tasks 1–8.

## Order & dependencies

Tasks 1→2→3→4→5 are the host vertical slice (each independently testable; land + verify before proceeding). Task 6 depends on 2. Task 7 depends on 1,2,3,4,6. Task 8 depends on 7. Tasks 9 and 10 are independent precursors (can land any time after Task 1 adds `handles.c` to the source set). Recommended execution order: 1, 2, 3, 4, 5, (10, 9 precursors), 6, 7, 8.
