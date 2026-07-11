# Rung 8 — On-target timer seam + self-gating Thread node — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Back the nsl `Timers` seam with a REAL Zephyr `k_timer` backend, and use it to run the Thread echo node on a **15 s autonomous cadence** — so it works on the **headless** nRF52840 dongle, proving its inbound leg with no console via a **self-gating train**.

**Architecture:** A pure lift mirroring the proven pulse seam. The virtual clock moves verbatim out of `src/platform.c` into `src/backend/posix/timer_posix.c`; a new `src/backend/zephyr/timer_zephyr.c` backs the same four prims with `k_timer`, whose ISR callback posts through the **existing** `pulse_post_isr → k_msgq → pulse_pump → space_post_id` path onto the reserved `sys/timer` channel — exactly what `host_advance` does today. No VM, no `Timers` class, no `NetBridge`, no CBOR change.

**Tech Stack:** C99 engine (`tuvm`), Newspeak dialect (`nsl-tuvm/lib/kernel.ns`, frozen), Zephyr/NCS v3.2.4 `k_timer` + OpenThread MTD, tuvm Model-A solidifier (`ns_compile_oevm.py`), west + J-Link (DK) / nRF Util DFU (dongle), the live `udp-echo6` echo on `gw85224-01`.

**Spec:** `docs/superpowers/specs/2026-07-11-rung8-ontarget-timer-design.md`

## Global Constraints

- **No VM / `Timers` / `NetBridge` / CBOR / facade change.** `lib/kernel.ns` is FROZEN. This rung adds C backend files, board config, and node `.ns` files only. The existing prim ids 85–88 keep their numbers and their nsl-visible behavior. If `kernel.ns` *needs* editing, STOP and escalate — a premise broke.
- **The lift is VERBATIM.** `timer_posix.c` is the current `platform.c` code relocated, not rewritten. Any behavior change to the host virtual clock is a bug, and Gate 0 will catch it.
- **MTD only, never FTD** (David, firm — RAM/MPSL boot-fault lesson).
- **The Thread network key is a SECRET.** It lives ONLY in the gitignored `prj_dk_thread_local.conf` / `prj_dongle_thread_local.conf`. Never write it into a committed file, this plan, a commit message, or a log paste.
- **`solidified_image.c` is a generated artifact.** Baking a node CLOBBERS `tuvm/src/solidified_image.c` + `.stmap`. ALWAYS restore before committing anything in tuvm:
  ```bash
  git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
  ```
  **Never** run a bake concurrently with the conformance suite — `run_tests_oevm.py` regenerates the same file per test and the two will clobber each other.
- **Zephyr build needs the SDK env** sourced in each shell: `source /home/david/workspace/ns-zephyr/tools/env.sh`
- **DK/dongle build config-D env**, exported before any bake/build:
  ```bash
  export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
  export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
  ```
- **THE BUILD-LIST GOTCHA — a new engine `.c` must be added to EVERY build site.** The SDD ledger (line 100) records this exact failure: a new seam file broke the portability-32 gate because `tuvm/tests/test_portability_32.sh` is a **shell** script and was missed by a `tools/*.py` sweep. The complete list, with the pattern to copy:

  | # | File | What to add |
  |---|------|-------------|
  | 1 | `nsl-tuvm/tools/run_tests_oevm.py` | `f"src/backend/{OEVM_TARGET}/timer_{OEVM_TARGET}.c"` in `OEVM_BACKEND` |
  | 2 | `nsl-tuvm/tools/test_supervisor_grant.py` | beside `pulse_posix.c` |
  | 3 | `nsl-tuvm/tools/test_debugger_integration.py` | beside `pulse_posix.c` |
  | 4 | **`tuvm/tests/test_portability_32.sh`** | `"src/backend/$TARGET/timer_$TARGET.c"` on the `-m32` gcc line (**the historical trap**) |
  | 5 | `tuvm/tools/run_c_tests.sh` | beside `src/backend/posix/pulse_posix.c` in `BACKEND` |
  | 6 | `tuvm/tests/test_oimg_blob.sh` | beside the other `$BACKEND` files |
  | 7 | `tuvm/tools/footprint.sh` | the **host** build (~line 81–84) needs `timer_posix.c`; the **engine-core** build (~line 42) must NOT get it (it excludes all backends by design — see Task 5) |
  | 8 | `tuvm/zephyr/CMakeLists.txt` | `${TUVM_SRC}/src/backend/zephyr/timer_zephyr.c` |

  After editing, verify none were missed: `grep -rln 'pulse_posix\|pulse_\$\|pulse_{' <both repos>` and confirm every hit that is a *build* site also names a timer file.
- **Commit-message trailer** on every commit:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```
- **Three repos, separate commits.** `tuvm` (C seam + board conf + nodes + docs), `nsl-tuvm` (build-list + plan/spec), `nsl-tests` (only if a test lands). Never cross-commit; `cd` into the right repo before `git add`.

---

## File Structure

- `tuvm/src/timer_backend.h` — CREATE (Task 1). The seam contract; sibling of `pulse_backend.h`.
- `tuvm/src/backend/posix/timer_posix.c` — CREATE (Task 1). The host virtual clock, moved VERBATIM from `platform.c`.
- `tuvm/src/platform.c` — MODIFY (Task 1). Delete the `host_timer_*`/`host_advance`/`host_clock_ms` block; prims 85–88 + `advanceMillis:` call the seam.
- The 8 build sites above — MODIFY (Task 1).
- `tuvm/src/backend/zephyr/timer_zephyr.c` — CREATE (Task 2). `k_timer` → `pulse_post_isr`.
- `tuvm/zephyr/TimerNode.ns` — CREATE (Task 2). native_sim gate: `TIMER OK`.
- `tuvm/zephyr/ThreadTimerNode.ns` — CREATE (Task 3). The 15 s self-gating node; ONE source, both boards.
- `tuvm/zephyr/README.md`, `tuvm/zephyr/FOOTPRINT_DK.md`, `tuvm/zephyr/docs/BOARD_PORTS.md`, `tuvm/review/program-sequence.md` — MODIFY (Task 5).

---

## Task 1: The timer seam lift (host behavior must not move)

Extract the virtual clock into a backend, exactly as the pulse seam is structured. **No behavior change anywhere.** The gate is the whole floor staying green.

**Files:**
- Create: `tuvm/src/timer_backend.h`, `tuvm/src/backend/posix/timer_posix.c`
- Modify: `tuvm/src/platform.c` (delete lines ~14–92, the timer block; rewire prims at ~511–528)
- Modify: the 8 build sites in Global Constraints

**Interfaces:**
- Consumes: `space_post` (`src/space.h`), `TAG_SMI`/`UNTAG_SMI` (`src/oevm.h`).
- Produces: `timer_backend_after(int token, uint64_t ms)`, `timer_backend_every(int token, uint64_t ms)`, `timer_backend_cancel(int token)`, `uint64_t timer_backend_millis(void)`, `int timer_backend_advance(uint64_t ms)`. Task 2 supplies the Zephyr impl of these exact signatures.

- [ ] **Step 1: Create `tuvm/src/timer_backend.h`**

```c
#ifndef OEVM_TIMER_BACKEND_H
#define OEVM_TIMER_BACKEND_H
/* The timer seam. Exactly one impl is linked per OEVM_TARGET:
   backend/posix/timer_posix.c   — a deterministic VIRTUAL clock. Nothing moves it but
                                   the nsl-level `advanceMillis:` prim, so timer firing
                                   order is exactly reproducible across runs and across
                                   Path A/B. This is the host test rig's clock.
   backend/zephyr/timer_zephyr.c — a REAL clock (k_timer + k_uptime_get). The expiry ISR
                                   posts through the existing pulse mailbox
                                   (pulse_post_isr), which pulse_pump drains in thread
                                   context onto the reserved `sys/timer` channel — the
                                   identical path host_advance posts on. pulse_backend.h
                                   already sanctions this: "A GPIO/timer ISR calls
                                   pulse_post_isr".

   The C side carries DATA ONLY (a token, a millisecond count). Which channel, what
   message, and all cancel bookkeeping live in nsl's `Timers` class (lib/kernel.ns).
   See the design spec §2. */
#include <stdint.h>

void     timer_backend_after(int token, uint64_t ms);   /* one-shot */
void     timer_backend_every(int token, uint64_t ms);   /* repeating */
void     timer_backend_cancel(int token);               /* MUST release any M6 handle it holds */
uint64_t timer_backend_millis(void);                    /* monotonic ms since boot */

/* Advance the virtual clock by `ms` and post every now-due token; answers the number of
   fires. HOST ONLY — the target has a real clock, so the zephyr impl is a no-op that
   answers 0. Backs the `advanceMillis:` prim. */
int      timer_backend_advance(uint64_t ms);
#endif /* OEVM_TIMER_BACKEND_H */
```

- [ ] **Step 2: Create `tuvm/src/backend/posix/timer_posix.c` — the verbatim lift**

Move the block from `src/platform.c` (the `HOST_MAX_TIMERS` `#define` through `host_advance`) into this file **unchanged**, renaming only the five entry points to the seam names. Do not "improve" anything: the 0 ms repeater floor, the dead-slot reuse, and the multi-fire inner loop are all load-bearing and covered by tests.

```c
#include "../../timer_backend.h"
#include "../../space.h"
#include "../../oevm.h"
#include <stdint.h>

/* The host virtual-clock timer facility (Milestone D Task 6; lifted verbatim out of
   src/platform.c in rung 8 when the seam gained a real on-target backend). A
   deterministic event source: the host test rig never reads wall time. `timer_backend_
   advance` is the ONLY thing that moves the clock, driven exclusively by the nsl-level
   `advanceMillis:` prim, so firing order is fully reproducible across runs and across
   Path A/B. Each live HostTimer is a (token, due_ms, interval_ms, repeating, live) slot
   in a small fixed table (HOST_MAX_TIMERS is a host-rig cap, not a VM limit).
   `host_timer_add` reuses a dead slot before growing the table, so after:/every: churn
   doesn't leak slots. C carries DATA ONLY (token, ms) — which channel and what message
   live in nsl's `Timers` class; this file posts just TAG_SMI(token) onto the reserved
   `sys/timer` channel (9 = strlen("sys/timer")). */
#define HOST_MAX_TIMERS 32
struct HostTimer { int token; uint64_t due_ms; uint64_t interval_ms; int repeating; int live; };
static struct HostTimer host_timers[HOST_MAX_TIMERS];
static int host_n_timers = 0;
static uint64_t host_clock_ms = 0;

static void host_timer_add(int token, uint64_t ms, int repeating) {
    int i;
    /* A repeating timer with a 0ms interval would never let due_ms advance past
       host_clock_ms in the fire loop below, hanging the process on the very next
       advanceMillis: call. Floor the interval to 1ms for repeaters only — a one-shot
       with ms==0 is harmless (fires once on the next advance, then deactivates). */
    if (repeating && ms == 0) ms = 1;
    for (i = 0; i < host_n_timers; i++) {
        if (!host_timers[i].live) {
            host_timers[i].token = token;
            host_timers[i].due_ms = host_clock_ms + ms;
            host_timers[i].interval_ms = ms;
            host_timers[i].repeating = repeating;
            host_timers[i].live = 1;
            return;
        }
    }
    if (host_n_timers < HOST_MAX_TIMERS) {
        host_timers[host_n_timers].token = token;
        host_timers[host_n_timers].due_ms = host_clock_ms + ms;
        host_timers[host_n_timers].interval_ms = ms;
        host_timers[host_n_timers].repeating = repeating;
        host_timers[host_n_timers].live = 1;
        host_n_timers++;
    }
    /* else: silently drop — host test rig; on-target uses the real timer facility */
}

void timer_backend_after(int token, uint64_t ms) { host_timer_add(token, ms, 0); }
void timer_backend_every(int token, uint64_t ms) { host_timer_add(token, ms, 1); }

void timer_backend_cancel(int token) {
    int i;
    for (i = 0; i < host_n_timers; i++)
        if (host_timers[i].live && host_timers[i].token == token) host_timers[i].live = 0;
}

uint64_t timer_backend_millis(void) { return host_clock_ms; }

/* Advance the virtual clock by `ms`; post each now-due timer's token onto sys/timer. A
   repeater whose interval is smaller than the advance can fire more than once per call
   (the inner while keeps posting and rescheduling until it is no longer due); a one-shot
   fires at most once and then deactivates. Answers the total number of fires. */
int timer_backend_advance(uint64_t ms) {
    int i, fired = 0;
    host_clock_ms += ms;
    for (i = 0; i < host_n_timers; i++) {
        struct HostTimer *t = &host_timers[i];
        while (t->live && t->due_ms <= host_clock_ms) {
            oop tok = TAG_SMI(t->token);
            space_post("sys/timer", 9, tok);
            fired++;
            if (t->repeating) t->due_ms += t->interval_ms; else { t->live = 0; break; }
        }
    }
    return fired;
}
```

- [ ] **Step 3: Rewire `src/platform.c`**

Delete the whole lifted block (the `HOST_MAX_TIMERS` define through `host_advance`, roughly lines 14–92 — stop BEFORE the `/* Host GPIO mock */` comment, which stays). Add the include beside the existing `#include "pulse_backend.h"`:

```c
#include "timer_backend.h"
```

Then replace the five prim bodies (currently ~lines 511–528) so they call the seam. The `*out` conventions are unchanged:

```c
    case PRIM_pf_timerAfter:
        timer_backend_after(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerEvery:
        timer_backend_every(UNTAG_SMI(args[1]), (uint64_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_timerCancel:
        timer_backend_cancel(UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    case PRIM_pf_monotonicMillis:
        *out = TAG_SMI((long)timer_backend_millis());
        return 1;
    case PRIM_pf_advanceMillis:
        *out = TAG_SMI(timer_backend_advance((uint64_t)UNTAG_SMI(args[0])));
        return 1;
```

- [ ] **Step 4: Add `timer_$TARGET.c` to all 8 build sites**

Use the table in Global Constraints. Two that are easy to get wrong:

`nsl-tuvm/tools/run_tests_oevm.py` (~line 24) — the list is target-parameterized, so it is one line:
```python
OEVM_BACKEND = [f"src/backend/{OEVM_TARGET}/platform_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/serial_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/udp_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/pulse_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/timer_{OEVM_TARGET}.c"]
```

`tuvm/tests/test_portability_32.sh` (~line 19) — **the trap.** Append to the `-m32` gcc line, after `pulse_$TARGET.c`:
```sh
"src/backend/$TARGET/timer_$TARGET.c"
```

- [ ] **Step 5: Run the C tests + the full both-path suite (Gate 0, part 1)**

```bash
cd /home/david/workspace/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -5
cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py 2>&1 | tail -5
```

Expected: C tests all PASS; suite **`180 passed / 0 failed / 10 skipped`** (10, not 12 — rung 7's close retired `43_udp_mesh`/`44_udp_thread`). The five timer tests (`11_supervision/06_timers`, `07_event_seam`, `08_timer_zero_interval`, `09_demo_node_integration`, `02_crash_at_depth`) are the ones that prove the lift was faithful. **A failure here means the lift was not verbatim — diff `timer_posix.c` against the deleted block before doing anything else.**

- [ ] **Step 6: Run the five named gates (Gate 0, part 2)**

```bash
cd /home/david/workspace/nsl-tuvm
python3 tools/test_supervisor_grant.py        # grant
python3 tools/test_debugger_integration.py    # debugger-integration
cd /home/david/workspace/tuvm
bash tests/test_portability_32.sh             # portability-32  <- the build-list trap
bash tools/footprint.sh                       # mcu-footprint
```
(boot-gc-asan runs inside `tools/run_c_tests.sh` — confirm its line said PASS in Step 5.)

Expected: all five PASS. `portability-32` failing to **link** (`undefined reference to timer_backend_*`) is the signature of the missed build site — fix site #4 and re-run. The `mcu-footprint` engine-core figure will **drop** (the timer code left a measured TU for an excluded backend TU); that is expected and is re-banked honestly in Task 5. If the ratchet gate FAILS on a drop, note the number and continue — Task 5 re-banks it.

- [ ] **Step 7: Commit (tuvm + nsl-tuvm, separately)**

```bash
cd /home/david/workspace/tuvm
git add src/timer_backend.h src/backend/posix/timer_posix.c src/platform.c \
        tools/run_c_tests.sh tools/footprint.sh tests/test_portability_32.sh \
        tests/test_oimg_blob.sh zephyr/CMakeLists.txt
git commit -m "refactor(rung8): lift the timer facility behind a backend seam

src/timer_backend.h + backend/posix/timer_posix.c: the host virtual clock moved
VERBATIM out of platform.c, exactly mirroring the pulse seam. Prims 85-88 +
advanceMillis: now dispatch through the seam; no id shifts, no nsl-visible change.
Opens the door for a real on-target k_timer backend (next commit). Host floor
unchanged: 180/0/10 both paths + all five named gates green.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd /home/david/workspace/nsl-tuvm
git add tools/run_tests_oevm.py tools/test_supervisor_grant.py tools/test_debugger_integration.py
git commit -m "build(rung8): link timer_\$TARGET.c into the host build lists

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: The Zephyr `k_timer` backend + the native_sim gate

Prove the real timer end-to-end with **zero hardware**: `k_timer` ISR → `pulse_post_isr` → `k_msgq` → `pumpEvents` → `sys/timer` → `Timers dispatch` → reaction.

**Files:**
- Create: `tuvm/src/backend/zephyr/timer_zephyr.c`
- Create: `tuvm/zephyr/TimerNode.ns`

**Interfaces:**
- Consumes: `timer_backend.h` (Task 1); `pulse_post_isr` (`src/pulse_backend.h`); `space_intern` (`src/space.h`); `handle_new`/`handle_free` (`src/handles.h`); `TAG_SMI` (`src/oevm.h`).
- Produces: the linked Zephyr timer impl; `TimerNode.ns` printing `TIMER OK`.

- [ ] **Step 1: Create `tuvm/src/backend/zephyr/timer_zephyr.c`**

```c
#include "../../timer_backend.h"
#include "../../pulse_backend.h"
#include "../../space.h"
#include "../../handles.h"
#include "../../oevm.h"
#include <zephyr/kernel.h>

/* Target timer backend: a real k_timer per live registration. The expiry callback runs
   in ISR context, so it does exactly what a GPIO edge ISR does and nothing more — it
   calls pulse_post_isr (non-allocating, non-blocking) with the (chan_id, handle_idx)
   resolved at REGISTRATION time in thread context. pulse_pump then resolves the handle
   and calls space_post_id in thread context, posting TAG_SMI(token) onto the reserved
   `sys/timer` channel — the identical path the host virtual clock posts on via
   space_post. pulse_backend.h sanctions this: "A GPIO/timer ISR calls pulse_post_isr".
   No nsl code ever runs in ISR context. */

#ifndef OEVM_MAX_TARGET_TIMERS
#define OEVM_MAX_TARGET_TIMERS 8
#endif

struct TargetTimer {
    struct k_timer t;      /* MUST be first — timer_expiry recovers the slot from it */
    int token;
    int chan_id;
    int handle_idx;        /* M6 handle pinning TAG_SMI(token); freed on cancel/reap */
    int repeating;
    int live;
};
static struct TargetTimer target_timers[OEVM_MAX_TARGET_TIMERS];

/* ISR context. The ONLY thing that may happen here. */
static void timer_expiry(struct k_timer *t) {
    struct TargetTimer *e = CONTAINER_OF(t, struct TargetTimer, t);
    if (e->live) pulse_post_isr(e->chan_id, e->handle_idx);
}

/* Thread context. A fired one-shot leaves a dead slot holding an M6 handle; reclaim it
   lazily at the next registration (handle_free is NOT ISR-safe, so it cannot happen in
   timer_expiry). k_timer_status_get answers >0 iff the timer fired since we last asked. */
static void reap_oneshots(void) {
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (e->live && !e->repeating && k_timer_status_get(&e->t) > 0) {
            handle_free(e->handle_idx);
            e->live = 0;
        }
    }
}

static void timer_start(int token, uint64_t ms, int repeating) {
    reap_oneshots();
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (!e->live) {
            e->token      = token;
            e->chan_id    = space_intern("sys/timer", 9);   /* the reserved channel */
            e->handle_idx = handle_new(TAG_SMI(token));     /* pin the token oop via M6 */
            e->repeating  = repeating;
            e->live       = 1;
            k_timer_init(&e->t, timer_expiry, NULL);
            k_timer_start(&e->t, K_MSEC(ms), repeating ? K_MSEC(ms) : K_NO_WAIT);
            return;
        }
    }
    printk("timer table full\n");
    k_oops();                     /* Tier-2 crash, as pulse_post_isr does when full */
}

void timer_backend_after(int token, uint64_t ms) { timer_start(token, ms, 0); }
void timer_backend_every(int token, uint64_t ms) { timer_start(token, ms, 1); }

void timer_backend_cancel(int token) {
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (e->live && e->token == token) {
            k_timer_stop(&e->t);
            handle_free(e->handle_idx);   /* or the slot leaks an M6 handle per cancel */
            e->live = 0;
        }
    }
}

uint64_t timer_backend_millis(void) { return (uint64_t)k_uptime_get(); }

/* No virtual clock on target — the clock is real and advances on its own. */
int timer_backend_advance(uint64_t ms) { (void)ms; return 0; }
```

- [ ] **Step 2: Create `tuvm/zephyr/TimerNode.ns` (the native_sim gate)**

Note the dialect rules: `[ ... ] whileTrue:`, never `repeat`; hold ONE `Timers` instance (`platform timers` answers a FRESH one each call — calling it twice would lose the registrations).

```
(*
TimerNode — Rung 8 gate 1. Proves the REAL on-target timer backend
(src/backend/zephyr/timer_zephyr.c) with NO radio and NO hardware, on native_sim:
k_timer ISR -> pulse_post_isr -> k_msgq -> pumpEvents -> sys/timer channel ->
Timers dispatch -> the reaction. A 200ms repeating timer must fire 3 times, then
the node prints TIMER OK. `platform pumpEvents` BLOCKS (K_FOREVER) between ticks,
so this node also demonstrates that the VM idles the CPU while waiting on a timer.
expect:
TIMER OK
*)
class TimerNode usingPlatform: platform = ( | ticks ::= 0. | ) (
  public main: platform2 args: args = (
    | space timers ping |
    space:: platform space.
    timers:: platform timers.     (* hold ONE instance — it owns the pending registry *)
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    ping:: space channel: 'tick'.
    ping whenever: '' do: [ :m | ticks:: ticks + 1 ].
    timers every: 200 post: #t on: ping.
    [ ticks < 3 ] whileTrue: [ platform pumpEvents. space drain ].
    'TIMER OK' println.
    ^ 0
  )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 3: Bake + build + run on native_sim**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/TimerNode.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b native_sim -p always . 2>&1 | tail -5
timeout 10 ./build/zephyr/zephyr/zephyr.exe > /tmp/rung8_timer.log 2>&1
cat /tmp/rung8_timer.log
cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: `TIMER OK` in the log, within ~1 s (3 × 200 ms). Capture to a **file**, never `| head` — a pipe artifact has faked "no output" before (SDD ledger line 114).
If it hangs with no `TIMER OK`: the timer never posted. Check (a) `timer_zephyr.c` is actually in `zephyr/CMakeLists.txt` (site #8), (b) `space_intern("sys/timer", 9)` matches the channel the node's `whenever:` registered.

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/tuvm
git add src/backend/zephyr/timer_zephyr.c zephyr/TimerNode.ns
git commit -m "feat(rung8): real Zephyr k_timer backend for the Timers seam

k_timer expiry (ISR) -> pulse_post_isr -> k_msgq -> pulse_pump -> space_post_id
onto the reserved sys/timer channel: the identical path the host virtual clock
posts on, and the one pulse_backend.h already sanctions ('A GPIO/timer ISR calls
pulse_post_isr'). monotonicMillis becomes real (k_uptime_get). Registration pins
TAG_SMI(token) via an M6 handle in thread context; cancel + one-shot reap free it.
Gate: zephyr/TimerNode.ns on native_sim -> TIMER OK (no radio, no hardware).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: The self-gating 15 s node, on the DK (console)

One node source for BOTH boards. Prove it on the DK, where the console is visible AND the gateway train can be watched at the same time — that lockstep is what **calibrates** the headless gate before the dongle relies on it alone.

**Files:**
- Create: `tuvm/zephyr/ThreadTimerNode.ns`

**Interfaces:**
- Consumes: frozen `kernel.ns` (`platform space`/`timers`/`NetBridge`/`Record`/`pumpEvents`/`monotonicMillis`, `net udpBindPort:`), the Task-2 Zephyr timer backend, the rung-7 `prj_dk_thread.conf` + `thread_bringup()` in `src/main.c` (both unchanged).
- Produces: `ThreadTimerNode.ns` — prints `BRIDGE OK` once per closed round-trip; drives the gateway train.

- [ ] **Step 1: Create `tuvm/zephyr/ThreadTimerNode.ns`**

State lives in **instance slots**, not `main:` locals, so every reaction block is a one-line self-send — the house style that dodges the reified-block LD_SLOT gotcha (nsl-tuvm#10).

```
(*
ThreadTimerNode — Rung 8. The Rung-7 NetBridge round-trip, on a 15s AUTONOMOUS
cadence (real k_timer) instead of a BUTTON1 press. ONE source, BOTH boards: the
DK (console -> BRIDGE OK on uart0/VCOM) and the nRF52840 dongle (HEADLESS —
Issue A; verified from the gateway instead).

THE SELF-GATING TRAIN (the headless proof). The gateway's udp-echo6 log witnesses
only the OUTBOUND leg. So the next send is made CONDITIONAL on the previous
reflection having decoded value-equal: `awaiting` is cleared ONLY by a good
reflection, and `fire` sends ONLY when it is clear. A steady 15s train of echo
lines therefore proves the FULL round-trip is closing every cycle (N lines =
N-1 proven round-trips); a broken inbound leg stops the train dead after one.
`awaiting` is load-bearing, not decorative: without it a timed-out wait would
just send again and fake a healthy train.

DELIBERATE: a single lost datagram wedges this node forever. That is a GATE
property (failure must be loud, never a silent pass), not production behavior.

Dialect: `[ ... ] whileTrue:`, not `repeat`. State is in instance slots so the
reaction blocks stay one-line self-sends (nsl-tuvm#10).
expect (DK console, every ~15s after attach):
BRIDGE OK
*)
class ThreadTimerNode usingPlatform: platform net: net = (
  | space bridge rec awaiting ::= false. |
) (
  gwAddr     = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )  (* gw ML-EID, NOT its OMR *)
  echoPort   = ( ^ 7777 )
  sendPeriod = ( ^ 15000 )   (* ms *)
  waitBudget = ( ^ 3000 )    (* ms to wait for the reflection, real clock *)

  public main: platform2 args: args = (
    | sock timers tickChan |
    space:: platform space.
    sock:: net udpBindPort: 54360.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform NetBridge
      socket: sock out: (space channel: 'echo/out') peer: gwAddr port: echoPort
      in: (space channel: 'echo/in').
    bridge start.
    rec:: platform Record tag: #reading with: #seq -> 1 with: #milliC -> 235.
    (space channel: 'echo/in') whenever: '' do: [ :v | self reflected: v ].
    timers:: platform timers.     (* hold ONE instance — it owns the pending registry *)
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    tickChan:: space channel: 'sensor/tick'.
    tickChan whenever: '' do: [ :m | self fire ].
    timers every: sendPeriod post: #go on: tickChan.
    'THREAD TIMER NODE READY' println.
    [ true ] whileTrue: [
      platform pumpEvents.     (* BLOCKS (K_FOREVER) until the k_timer ISR posts: the
                                  node idles the CPU for 15s and yields to OpenThread *)
      space drain.             (* the timer reaction -> self fire -> send iff armed *)
      self awaitReflection ].
    ^ 0
  )

  (* Send IFF the previous round-trip closed. See the self-gating note above. *)
  fire = (
    awaiting ifTrue: [ ^ self ].              (* still waiting -> stay silent: train stops *)
    awaiting:: true.
    (space channel: 'echo/out') out: '' value: rec.
    ^ self
  )

  (* The ONLY thing that can re-arm the sender. *)
  reflected: v = (
    (v = rec) ifTrue: [ awaiting:: false. 'BRIDGE OK' println ].
    ^ self
  )

  (* The reflection MUST be consumed in the SAME iteration as the send. If we went
     straight back to the blocking pumpEvents, the reflection would land during the 15s
     sleep and sit unread in the socket buffer; the next tick's reaction would run BEFORE
     we drained it, still see awaiting = true, and skip the send — the node would fire
     every 30s instead of 15. Bounded with the now-REAL monotonicMillis (rung 8). The
     poll is non-blocking and net RX preempts it (proven in rung 7). *)
  awaitReflection = (
    | deadline |
    deadline:: platform monotonicMillis + waitBudget.
    [ awaiting and: [ platform monotonicMillis < deadline ] ] whileTrue: [
      bridge pumpInbound. space drain ].
    ^ self
  )

  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Bake, build, and flash the DK (David pre-approved flashing)**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/ThreadTimerNode.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dk/nrf52840 -p always . -- \
  -DCONF_FILE=prj_dk_thread.conf -DEXTRA_CONF_FILE=prj_dk_thread_local.conf 2>&1 | tail -12
west flash --skip-rebuild --runner jlink 2>&1 | tail -5
cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: build RAM/FLASH well under 100 %, flash programs the DK. Record the RAM/ROM figures for Task 5. (If `/dev/ttyACM0` is missing or J-Link times out, power-cycle the debug USB; healthy it enumerates as `1366:1025 SEGGER J-Link`.)

- [ ] **Step 3: Watch BOTH ends for ≥ 3 cycles — the calibration gate**

This is the whole point of doing the dongle's timer on the DK first. **No button press is needed** — the node is autonomous.

```bash
# terminal 1 — the DK console (capture to a file; never pipe to head)
timeout 75 cat /dev/ttyACM0 > /tmp/rung8_dk.log 2>&1 &
# terminal 2 — the gateway echo log, same window
ssh david@gw85224-01 'timeout 75 journalctl -u udp-echo6.service -f -o cat' > /tmp/rung8_gw.log 2>&1
wait; cat /tmp/rung8_dk.log; echo "=== GW ==="; cat /tmp/rung8_gw.log
```

Expected, after the boot banner + `thread: attached role=2`:
1. `THREAD TIMER NODE READY`
2. `BRIDGE OK` on the console roughly every 15 s (≥ 3 of them in 75 s), and
3. a gateway `echo 32B <-> [fdc5:...]:54360` line in **lockstep** with each one.

**The train and the console must tick together.** That correspondence is what licenses trusting the train alone on the dongle. If the console prints `BRIDGE OK` but the gateway shows nothing (or vice versa), STOP — the headless gate is not calibrated and Task 4 cannot be trusted.

Troubleshooting: only ONE gateway line then silence ⇒ the inbound leg is broken and `awaiting` is stuck — exactly the failure the design is built to make visible; debug it here on the DK, where the console can tell you why. Every 30 s instead of 15 ⇒ the reflection is being consumed a cycle late; re-read `awaitReflection` (the ordering note).

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/tuvm
git status --short   # ThreadTimerNode.ns ONLY; solidified_image.* must NOT appear
git add zephyr/ThreadTimerNode.ns
git commit -m "feat(rung8): ThreadTimerNode — 15s autonomous self-gating round-trip

One node source, both boards. The rung-7 NetBridge round-trip on a real k_timer
cadence instead of a BUTTON1 press, so it runs on the headless dongle. The next
send is CONDITIONAL on the previous reflection decoding value-equal, which makes
the gateway's outbound-only echo log witness round-trip closure with no console
(N lines = N-1 proven round-trips; a broken inbound leg stops the train dead).
Bench-verified on the DK: BRIDGE OK every ~15s on VCOM, in lockstep with the
gateway train — which calibrates the headless gate for the dongle.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: The dongle — the headless gate

Same image, same node, no console. The gateway is the only witness, and Task 3 just proved it is a trustworthy one.

**Files:** none (a build + DFU flash of the Task-3 node with the dongle conf).

**Interfaces:** consumes `ThreadTimerNode.ns` (Task 3) + the rung-7 `prj_dongle_thread.conf` + `boards/nrf52840dongle_nrf52840.overlay` (both unchanged); produces the gateway-observed train.

- [ ] **Step 1: Bake + build for the dongle**

```bash
source /home/david/workspace/ns-zephyr/tools/env.sh
export OEVM_HEAP_SIZE=49152 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256
export VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512
cd /home/david/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  /home/david/workspace/tuvm/zephyr/ThreadTimerNode.ns \
  /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm/zephyr
west build -b nrf52840dongle/nrf52840 -p always . -- \
  -DCONF_FILE=prj_dongle_thread.conf -DEXTRA_CONF_FILE=prj_dongle_thread_local.conf 2>&1 | tail -12
cd /home/david/workspace/tuvm && git checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: builds; RAM/FLASH under 100 % (rung 7 measured FLASH 42.55 % / RAM 74.83 % for the button node — the timer node is within noise of that). Record for Task 5.

- [ ] **Step 2: DFU-flash the dongle**

Put it in DFU mode (hold RESET while inserting; it enumerates as `1915:521f`), then:

```bash
cd /home/david/workspace/tuvm/zephyr
west flash --skip-rebuild --runner nrfutil 2>&1 | tail -5
```

- [ ] **Step 3: Verify from the gateway — the headless gate**

The dongle will go dark on USB after reset (Issue A — expected, not a failure). Watch the gateway:

```bash
# attach: a NEW child row (the dongle's RLOC16) — the DK may also be present
ssh david@gw85224-01 'docker exec otbr ot-ctl child table; echo ---; docker exec otbr ot-ctl childip'
# the train: >= 4 lines, ~15s apart
ssh david@gw85224-01 'timeout 75 journalctl -u udp-echo6.service -f -o cat'
```

Expected: a child row for the dongle, and **≥ 4 `echo 32B <-> [fdc5:...]:54360` lines about 15 s apart**. By the self-gating property, 4 lines = **3 proven complete round-trips** over 802.15.4, with no console. That is the rung's proof.

**Note:** if the DK is still powered and running the same node, both boards send from port 54360 and their lines interleave — power the DK down, or tell them apart by the source ML-EID in each log line, before reading the train.

Troubleshooting: exactly ONE line then silence ⇒ outbound works, inbound is broken (the design's loud failure). No lines but a child row ⇒ attached, but `sendto` is failing — re-check that the dongle build really has `CONFIG_OPENTHREAD_MANUAL_START=y` and that `thread_bringup()` (with `net_if_up()`) is compiled in; that was rung 7's `ENETDOWN`. No child row ⇒ the dataset overlay didn't apply (`grep OPENTHREAD_CHANNEL build/zephyr/.config` must show the REAL channel, not the placeholder 11).

- [ ] **Step 4: Record the bench evidence**

Save the child-table row + the timestamped train lines to the SDD ledger. No repo change.

---

## Task 5: Close — footprint re-bank, docs, memory

**Files:**
- Modify: `tuvm/zephyr/FOOTPRINT_DK.md` (the Thread-node figures + the engine-core note)
- Modify: `tuvm/zephyr/README.md` (the rung-8 timer gate + the autonomous node)
- Modify: `tuvm/zephyr/docs/BOARD_PORTS.md` (dongle: ⏳ NEXT → ✅ DONE, headless-but-verified)
- Modify: `tuvm/review/program-sequence.md` (rung-8 line)

- [ ] **Step 1: Re-bank the footprints — and be honest about the engine-core drop**

```bash
cd /home/david/workspace/tuvm && bash tools/footprint.sh 2>&1 | tail -5
```

The engine-core figure **drops** by roughly the size of the lifted virtual clock. This is **not a real win** and must not be recorded as one: `tools/footprint.sh` deliberately excludes every `src/backend/*` TU, so moving the timer code from `platform.c` (measured) into `timer_posix.c` (excluded) shrinks the *measurement* while the on-target image does not shrink at all — `timer_zephyr.c` takes its place. Re-bank the baseline with exactly that explanation in the commit message.

Then record the DK + dongle Thread-node ROM/RAM captured in Tasks 3–4 in `FOOTPRINT_DK.md`, beside the rung-4 loopback figure, noting the delta is OpenThread L2 + mbedTLS (not the timer, which is a few hundred bytes).

- [ ] **Step 2: Document the gates in `tuvm/zephyr/README.md`**

Add a `# Rung 8 — autonomous timer round-trip (ThreadTimerNode.ns)` section with: the native_sim `TIMER OK` recipe (Task 2 Step 3), the DK recipe (Task 3 Step 2) noting **no button press is needed**, the dongle DFU recipe (Task 4), and the self-gating-train explanation (why N gateway lines = N−1 proven round-trips, and why one line then silence is a real failure).

- [ ] **Step 3: Update `BOARD_PORTS.md` + `program-sequence.md`**

`BOARD_PORTS.md`: the dongle row moves from "⏳ NEXT, and the hard case" to **✅ DONE (headless)** — attached, round-tripping, verified from the gateway; Issue A remains unbeaten and is now *routed around* rather than blocking. Add the timer to the "one core, per-board IO" table as a board-agnostic event source (it is not a per-board axis — that is the point).

`program-sequence.md`: add the rung-8 line under the Layer-3 entry — the `Timers` seam has a real on-target backend; the node is autonomous; the dongle is verified headless.

- [ ] **Step 4: Update memory**

Update `milestone-status` (rung 8 done: real k_timer backend behind `timer_backend.h`; `ThreadTimerNode` self-gating 15 s; dongle verified headless from the gateway; suite 180/0/10) and `nsl-zephyr-multi-target` (dongle now DONE-headless; the gateway-train verification recipe).

- [ ] **Step 5: Commit + push all three repos (David's call on the push)**

```bash
cd /home/david/workspace/tuvm
git add zephyr/README.md zephyr/FOOTPRINT_DK.md zephyr/docs/BOARD_PORTS.md \
        review/program-sequence.md tools/footprint-baseline.txt
git commit -m "docs(rung8): close — real on-target timer + headless dongle verified

The Timers seam has a real Zephyr k_timer backend; ThreadTimerNode runs a 15s
autonomous self-gating round-trip. Dongle verified HEADLESS from the gateway
(child table + a sustained 15s echo train = N-1 proven round-trips). Engine-core
footprint re-banked DOWN — NOT a real win: the virtual clock moved from
platform.c (measured) to backend/posix/timer_posix.c (excluded by design from
footprint.sh); the on-target image does not shrink, timer_zephyr.c replaces it.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage** (each design § → a task):
- §2 the timer seam (contract, verbatim posix lift, zephyr k_timer via pulse, the two invariants: cancel frees the handle, advance is a no-op on target) → Task 1 (lift + contract) + Task 2 (zephyr impl; `timer_backend_cancel` + `reap_oneshots` both `handle_free`; `timer_backend_advance` returns 0). ✓
- §3 the self-gating train (`awaiting` semantics, N lines = N−1 round-trips, the deliberate wedge) → Task 3 Step 1 (`fire`/`reflected:`) + Task 4 Step 3 (reading the train). ✓
- §4 the node (one source both boards, the loop ordering, instance slots for the LD_SLOT gotcha) → Task 3 Step 1, with the ordering rationale in `awaitReflection`'s comment. ✓
- §5 proof matrix: Gate 0 floor → Task 1 Steps 5–6; Gate 1 native_sim → Task 2 Step 3; Gate 2 DK → Task 3 Step 3; Gate 3 dongle → Task 4 Step 3. ✓
- §6 risks: floor-moves → Gate 0 before any hardware; handle leak → Task 2 Step 1; build-list → Global Constraints table (8 sites, incl. the `test_portability_32.sh` trap the ledger records); `advanceMillis:` no-op → Task 1 Step 5 (the five timer tests are host/posix, so they keep the virtual clock); OT starvation → disproven in rung 7. ✓
- §7 out of scope (Issue A, LED seam, production retry) → named, not built. ✓

**Placeholder scan:** none. Every C file, `.ns` node, build-list edit, and command is given in full. The one thing deliberately not spelled out is the *content* of the gitignored dataset overlays — a security requirement, not a gap.

**Type consistency:** `timer_backend_{after,every,cancel,millis,advance}` have identical signatures in `timer_backend.h` (Task 1 Step 1), `timer_posix.c` (Step 2), the `platform.c` call sites (Step 3), and `timer_zephyr.c` (Task 2 Step 1). The nsl surface is unchanged and frozen: `timers every: ms post: msg on: chan` + `space whenever: 'sys/timer' do: [:e | timers dispatch: e]` match `11_supervision/06_timers.ns` exactly; `platform NetBridge socket:out:peer:port:in:` + `start` + `pumpInbound` and `platform Record tag:with:with:` match the rung-6/7 call sites. `TimerNode.ns` and `ThreadTimerNode.ns` both hold ONE `platform timers` instance (it answers a fresh one per call). ✓

**Known gap, deliberate:** `reap_oneshots` reclaims a fired one-shot's handle only at the *next* registration. A program that registers many one-shots and never registers again holds those handles until exit. `OEVM_MAX_TARGET_TIMERS` (8) bounds the leak, `k_oops` makes exhaustion loud, and this rung's nodes use `every:` exclusively. Documented rather than solved (YAGNI); revisit if a node leans on `after:` on target.
