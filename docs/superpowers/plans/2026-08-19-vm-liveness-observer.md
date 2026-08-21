# VM Liveness Observer Implementation Plan (#215)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **STATUS 2026-08-19 — EXECUTED THROUGH TASK 10, then blocked.** Tasks 1-10 are done on
> branch `215-vm-liveness-observer` (`722cd34..e997fc9`). Tasks 11-13 are blocked by
> **#222**: the armed bridge wedges at ~31 s where the pre-#215 build ran 21.4 h clean on
> the same board. What the bench actually said — including the three plan predictions that
> hardware confirmed and the two false greens caught along the way — is §11 of the design
> doc, which is the durable record; this file is scaffolding. Corrections found while
> executing: `run.sh --board` takes a board ID (`esp32c6`) and not a bench board name, it
> builds by default and flashes only with `--flash`, and the target harness needed a
> `TAP_NAME` override plus a pristine-on-config-change guard before two scenarios could
> share one workload. New follow-ons: **#222**, **#223**, **#224**.

**Goal:** Give a wedged node a way to say that it stopped — detect the silence that raises no
fault, dump the crime scene to a console the collector already persists, and report it as one
`Panic { reason: #watchdog, … }` on `sys/panic` after the reboot.

**Architecture:** A cooperative-priority supervisor thread samples two conditions once a second,
each judged against the app's **own declared timer periods** rather than any constant chosen
here. On a wedge verdict it dumps, fills a `RebootNote` in `.rtc_noinit`, and either reboots or —
on the third consecutive note with no successful check-in between — halts. A `task_wdt` channel
sits **behind** the supervisor as the backstop for the supervisor itself dying, with the C6
hardware WDT behind that. On the next boot the app reads the note through a new platform prim and
publishes the already-pinned `Panic` record.

**Tech Stack:** C99 (the tuvm VM + its Zephyr port), Zephyr `subsys/task_wdt` + `wdt_esp32`,
`RTC_NOINIT_ATTR` on ESP32-C6, nsl (`nodus/nsl/lib/kernel.nsl` and the app nodes under
`nodus/tuvm/zephyr/`), and the target gate harness under `nodus/tuvm/tests/target/`.

**Spec:** `docs/design/2026-08-16-vm-liveness-observer-design.md` (APPROVED 2026-08-17), which
adopts `docs/design/2026-08-10-node-memory-and-liveness-design.md` §5.1/§5.2/§8 verbatim on the
reporting path. Both travel with this plan; the executor reads both. The record is pinned in
`protocol/PROTOCOL-NSL-RECORDS.md` §2.4.

---

## Global Constraints

- **`nodus/tuvm/zephyr/src/main.c` is the ONLY place target-side leaves live** — the
  `bell_target_*` / `ntp_target_*` / `rtc_target_*` family is the precedent
  (`nodus/tuvm/src/platform.c:437-460`). A symbol declared `extern` in `platform.c` must link on
  **every** Zephyr board: **guard the function body, never the definition.**
- **Prim ids are name-resolved and append-only.** A new id is arrived at by **COUNTING**
  `PRIM_NAMES` in `nodus/tuvm/src/dispatch.c`, never by arithmetic on the last one. The parity
  check at `dispatch.c:944` enforces the 1:1 correspondence at compile time and must be updated
  to name the new last enum entry.
- **Adding a `.c` file to the VM engine means editing NINE enumerated source lists** (#63). This
  plan deliberately adds **no** engine `.c` file — see "Decisions taken in this plan" below. The
  only build-list edit is `nodus/tuvm/zephyr/CMakeLists.txt`.
- **All timestamps are milliseconds. Monotonic only** — nodes have no clock (PROTOCOL-NSL §3).
- **`Panic.detail` is the one place truncation is legal** (RECORDS §2.4), *"because a partial
  reason beats no reason."*
- **A false trip is the failure mode that would make this instrument worse than nothing**
  (design §8). Where a choice trades detection speed against false-trip risk, take the slow side.
- **The counters are monotonic and never reset** (design §4). The deadline resets constantly;
  `consecutive` resets on a successful check-in. These are three different things and conflating
  them is how this goes wrong.
- **No Kconfig bench/field split for the detector** (design §9). It ships always-on. The two
  Kconfig symbols this plan adds gate *test triggers* only, in the shape of the existing
  `CONFIG_TUVM_FAULT_TRIGGER`.
- Every new C file carries the tree's existing header-comment style: what it is, why it is not
  the obvious alternative, and what it was measured against.
- Gates that must stay green: `cd nodus/nsl && python3 tools/run_tests_oevm.py` (read the **aux
  gate line**, not the pass count), `cd nodus/tuvm && bash tools/run_c_tests.sh`,
  `cd porta && go test ./...`, `cd nodus/tuvm && bash tests/porta_sync_host.sh`.

---

## Decisions taken in this plan, and why they differ from the design

The design is approved; these are implementation-level readings of it, each forced by something
in the tree. **They are listed here so a reviewer can reject any of them in one place**, and
§10's promotion step carries the survivors back into the design doc.

### D-P1. The supervisor is a THREAD, and `task_wdt` sits behind it rather than in front

Design §4 reads as though `task_wdt` is the detector: the callback dumps and reboots. It cannot
be, for a mechanical reason:

> `task_wdt_feed()` calls `k_sched_lock()` (`subsys/task_wdt/task_wdt.c:240`), and `k_sched_lock()`
> asserts `!arch_is_in_isr()` (`kernel/sched.c:774`). **`CONFIG_ASSERT` is not set in the C6
> build** (`zephyr/build-esp32c6-B/zephyr/.config`), so an ISR-context feed would not fail loudly —
> it would decrement `_current->base.sched_locked` against whatever thread was interrupted.

So the feed must happen in thread context. Everything the tree could feed it from — the reactor —
is exactly the thing that wedges. Hence a dedicated supervisor thread.

Once there is a thread, the verdict is better taken **in** it: `printk` and `sys_reboot` from
thread context are the ordinary paths, the dump can be formatted freely, and the `task_wdt`
channel is then free to be what it should be — the backstop for **the supervisor itself** dying,
a genuinely different failure. That is a strict improvement on the design's shape, not a
compromise with it.

The thread runs at a **cooperative** priority. A preemptible one at the main thread's priority
would never be scheduled while the VM spins in the interpreter, so the trip would come from a
missed feed against a fixed channel period — which is precisely the global "how often should a
node wake" constant §2 forbids. Cooperative priority is what keeps the app's own declared periods
the operative deadline.

### D-P2. Two more invented constants than §3.3 counted, and they are named

§3.3 says `SLACK` is the only number this design invents. The thread adds two:

| Constant | Value | Standing |
|---|---|---|
| `OBSERVER_TICK_MS` | 1000 | **Derived, not chosen.** A sampling resolution. It must be far below the shortest deadline in play; the shortest deadline this bench can produce is the bridge's `8 × 2000 ms` = 16 s, which 1 s samples 16×. Earns revision from a node whose shortest declared period is under ~1 s |
| `OBSERVER_WDT_MS` | 30000 | **Chosen, deliberately generous.** It must NEVER be the operative deadline — it exists only for "the supervisor thread stopped running". 30 × the tick. Earns revision only if a supervisor death is ever observed and 30 s proves too slow to matter |

### D-P3. `mcause` comes from the CSR, not from the `esf`

2026-08-10 §5.1's struct carries `mcause, mepc, ra`. On this SoC `struct arch_esf` carries `ra`
and `mepc` but **`mcause` only under `CONFIG_CLIC_SUPPORT_INTERRUPT_LEVEL`**
(`include/zephyr/arch/riscv/exception.h:81-83`), which is unset here — and
`CONFIG_RISCV_SOC_HAS_ISR_STACKING` is also unset, so the plain struct applies. The field stays;
it is filled with `csr_read(mcause)`, which still holds the trap cause when the fatal handler
runs. Dropping the field instead would delete a capability to save a line.

### D-P4. No new engine `.c` file, so the nine source lists are untouched

Design §6 places the module at `src/backend/zephyr/observer.c` and asks for a posix stub so the
host suite compiles against the seam. Following that literally costs nine list edits (#63) to
carry a stub **that nothing on the host calls**.

Instead:

- the **detector** lives at `nodus/tuvm/zephyr/src/observer.c` — app-side, beside the other
  target-only leaves, reached only from `main.c`. One build-list edit
  (`zephyr/CMakeLists.txt`), zero engine lists.
- the **note's logic** (magic, checksum, validity) lives in a **header**,
  `nodus/tuvm/src/reboot_note.h`, as `static inline` — a header needs no list edit, is linked by
  both sides, and is directly unit-testable on the host.
- the **note's storage** is the `rtc_target_*` split verbatim: host impl inline in `platform.c`,
  target impl in `zephyr/src/reboot_note.c`.

§6's actual requirement — *"two readers, one source of truth"* — is met more simply than §6
proposed: the dump calls `pulse_drops()` / `pulse_coalesced()` / `pulse_stale()` from
`pulse_backend.h` directly, which is the same single source `PRIM_pf_sysStats` already reads
(`platform.c:1439-1445`). Nothing is re-exported because nothing is duplicated.

### D-P5. HALT must disable the hardware watchdog first

Not in either design, and it silently inverts §5.2. `task_wdt_init(hw_wdt)` installs a hardware
timeout of `CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY`
(`task_wdt.c:137-138`) with `WDT_FLAG_RESET_SOC`. A halted node feeds nothing, so **the hardware
watchdog resets it** — and the loop breaker's "clean, findable failure" becomes the reboot loop
it was built to stop, with no console line saying why. The HALT arm calls
`wdt_disable(hw_wdt_dev)` before parking. Task 12 gates on exactly this.

### D-P6. One clearing prim, not two, and it carries the boot id

§5.2 wants `consecutive = 0` on the first successful check-in; §5.1 wants the note cleared after
it is reported. Those are the same moment, so they are one call: `panicNoteAck: bootId`. It also
deposits **this** boot's id in the note, so a death later in this boot already carries the id it
died under — the node's C side has no other way to know it. A report written before any ack
carries `bootId = 0`, and **0 means unknown and is never invented into a number**, which is the
convention #217 §6.4 already set for a `gw` row.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `nodus/tuvm/src/reboot_note.h` | The `RebootNote` struct + `static inline` magic/checksum/validity logic + the four seam declarations. Header-only so both sides share the logic and neither build list changes. |
| `nodus/tuvm/tests/test_reboot_note.c` | Host unit test of that logic: garbage is not a report, a written note round-trips, `consecutive` ratchets, an ack clears the report but keeps the boot id. |
| `nodus/tuvm/zephyr/src/reboot_note.c` | Target storage (`RTC_NOINIT_ATTR`) + the four seam impls + the `k_sys_fatal_error_handler` override. |
| `nodus/tuvm/zephyr/src/observer.c` | The supervisor thread, the two wedge conditions, the dump, the reboot/HALT policy, and the `task_wdt` backstop. |
| `nodus/tuvm/zephyr/WedgeProbeC6.nsl` | The disruptive probe: ticks normally, then stops draining forever. |
| `nodus/tuvm/zephyr/SlowTimerNodeC6.nsl` | The legitimately-slow node: one 10-minute timer and nothing else. |
| `nodus/tuvm/tests/target/wedge-note.env` | §8 row 1 — the note survives a watchdog reset. |
| `nodus/tuvm/tests/target/wedge-detect.env` | §8 row 3 — the detector fires on a real wedge. |
| `nodus/tuvm/tests/target/wedge-quiet.env` | §8 row 5 — it does not fire on a slow node. |

**Modified:**

| File | Change |
|---|---|
| `nodus/tuvm/src/platform.c` | Host impls of the note seam; `extern` declarations on the target side; two new prim cases. |
| `nodus/tuvm/src/prim_ids.h` | `PRIM_pf_panicNote` / `PRIM_pf_panicNoteAck`. |
| `nodus/tuvm/src/dispatch.c` | Two `PRIM_NAMES` entries, two enum entries, the parity-check typedef. |
| `nodus/tuvm/src/backend/zephyr/timer_zephyr.c` | `struct TargetTimer` gains `period_ms` / `last_fire_ms`; new `timer_target_snapshot()`. |
| `nodus/tuvm/zephyr/CMakeLists.txt` | `src/reboot_note.c`, `src/observer.c`. |
| `nodus/tuvm/zephyr/src/main.c` | `reboot_note_init()` early; `observer_start()` before the VM runs. |
| `nodus/tuvm/zephyr/Kconfig` | `TUVM_WEDGE_TRIGGER`, `TUVM_WEDGE_TRIGGER_S`. |
| `nodus/tuvm/zephyr/prj_esp32c6.conf` | `CONFIG_WATCHDOG`, `CONFIG_TASK_WDT` and its three sizing values. |
| `nodus/tuvm/zephyr/prj_esp32c6_wedgegate.conf` | New overlay turning the trigger on (never the default build). |
| `nodus/nsl/lib/kernel.nsl` | `panicNote` / `panicNoteAck:` forwarders and the `Panic` record builder. |
| `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl` | Publish `sys/panic` on boot; ack on the first clean check-in. |
| `nodus/tuvm/tools/run_c_tests.sh` | Run `test_reboot_note`. |

---

## Task 1: `RebootNote` — the retained report, and its logic proven on the host first

Design §8's first row is deliberately first: *"Fails by reading zero, which is indistinguishable
from 'no wedge ever happened' — so this is proven before anything is built on it."* Everything
downstream reports through this struct, so its validity rule gets a host test before a board ever
sees it.

**Files:**
- Create: `nodus/tuvm/src/reboot_note.h`
- Create: `nodus/tuvm/tests/test_reboot_note.c`
- Modify: `nodus/tuvm/src/platform.c` (host impls in the `#ifndef` host block near `rtc_target_*`
  at `:395-431`; `extern` declarations in the target block at `:437-460`)
- Modify: `nodus/tuvm/tools/run_c_tests.sh`

**Interfaces:**
- Consumes: nothing.
- Produces — the seam every later task calls:
  ```c
  void     reboot_note_init(void);                       /* boot: validate, or zero the block */
  int      reboot_note_read(struct RebootNote *out);     /* 1 + fills *out if a report is pending */
  void     reboot_note_write(uint32_t reason, uint32_t mcause,
                             uint32_t mepc, uint32_t ra, uint32_t mono_ms);
  void     reboot_note_ack(uint32_t boot_id);            /* clear report, consecutive=0, keep id */
  uint32_t reboot_note_consecutive(void);
  ```
  and the reason codes `RN_REASON_WATCHDOG 1`, `RN_REASON_HARD_FAULT 2`, `RN_REASON_ASSERT 3`,
  `RN_REASON_STACK_OVERFLOW 4`, `RN_REASON_OOM 5`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tuvm/tests/test_reboot_note.c`:

```c
/* Unit test for the retained panic note (src/reboot_note.h, rete#215 / the
   2026-08-10 liveness design §5.1). This struct is the ONLY channel a node that
   stopped has for saying so, and its failure mode is that it reads zero — which is
   byte-identical to "no wedge ever happened". So the validity rule is proven here,
   on the host, before a board is asked to retain anything: a block of garbage must
   NOT decode as a report, or the first boot after a cold power-on invents a panic.

   Build (Model A link set, same as test_cheap_stats.c):
     gcc -std=c99 -Isrc -DOEVM_HEAP_SIZE=65536 tests/test_reboot_note.c \
         <ENG_A> -lm -o /tmp/trn && /tmp/trn

   Verifies:
     1. An uninitialised block is not a report (magic AND checksum, not magic alone).
     2. reboot_note_init() on garbage leaves a clean, report-free block.
     3. A written note round-trips every field.
     4. consecutive RATCHETS across writes and is NOT reset by a read.
     5. reboot_note_ack() clears the report and consecutive but KEEPS the boot id,
        and the next write carries that id (D-P6).
     6. A one-bit corruption of any field invalidates the note. */
#include "../src/reboot_note.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    struct RebootNote n;

    /* 1. garbage is not a report */
    memset(&n, 0xA5, sizeof n);
    assert(!reboot_note_is_valid(&n));
    memset(&n, 0x00, sizeof n);
    assert(!reboot_note_is_valid(&n));
    printf("test_reboot_note: property 1 (garbage is not a report) OK\n");

    /* 2. init leaves a clean block */
    reboot_note_init();
    assert(reboot_note_read(&n) == 0);
    assert(reboot_note_consecutive() == 0);
    printf("test_reboot_note: property 2 (init clears) OK\n");

    /* 3. a written note round-trips */
    reboot_note_write(RN_REASON_WATCHDOG, 0xdeadbeef, 0x42004200, 0x1234, 98765);
    assert(reboot_note_read(&n) == 1);
    assert(n.reason == RN_REASON_WATCHDOG);
    assert(n.mcause == 0xdeadbeef);
    assert(n.mepc   == 0x42004200);
    assert(n.ra     == 0x1234);
    assert(n.mono_ms == 98765);
    assert(n.boot_id == 0);            /* no ack yet: unknown stays 0, never invented */
    printf("test_reboot_note: property 3 (round-trip) OK\n");

    /* 4. consecutive ratchets, and reading does not reset it */
    assert(n.consecutive == 1);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 1);
    reboot_note_write(RN_REASON_HARD_FAULT, 2, 3, 4, 5);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 2);
    reboot_note_write(RN_REASON_ASSERT, 2, 3, 4, 5);
    assert(reboot_note_read(&n) == 1 && n.consecutive == 3);
    printf("test_reboot_note: property 4 (consecutive ratchets) OK\n");

    /* 5. ack clears the report and the counter but KEEPS the boot id */
    reboot_note_ack(77);
    assert(reboot_note_read(&n) == 0);
    assert(reboot_note_consecutive() == 0);
    reboot_note_write(RN_REASON_OOM, 0, 0, 0, 11);
    assert(reboot_note_read(&n) == 1);
    assert(n.boot_id == 77);
    assert(n.consecutive == 1);
    printf("test_reboot_note: property 5 (ack clears, boot id survives) OK\n");

    /* 6. a one-bit corruption invalidates */
    struct RebootNote bad = n;
    bad.mono_ms ^= 1u;
    assert(!reboot_note_is_valid(&bad));
    bad = n;
    bad.consecutive ^= 1u;
    assert(!reboot_note_is_valid(&bad));
    printf("test_reboot_note: property 6 (checksum catches one bit) OK\n");

    printf("test_reboot_note: all properties OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/tuvm && gcc -std=c99 -Isrc tests/test_reboot_note.c -o /tmp/trn
```
Expected: FAIL — `fatal error: ../src/reboot_note.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `nodus/tuvm/src/reboot_note.h`:

```c
#ifndef OEVM_REBOOT_NOTE_H
#define OEVM_REBOOT_NOTE_H
/* The retained panic note — rete#215, adopting 2026-08-10 §5.1 verbatim.

   A node that dies must still be able to say what killed it, and the report has to
   survive the reboot that recovers it. On ESP32 that is `.rtc_noinit` (RTC_NOINIT_ATTR),
   a NOLOAD section which survives both deep sleep and a restart, and which the MagTag
   deep-sleep counter (zephyr/src/main.c:285) already proves in-tree. CONFIG_RETAINED_MEM
   was rejected in §5.1: a driver, a Kconfig and a devicetree edit to store twenty-odd
   bytes.

   THE LOGIC LIVES IN THIS HEADER AND THE STORAGE DOES NOT. Retention differs per side
   (RTC_NOINIT_ATTR on target, a plain static on the host) but the validity RULE must
   not — a rule that is written twice is a rule that will diverge, and the divergence
   would be invisible until a real death. Header-only also means neither the nine engine
   source lists (#63) nor the posix backend gain a file for logic nobody links twice.

   VALIDITY IS MAGIC *AND* CHECKSUM. Magic alone is not enough: uninitialised RTC RAM is
   whatever the last program left there, and a block that happens to open with the magic
   word would be reported as a panic that never happened — inventing a death is worse
   than missing one, because it is unfalsifiable from the gateway. */
#include <stdint.h>
#include <string.h>

/* Maps to Panic.reason (PROTOCOL-NSL-RECORDS.md §2.4). Small ints rather than strings:
   the C side must write this from a fatal handler that may not allocate, and the symbol
   lookup belongs in nsl where the record is built. Append-only. */
#define RN_REASON_WATCHDOG       1u   /* #watchdog       — the wedge detector (rete#215) */
#define RN_REASON_HARD_FAULT     2u   /* #hardFault      — K_ERR_CPU_EXCEPTION and friends */
#define RN_REASON_ASSERT         3u   /* #assert         — K_ERR_KERNEL_OOPS / _PANIC */
#define RN_REASON_STACK_OVERFLOW 4u   /* #stackOverflow  — K_ERR_STACK_CHK_FAIL */
#define RN_REASON_OOM            5u   /* #oom            — reserved; no producer yet */

#define RN_MAGIC 0x52424E31u   /* "RBN1" */

struct RebootNote {
    uint32_t magic;        /* else uninitialised RAM reads as a panic report */
    uint32_t sum;          /* checksum — garbage must not become a report */
    uint32_t reason;       /* RN_REASON_* above; maps to Panic.reason */
    uint32_t mcause;       /* csr_read(mcause) — NOT esf->mcause, see the plan's D-P3 */
    uint32_t mepc;
    uint32_t ra;
    uint32_t boot_id;      /* the boot this node was RUNNING under; 0 means unknown and is
                              never invented into a number (the #217 §6.4 convention) */
    uint32_t mono_ms;      /* uptime at death */
    uint32_t consecutive;  /* §5.2's loop breaker — survives the reboot BY DESIGN */
};

/* Sum every word after `sum` itself, plus the magic. Deliberately trivial: this runs in a
   fatal handler, where anything that can fail must not be here. */
static inline uint32_t reboot_note_checksum(const struct RebootNote *n) {
    return n->magic + n->reason + n->mcause + n->mepc + n->ra
         + n->boot_id + n->mono_ms + n->consecutive + 0x9E3779B9u;
}

static inline int reboot_note_is_valid(const struct RebootNote *n) {
    return (n->magic == RN_MAGIC) && (n->sum == reboot_note_checksum(n));
}

/* Called ONCE at boot, before anything can read the note. Validates the retained block
   and zeroes it whole if it is garbage — so every field below, `boot_id` included, is a
   known value from here on rather than whatever the last program left in RTC RAM. */
void reboot_note_init(void);

/* 1 and fills *out when a report is PENDING (a death that has not yet been acked); 0
   otherwise. Reading NEVER clears: a read that clears loses the report to any transient
   that reads it first, and the app may need to try more than once to publish. */
int reboot_note_read(struct RebootNote *out);

/* Fill and stamp a death. Increments `consecutive` (from 0 if the block held no valid
   note) and carries forward whatever `boot_id` the last ack deposited. Must be callable
   from a fatal handler: no allocation, no logging, no locks. */
void reboot_note_write(uint32_t reason, uint32_t mcause,
                       uint32_t mepc, uint32_t ra, uint32_t mono_ms);

/* The app's "I got far enough to report" signal — §5.2's loop-breaker reset and §5.1's
   report-once clear in one call, because they are the same moment (plan D-P6). Clears
   the pending report and `consecutive`, and deposits THIS boot's id so a later death in
   this boot carries the id it died under. */
void reboot_note_ack(uint32_t boot_id);

/* The loop breaker's live count, for the observer's HALT decision and the dump. */
uint32_t reboot_note_consecutive(void);
#endif /* OEVM_REBOOT_NOTE_H */
```

- [ ] **Step 4: Write the host implementation**

In `nodus/tuvm/src/platform.c`, inside the **host** block (the one holding `rtc_target_valid`
at `:419-421`), immediately after `rtc_target_put`, add:

```c
/* rete#215: the retained panic note, host side. There is no RTC RAM here and no reboot,
   so the block is a plain static: a fresh process is a cold power-on, which is exactly
   the semantic the target gets from a power cycle. That makes the host able to test the
   VALIDITY RULE and the ratchet — the parts that are logic — while leaving RETENTION,
   the part that is hardware, to the target gate that can actually falsify it (design §6:
   the host suite must COMPILE against this seam, never simulate it). */
static struct RebootNote g_note;

void reboot_note_init(void) {
    if (!reboot_note_is_valid(&g_note)) memset(&g_note, 0, sizeof g_note);
}
int reboot_note_read(struct RebootNote *out) {
    if (!reboot_note_is_valid(&g_note)) return 0;
    *out = g_note;
    return 1;
}
void reboot_note_write(uint32_t reason, uint32_t mcause,
                       uint32_t mepc, uint32_t ra, uint32_t mono_ms) {
    uint32_t prev = reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
    /* boot_id is read UNGUARDED, and that is deliberate. reboot_note_ack() clears the
       magic while keeping the id, so the block is legitimately "no report, known
       identity" — a validity-guarded read would take 0 there and every post-ack death
       would report an unknown boot. reboot_note_init() zeroing the whole block on garbage
       is what makes an unguarded read safe. */
    uint32_t boot = g_note.boot_id;
    g_note.magic = RN_MAGIC;
    g_note.reason = reason; g_note.mcause = mcause;
    g_note.mepc = mepc;     g_note.ra = ra;
    g_note.boot_id = boot;  g_note.mono_ms = mono_ms;
    g_note.consecutive = prev + 1;
    g_note.sum = reboot_note_checksum(&g_note);
}
void reboot_note_ack(uint32_t boot_id) {
    memset(&g_note, 0, sizeof g_note);
    g_note.boot_id = boot_id;    /* kept: the note is cleared, the identity is not */
}
uint32_t reboot_note_consecutive(void) {
    return reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
}
```

Add near the top of `platform.c`, with the other includes:

```c
#include "reboot_note.h"
```

And in the **target** block (beside `extern int rtc_target_valid(void);` at `:452`):

```c
/* rete#215: the panic note's leaves live in zephyr/src/reboot_note.c, beside the RTC
   retention block they share a section with. Same rule as bell/ntp/rtc above — the
   symbols must link on EVERY Zephyr board, so that file guards bodies, never
   definitions. */
extern void     reboot_note_init(void);
extern int      reboot_note_read(struct RebootNote *out);
extern void     reboot_note_write(uint32_t reason, uint32_t mcause,
                                  uint32_t mepc, uint32_t ra, uint32_t mono_ms);
extern void     reboot_note_ack(uint32_t boot_id);
extern uint32_t reboot_note_consecutive(void);
```

- [ ] **Step 5: Register the test**

In `nodus/tuvm/tools/run_c_tests.sh`, after the `run test_cheap_stats …` line:

```bash
# rete#215: the retained panic note's validity rule. A block of garbage must not decode
# as a report — inventing a death is worse than missing one, because the gateway cannot
# falsify it. Retention itself is a TARGET property and is gated in tests/target/.
run test_reboot_note "$ENG_A" -DOEVM_HEAP_SIZE=65536
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cd nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -40
```
Expected: `test_reboot_note              PASS`, and no other line changes from `PASS` to `FAIL`.

- [ ] **Step 7: Run the full host gates**

```bash
cd nodus/nsl && python3 tools/run_tests_oevm.py > /tmp/nsl-t1.log 2>&1; echo "exit=$?"; tail -20 /tmp/nsl-t1.log
```
Expected: `exit=0`. **Read the aux gate line, not the pass count** — `footprint: SKIP (no
arm-zephyr-eabi toolchain)` is expected; anything else red is a real break.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/reboot_note.h nodus/tuvm/tests/test_reboot_note.c \
        nodus/tuvm/src/platform.c nodus/tuvm/tools/run_c_tests.sh
git commit -m "feat(#215): a retained note, and the rule that garbage is not a report"
```

---

## Task 2: The target side of the note, and the fatal handler that fills it

**Files:**
- Create: `nodus/tuvm/zephyr/src/reboot_note.c`
- Modify: `nodus/tuvm/zephyr/CMakeLists.txt`
- Modify: `nodus/tuvm/zephyr/src/main.c`

**Interfaces:**
- Consumes: `reboot_note.h`'s five functions and `RN_REASON_*` from Task 1.
- Produces: `k_sys_fatal_error_handler` override — the `#hardFault` / `#assert` /
  `#stackOverflow` producers. `#watchdog` arrives in Task 4.

- [ ] **Step 1: Write the target implementation**

Create `nodus/tuvm/zephyr/src/reboot_note.c`:

```c
/* rete#215 — the retained panic note on target, plus the fatal-error seam that fills it.
   2026-08-10 §5.1 chose `.rtc_noinit` over CONFIG_RETAINED_MEM and that choice is adopted
   verbatim; g_rtc in main.c:285 is the second in-tree user, so the section is already
   proven here.

   THE FATAL SEAM IS ONLY HALF THE PROBLEM, and it is the half that already worked. A
   wedge raises no fatal error, so this handler never runs for #192/#213 — that is the
   whole reason rete#215 exists. What lands here is the OTHER reasons: a CPU exception, a
   kernel oops, a stack-check failure. The wedge producer is observer.c.

   k_sys_fatal_error_handler is __weak in kernel/fatal.c:37 and tuvm overrides nothing
   today, so this replaces nothing. It MUST NOT RETURN: Zephyr's default halts, and
   returning from a fatal handler is undefined. */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/arch/riscv/csr.h>
#include <string.h>
#include "reboot_note.h"

#ifdef CONFIG_SOC_FAMILY_ESPRESSIF_ESP32
#include <esp_attr.h>   /* RTC_NOINIT_ATTR — .rtc_noinit is NOLOAD, so it survives a reset */
static RTC_NOINIT_ATTR struct RebootNote g_note;
#else
/* Every other board: plain .noinit. It does not survive a power cycle and on some parts
   does not survive a reset either — which costs those boards the report, not the node.
   The guard is on the STORAGE ATTRIBUTE, never on the definitions: platform.c externs
   these five symbols and they must link on every Zephyr board. */
static __noinit struct RebootNote g_note;
#endif

void reboot_note_init(void) {
    if (!reboot_note_is_valid(&g_note)) memset(&g_note, 0, sizeof g_note);
}

int reboot_note_read(struct RebootNote *out) {
    if (!reboot_note_is_valid(&g_note)) return 0;
    *out = g_note;
    return 1;
}

void reboot_note_write(uint32_t reason, uint32_t mcause,
                       uint32_t mepc, uint32_t ra, uint32_t mono_ms) {
    uint32_t prev = reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
    uint32_t boot = g_note.boot_id;    /* always live — init() made it a known value */
    g_note.magic = RN_MAGIC;
    g_note.reason = reason; g_note.mcause = mcause;
    g_note.mepc = mepc;     g_note.ra = ra;
    g_note.boot_id = boot;  g_note.mono_ms = mono_ms;
    g_note.consecutive = prev + 1;
    g_note.sum = reboot_note_checksum(&g_note);
}

void reboot_note_ack(uint32_t boot_id) {
    memset(&g_note, 0, sizeof g_note);
    g_note.boot_id = boot_id;
}

uint32_t reboot_note_consecutive(void) {
    return reboot_note_is_valid(&g_note) ? g_note.consecutive : 0;
}

/* Zephyr's K_ERR_* to Panic.reason. Anything unrecognised becomes #hardFault rather than
   a sixth symbol: RECORDS §2.4 pins the enumeration, and a reason the gateway cannot
   name is worth less than an imprecise one it can. */
static uint32_t rn_reason_for(unsigned int k_err) {
    switch (k_err) {
    case K_ERR_STACK_CHK_FAIL: return RN_REASON_STACK_OVERFLOW;
    case K_ERR_KERNEL_OOPS:
    case K_ERR_KERNEL_PANIC:   return RN_REASON_ASSERT;
    default:                   return RN_REASON_HARD_FAULT;
    }
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    /* mcause from the CSR, NOT from the esf: struct arch_esf carries mcause only under
       CONFIG_CLIC_SUPPORT_INTERRUPT_LEVEL (include/zephyr/arch/riscv/exception.h:81),
       which is unset on the C6. The CSR still holds the trap cause here — nothing has
       re-trapped between the fault and this handler. */
    uint32_t mcause = (uint32_t)csr_read(mcause);
    uint32_t mepc = esf ? (uint32_t)esf->mepc : 0u;
    uint32_t ra   = esf ? (uint32_t)esf->ra   : 0u;

    reboot_note_write(rn_reason_for(reason), mcause, mepc, ra,
                      (uint32_t)k_uptime_get_32());

    /* The dump goes to the console, which the collector already persists — so the crime
       scene survives the reboot in ~/.local/state/rete/console/ even though the node
       recovers. printk, not printf: the fault path gags newlib stdio. */
    printk("tuvm: FATAL reason=%u mcause=0x%08x mepc=0x%08x ra=0x%08x up=%ums consec=%u\n",
           reason, mcause, mepc, ra, (unsigned)k_uptime_get_32(),
           (unsigned)reboot_note_consecutive());

    /* §5.2's LOOP BREAKER IS NOT WIRED HERE YET, and the omission is the staging, not an
       oversight: the HALT arm is only safe because gw/liveness is watching (#217), and
       halting also requires disabling the hardware watchdog — which does not exist until
       observer.c arms it. Task 5 adds the arm to this function once both are true. Until
       then a repeating fault reboots forever, which is the behaviour every build before
       this one already had, and `consecutive` is counting the whole time. */
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}
```

- [ ] **Step 2: Add it to the build**

In `nodus/tuvm/zephyr/CMakeLists.txt`, inside the `target_sources(app PRIVATE …)` block, after
`src/log_udp.c`:

```cmake
  # rete#215: the retained panic note (.rtc_noinit) + the k_sys_fatal_error_handler
  # override. App-side, not an engine file: platform.c externs the five seam symbols the
  # way it already externs bell/ntp/rtc, so none of the nine engine source lists (#63)
  # changes.
  src/reboot_note.c
```

- [ ] **Step 3: Call `reboot_note_init()` at boot**

In `nodus/tuvm/zephyr/src/main.c`, immediately **after** the build-id banner
(`printk("tuvm: build-id %s\n", tuvm_build_id);`) and **before** `log_udp_start()`:

```c
    /* rete#215: validate the retained note before anything can read it, and before the
       remote console starts — a note that was going to be reported must not be lost to a
       crash during mesh bring-up, and a note that is GARBAGE must not survive to be
       reported as a death that never happened. After the build-id line so the boot that
       reports a note is attributable to the binary that reports it. */
    reboot_note_init();
    if (reboot_note_consecutive() > 0) {
        struct RebootNote rn;
        if (reboot_note_read(&rn)) {
            printk("tuvm: reboot-note reason=%u mcause=0x%08x mepc=0x%08x ra=0x%08x "
                   "boot=%u up=%ums consec=%u\n",
                   (unsigned)rn.reason, (unsigned)rn.mcause, (unsigned)rn.mepc,
                   (unsigned)rn.ra, (unsigned)rn.boot_id, (unsigned)rn.mono_ms,
                   (unsigned)rn.consecutive);
        }
    }
```

Add `#include "reboot_note.h"` beside main.c's other tuvm includes (`#include "oevm.h"`).

- [ ] **Step 4: Build for the C6 and verify it links**

```bash
cd nodus/tuvm/zephyr && bash run.sh --board esp32c6 --build-only 2>&1 | tail -20
```
Expected: a clean build. If `run.sh` has no `--build-only`, use the west invocation it prints —
**do not flash in this step**; linking is the assertion here.

- [ ] **Step 5: Prove the fatal arm on hardware, using the build that already exists for it**

`prj_esp32c6_faultgate.conf` exists for exactly this (#56 D8) and faults on purpose at boot.

```bash
cd nodus/tuvm/zephyr && ./run.sh --board c6spare --conf-extra prj_esp32c6_faultgate.conf --flash
```
Expected, across **two** boots: boot 1 prints `tuvm: FATAL reason=… consec=1` and reboots;
boot 2 prints `tuvm: reboot-note reason=2 … consec=1` before the VM starts, then faults again
with `consec=2`. It keeps rebooting — the HALT arm is Task 5's, and Task 12 gates it. **Stop the
loop by reflashing the default build** when you have the two lines you came for.

**This is the falsification, and it fails by reading zero.** If boot 2 prints no
`reboot-note` line, retention is not working and NOTHING later in this plan can be trusted —
stop and report rather than continuing.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/zephyr/src/reboot_note.c nodus/tuvm/zephyr/CMakeLists.txt \
        nodus/tuvm/zephyr/src/main.c
git commit -m "feat(#215): a fatal error now leaves a note that survives the reboot"
```

---

## Task 3: The timer table learns what the app declared

Design §3.1: the deadline comes from the app's own timers, so the registry has to remember what
was declared and when it last fired. `timer_backend.h` says the C side carries **data only** —
so this task adds two fields and one query, and no policy. `SLACK` lives in the observer.

**Files:**
- Modify: `nodus/tuvm/src/backend/zephyr/timer_zephyr.c:29-35` (struct), `:51-56`
  (`timer_expiry`), `:58-73` (`timer_start`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  /* Fills up to `max` entries describing the live REPEATING timers, and answers how many
     were written. period_ms[i] is what the app declared; since_ms[i] is ms since that
     timer last fired (or since it was armed, if it never has). */
  int timer_target_snapshot(uint32_t *period_ms, uint32_t *since_ms, int max);
  ```
  `OEVM_MAX_TARGET_TIMERS` is 8, so a caller sizes its arrays at 8.

- [ ] **Step 1: Widen the struct**

```c
struct TargetTimer {
    struct k_timer t;   /* first member: timer_expiry recovers the slot via CONTAINER_OF */
    int token;
    int chan_id;
    int repeating;
    int live;
    /* rete#215: a repeating timer's period is the app's own statement of how often it
       expects to be woken, and it is the ONLY deadline the wedge detector is allowed to
       judge this node against (design §2 — a battery sensor waking every ten minutes is
       the ordinary case, not a hypothetical). Both are 32-bit stores made from ISR
       context, which is the discipline `live = 0` already relies on. */
    uint32_t period_ms;
    uint32_t last_fire_ms;   /* k_uptime_get_32(); see the wrap note on the query below */
};
```

- [ ] **Step 2: Stamp it in the ISR and at registration**

In `timer_expiry`, after the `if (!e->live) return;` guard and **before** the post:

```c
    e->last_fire_ms = k_uptime_get_32();
```

In `timer_start`, inside the `if (!e->live)` block, beside the other field stores:

```c
            e->period_ms    = (uint32_t)ms;
            e->last_fire_ms = k_uptime_get_32();   /* the deadline runs from ARMING, not
                                                      from the first fire — else a timer
                                                      that never fires at all is judged
                                                      against a zero epoch and trips at
                                                      once */
```

- [ ] **Step 3: Add the query**

At the end of `timer_zephyr.c`:

```c
/* rete#215: what the app declared, for the wedge detector (zephyr/src/observer.c). Data
   only — no SLACK, no verdict, no policy. That division is timer_backend.h's own: "The C
   side carries DATA ONLY (a token, a millisecond count)."

   ONE-SHOTS ARE EXCLUDED. A repeating period is a statement about cadence; a one-shot is
   a statement about a single future moment, and a node with only one-shots has declared
   nothing to be judged against.

   THE SUBTRACTION HAPPENS IN THE UNIT IT WRAPS IN. k_uptime_get_32() wraps at ~49.7 days;
   `now - last` in uint32 is correct across that wrap for any interval shorter than the
   period itself, which is why this function — and not its caller — owns the subtraction.
   span_backend.h carries the same lesson for the same reason (tuvm#61). */
int timer_target_snapshot(uint32_t *period_ms, uint32_t *since_ms, int max) {
    uint32_t now = k_uptime_get_32();
    int n = 0;
    for (int i = 0; i < OEVM_MAX_TARGET_TIMERS && n < max; i++) {
        struct TargetTimer *e = &target_timers[i];
        if (!e->live || !e->repeating) continue;
        period_ms[n] = e->period_ms;
        since_ms[n]  = now - e->last_fire_ms;
        n++;
    }
    return n;
}
```

- [ ] **Step 4: Verify it builds and the host suite is untouched**

```bash
cd nodus/tuvm/zephyr && bash run.sh --board esp32c6 --build-only 2>&1 | tail -5
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py > /tmp/nsl-t3.log 2>&1; echo "exit=$?"; tail -8 /tmp/nsl-t3.log
```
Expected: the C6 build links; `exit=0` on the nsl suite. `timer_posix.c` is untouched by design —
the host has no mailbox and no ISR, which is the whole reason `tests/target/` exists.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/backend/zephyr/timer_zephyr.c
git commit -m "feat(#215): a repeating timer now remembers what the app declared"
```

---

## Task 4: The mailbox learns to say how much it is holding

Design §3.2's condition is *"(posted − consumed) > 0 continuously"*. `k_msgq` already knows that
number; it is just not reachable — `pulse_q` is static inside `pulse_zephyr.c`. One accessor,
both backends, no new file.

**Files:**
- Modify: `nodus/tuvm/src/pulse_backend.h`
- Modify: `nodus/tuvm/src/backend/zephyr/pulse_zephyr.c`
- Modify: `nodus/tuvm/src/backend/posix/pulse_posix.c`

**Interfaces:**
- Produces: `uint32_t pulse_pending(void);` — items queued and not yet drained, right now.

- [ ] **Step 1: Declare it in the shared seam**

In `nodus/tuvm/src/pulse_backend.h`, after `uint32_t pulse_stale(void);`:

```c
/* rete#215: how many posts are queued and not yet drained, RIGHT NOW. Not a counter —
   an instantaneous depth, and the only value in this seam that can go down. The wedge
   detector's second condition is this reading staying above zero for longer than the
   app's own shortest declared timer period allows (design §3.2): the ISR is firing and
   the mailbox is filling, but the VM is not draining it. Distinct from pulse_drops(),
   which counts what a FULL mailbox threw away — a node can be wedged with a mailbox one
   deep and never shed anything. */
uint32_t pulse_pending(void);
```

- [ ] **Step 2: Implement it on both backends**

`nodus/tuvm/src/backend/zephyr/pulse_zephyr.c`, beside the other accessors at `:86-88`:

```c
uint32_t pulse_pending(void)   { return (uint32_t)k_msgq_num_used_get(&pulse_q); }
```

`nodus/tuvm/src/backend/posix/pulse_posix.c` — read the file's existing ring bookkeeping and
answer the same question from it (the ring's count field, however it is spelled there). It exists
so the seam LINKS on the host and so a host caller gets an honest answer; the host has no ISR, so
it can never be non-zero at a safe point.

- [ ] **Step 3: Verify both link**

```bash
cd nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -30
cd zephyr && bash run.sh --board esp32c6 --build-only 2>&1 | tail -5
```
Expected: every C test still `PASS`; the C6 build links.

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/pulse_backend.h nodus/tuvm/src/backend/zephyr/pulse_zephyr.c \
        nodus/tuvm/src/backend/posix/pulse_posix.c
git commit -m "feat(#215): the pulse mailbox can now say how much it is holding"
```

---

## Task 5: The observer — the detector, the dump, and the policy line

This is the module design §6 asks for. It owns the two conditions, the verdict, the dump, and the
reboot/HALT decision. Read plan decisions **D-P1**, **D-P2** and **D-P5** before writing a line of
it — all three are load-bearing here.

**Files:**
- Create: `nodus/tuvm/zephyr/src/observer.c`
- Modify: `nodus/tuvm/zephyr/CMakeLists.txt`
- Modify: `nodus/tuvm/zephyr/src/main.c`
- Modify: `nodus/tuvm/zephyr/prj_esp32c6.conf`
- Modify: `nodus/tuvm/zephyr/Kconfig`
- Create: `nodus/tuvm/zephyr/prj_esp32c6_wedgegate.conf`

**Interfaces:**
- Consumes: `timer_target_snapshot()` (Task 3), `pulse_pending()` / `pulse_drops()` /
  `pulse_coalesced()` / `pulse_stale()` (Task 4 + `pulse_backend.h`), the note seam (Task 1).
- Produces: `void observer_start(void);` and `void observer_halt(void);` — the latter already
  wired into `reboot_note.c`'s fatal handler in Step 2 below — Task 2 deliberately left the
  loop breaker out, because halting requires the hardware watchdog this file arms.

- [ ] **Step 1: Write the observer**

Create `nodus/tuvm/zephyr/src/observer.c`:

```c
/* rete#215 — the detector that converts "no fault" into a fault worth reporting.

   WHY THIS EXISTS. Two changes on the check-in path — CHeapStats fromList: (#192) and one
   line of valueFits: (#213) — have each killed a C6 within six check-ins. Both passed
   every host gate. Both die the same way: no panic, no coredump, no VM Error, no reboot
   banner, console port=up, obs/health frozen. Neither mechanism is understood and this
   file does not try to explain them. It closes the gap that makes them so expensive to
   chase: the node has no way to say that it stopped.

   WHY THE OBVIOUS DETECTOR IS WRONG. pumpEvents blocks on k_msgq_get(K_FOREVER)
   (pulse_zephyr.c:94). That single call is BOTH the healthy idle state — the node has
   nothing to do and is sleeping efficiently, which is the point of the design — AND the
   wedged state. "Has the VM pumped recently?" cannot separate them. It works today only
   because the bridge happens to hold a 2 s timer, and it would falsely reboot the first
   node whose app legitimately sleeps for ten minutes. So THE DETECTOR CARRIES NO IDEA OF
   HOW OFTEN A NODE SHOULD WAKE. It asks the app, through timer_target_snapshot().

   WHY A THREAD, AND WHY task_wdt SITS BEHIND IT RATHER THAN IN FRONT. task_wdt_feed()
   calls k_sched_lock() (task_wdt.c:240), which asserts !arch_is_in_isr()
   (kernel/sched.c:774) — and CONFIG_ASSERT is OFF in this build, so an ISR-context feed
   would silently decrement the interrupted thread's sched_locked. The feed must therefore
   happen in thread context, and the only thread that could otherwise do it is the reactor,
   which is the thing that wedges. Given a thread, the verdict is better taken in it:
   printk and sys_reboot from thread context are the ordinary paths. That leaves the
   task_wdt channel free to be the backstop for THIS THREAD dying — a genuinely different
   failure — with the C6 hardware WDT behind that for the kernel itself dying.

   COOPERATIVE PRIORITY IS LOAD-BEARING. At a preemptible priority this thread would never
   be scheduled while the VM spins in the interpreter, so the trip would come from a missed
   feed against a FIXED channel period — which is exactly the global "how often should a
   node wake" constant the design forbids. Cooperative priority keeps the app's own
   declared periods the operative deadline. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include "reboot_note.h"
#include "pulse_backend.h"

/* Declared across translation units the way platform.c declares bell/ntp/rtc — the tree's
   idiom for a target leaf that lives in another file. Defined in
   src/backend/zephyr/timer_zephyr.c. */
int timer_target_snapshot(uint32_t *period_ms, uint32_t *since_ms, int max);

#define OBS_MAX_TIMERS 8    /* == OEVM_MAX_TARGET_TIMERS */

/* The ONE invented constant of the design (§3.3), shipped at 8. Deliberately generous:
   the cost of a false reboot is far higher than the cost of a slow detection, and the
   measured worst-case pushBuild is 132 ms against a 2 s period. It is falsifiable BY
   CONSTRUCTION — a wrong SLACK produces a reboot whose dump says exactly how overdue it
   thought the node was, and a constant that announces its own error when wrong is worth
   more than one argued to be right in advance. */
#define WEDGE_SLACK 8u

/* A sampling resolution, not a deadline (plan D-P2). It must sit far below the shortest
   deadline in play; the shortest this bench can produce is the bridge's 8 x 2000 ms, which
   this samples 16 times. */
#define OBSERVER_TICK_MS 1000

/* The backstop for THIS THREAD dying, and nothing else. Deliberately 30x the tick so it
   can never become the operative deadline — if this number ever decides anything, the
   dump will say so, because the wedge path prints a condition and this path prints none. */
#define OBSERVER_WDT_MS 30000

enum { COND_NONE = 0, COND_TIMER_OVERDUE, COND_PULSE_UNDRAINED };

static const struct device *g_hw_wdt;
static int g_wdt_channel = -1;

/* Kept so the dump can name the margin rather than merely the condition — "which one
   fired, and by how much" is the first fact anyone chasing the next wedge will want. */
static uint32_t g_margin_ms;
static uint32_t g_deadline_ms;

/* Answers COND_* and fills g_margin_ms / g_deadline_ms. Both conditions derive their
   deadline from state the app already declared; nothing global is assumed.

   They are kept as TWO NAMED CONDITIONS rather than one merged predicate because the dump
   reports which one fired. Neither alone covers both wedge shapes. */
static int observer_verdict(void) {
    uint32_t period[OBS_MAX_TIMERS], since[OBS_MAX_TIMERS];
    int n = timer_target_snapshot(period, since, OBS_MAX_TIMERS);
    if (n == 0) return COND_NONE;   /* the app declared no cadence: nothing to judge it
                                       against, and guessing one is what §2 forbids */

    /* §3.1 — an armed repeating timer is overdue against ITS OWN period. Catches the
       timer subsystem itself stopping: the ISR no longer firing at all. A node with a
       ten-minute timer is judged against ten minutes; one with a 2 s timer against 2 s. */
    uint32_t shortest = period[0];
    for (int i = 0; i < n; i++) {
        uint32_t deadline = period[i] * WEDGE_SLACK;
        if (period[i] < shortest) shortest = period[i];
        if (since[i] > deadline) {
            g_deadline_ms = deadline;
            g_margin_ms   = since[i] - deadline;
            return COND_TIMER_OVERDUE;
        }
    }

    /* §3.2 — the ISR is firing and the mailbox is filling, but the VM is not draining it:
       a VM stuck in the interpreter rather than blocked at the mailbox. Judged against the
       SHORTEST declared period, because that is the fastest cadence the app claims to
       service. */
    {
        static uint32_t undrained_since_ms;   /* 0 == not currently undrained */
        uint32_t deadline = shortest * WEDGE_SLACK;
        uint32_t now = k_uptime_get_32();
        if (pulse_pending() == 0) {
            undrained_since_ms = 0;
        } else {
            if (undrained_since_ms == 0) undrained_since_ms = now ? now : 1u;
            uint32_t held = now - undrained_since_ms;
            if (held > deadline) {
                g_deadline_ms = deadline;
                g_margin_ms   = held - deadline;
                return COND_PULSE_UNDRAINED;
            }
        }
    }
    return COND_NONE;
}

/* The crime scene. It goes to the console, which the collector already persists — so the
   evidence survives the reboot in ~/.local/state/rete/console/ AND the node still
   recovers. That was the deciding property: the field-correct behaviour is also the one
   that keeps the evidence.

   The counters come from pulse_backend.h directly — the same single source
   PRIM_pf_sysStats reads (platform.c:1439-1445), so the console dump and sys/metrics can
   never disagree about pdr/pco/pst. */
static void observer_dump(int cond) {
    printk("tuvm: WEDGE cond=%s overdue=%ums deadline=%ums up=%ums "
           "pending=%u pdr=%u pco=%u pst=%u consec=%u\n",
           cond == COND_TIMER_OVERDUE ? "timer-overdue" : "pulse-undrained",
           (unsigned)g_margin_ms, (unsigned)g_deadline_ms, (unsigned)k_uptime_get_32(),
           (unsigned)pulse_pending(), (unsigned)pulse_drops(),
           (unsigned)pulse_coalesced(), (unsigned)pulse_stale(),
           (unsigned)reboot_note_consecutive() + 1u);
}

/* §5.2's HALT arm. THE HARDWARE WATCHDOG MUST BE DISABLED FIRST, and this is not in either
   design document: task_wdt_init() installs a hardware timeout with WDT_FLAG_RESET_SOC
   (task_wdt.c:135-138). A halted node feeds nothing, so the SoC would reset ~11 s later and
   the loop breaker's "clean, findable failure" would silently become the reboot loop it
   exists to stop — with no console line saying why.

   Safe to call from the fatal handler in ISR context: wdt_disable and printk are, and
   nothing after the busy-wait needs a scheduler. */
void observer_halt(void) {
    if (g_hw_wdt) (void)wdt_disable(g_hw_wdt);
    printk("tuvm: HALT — 3 consecutive faults with no check-in between (2026-08-10 §5.2). "
           "The note is kept. gw/liveness is watching (#217).\n");
    k_busy_wait(50000);   /* let the UART FIFO drain before interrupts go away */
    unsigned int key = irq_lock();
    (void)key;
    for (;;) { /* a clean, findable failure — not a noisy zombie */ }
}

static void observer_thread_fn(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    g_wdt_channel = task_wdt_add(OBSERVER_WDT_MS, NULL, NULL);
    /* callback NULL == sys_reboot(SYS_REBOOT_COLD) from task_wdt itself
       (task_wdt.c:121-125). That is the right handling for "this thread stopped": there is
       nobody left to write a note, and a reboot with no note is still better than a node
       that is silently gone. The wedge path below never relies on it. */

    for (;;) {
        int cond = observer_verdict();
        if (cond == COND_NONE) {
            if (g_wdt_channel >= 0) (void)task_wdt_feed(g_wdt_channel);
        } else {
            observer_dump(cond);
            reboot_note_write(RN_REASON_WATCHDOG, 0, 0, 0, k_uptime_get_32());
            if (reboot_note_consecutive() >= 3) observer_halt();   /* does not return */
            sys_reboot(SYS_REBOOT_COLD);                           /* does not return */
        }
        k_sleep(K_MSEC(OBSERVER_TICK_MS));
    }
}

/* K_PRIO_COOP(7): see the header comment. It must run when it wakes regardless of what the
   reactor is doing, and its whole body is a handful of comparisons, so it yields promptly
   and starves nothing. */
K_THREAD_STACK_DEFINE(observer_stack, 1024);
static struct k_thread observer_thread;

void observer_start(void) {
    g_hw_wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
    if (g_hw_wdt && !device_is_ready(g_hw_wdt)) g_hw_wdt = NULL;
    if (task_wdt_init(g_hw_wdt) != 0) {
        /* Loud, and NOT fatal: a node with no watchdog is a node with no wedge detector,
           which is the state every build was in before this one — but it must not be the
           state a build is in SILENTLY. */
        printk("tuvm: observer — task_wdt_init failed, wedge detection is OFF\n");
        return;
    }
    printk("tuvm: observer armed (slack=%u tick=%ums backstop=%ums hw=%s)\n",
           (unsigned)WEDGE_SLACK, (unsigned)OBSERVER_TICK_MS, (unsigned)OBSERVER_WDT_MS,
           g_hw_wdt ? "yes" : "no");
    k_thread_create(&observer_thread, observer_stack,
                    K_THREAD_STACK_SIZEOF(observer_stack),
                    observer_thread_fn, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_name_set(&observer_thread, "tuvm_observer");
}
```

- [ ] **Step 2: Wire the HALT arm into the fatal handler, and add the file to the build**

Now that `observer_halt()` exists, add §5.2's loop breaker to `k_sys_fatal_error_handler` in
`nodus/tuvm/zephyr/src/reboot_note.c`, immediately before its `sys_reboot`, replacing the
staging comment left there in Task 2:

```c
    /* §5.2's loop breaker. Safe to ship only because gw/liveness is watching (built and
       verified 2026-08-18, #217) — without it this would trade a noisy zombie for an
       invisible corpse. observer_halt() disables the hardware watchdog first; see the
       comment there for why that is not optional. */
    if (reboot_note_consecutive() >= 3) {
        printk("tuvm: 3 consecutive faults with no check-in between — HALT\n");
        observer_halt();   /* does not return */
    }
```

with, at the top of that file:

```c
/* Defined in observer.c. Declared across translation units rather than included, the way
   platform.c declares bell/ntp/rtc — the tree's idiom for a target leaf in another file. */
void observer_halt(void);
```

In `nodus/tuvm/zephyr/CMakeLists.txt`, after `src/reboot_note.c`:

```cmake
  # rete#215: the wedge detector. App-side beside reboot_note.c for the same reason — it
  # is reached only from main.c, and placing it in src/backend/zephyr/ would cost a posix
  # stub in nine source lists (#63) that nothing on the host would call.
  src/observer.c
```

- [ ] **Step 3: Enable the watchdog subsystem for the C6**

Append to `nodus/tuvm/zephyr/prj_esp32c6.conf`:

```
# --- rete#215: the wedge detector's timeout path -----------------------------
# wdt0 is `okay` on esp32c6_devkitc_hpcore.dts:72 and CONFIG_WDT_ESP32 defaults y
# once CONFIG_WATCHDOG is on, so this is the whole of it. HAS_WDT_DISABLE_AT_BOOT
# means the driver hands us a watchdog that is off until observer_start() sets it up.
CONFIG_WATCHDOG=y
CONFIG_TASK_WDT=y
CONFIG_TASK_WDT_CHANNELS=2
# The HARDWARE window is MIN_TIMEOUT + HW_FALLBACK_DELAY (task_wdt.c:137-138) = 11 s.
# Deliberately far above the observer's own 30 s backstop being reachable in normal
# operation: the hw watchdog is the LAST resort, for the kernel itself dying, and it
# resets with no dump at all. It is fed on every schedule_next_timeout(), which our 1 s
# feed triggers, so this costs no extra wakeups.
CONFIG_TASK_WDT_MIN_TIMEOUT=10000
CONFIG_TASK_WDT_HW_FALLBACK_DELAY=1000
```

- [ ] **Step 4: Start it at boot**

In `nodus/tuvm/zephyr/src/main.c`, immediately **before** the model-specific VM start
(`init_solidified_vtables();` / `kernel_boot();`), after the radio bring-up block:

```c
    /* rete#215: arm the wedge detector before the VM runs, and AFTER the radio — a node
       that wedges during OpenThread bring-up has not yet declared any timer, so the
       detector answers COND_NONE and cannot trip on it. Arming earlier would buy nothing
       and would put the first reboot inside a path that has no note to write about it. */
    observer_start();
```

with, beside the other externs at the top of `main.c`:

```c
void observer_start(void);   /* zephyr/src/observer.c (rete#215) */
```

- [ ] **Step 5: Add the test trigger, in the shape `TUVM_FAULT_TRIGGER` already set**

Append to `nodus/tuvm/zephyr/Kconfig`, before `source "Kconfig.zephyr"`:

```
# rete#215 §8 row 1. Default n so it is not merely unreachable in a normal build but
# ABSENT from it — the same shape as TUVM_FAULT_TRIGGER above, and turned on only by
# prj_esp32c6_wedgegate.conf. It exists so "the note survives a watchdog reset" is proven
# by a scripted gate rather than by the next real wedge.
#
# This is NOT the Kconfig bench/field split design §9 rules out: the DETECTOR ships
# always-on and unchanged. What this gates is a synthetic wedge, and a gate needs one
# because a real wedge cannot be summoned on demand.
config TUVM_WEDGE_TRIGGER
	bool "Wedge on purpose after TUVM_WEDGE_TRIGGER_S seconds (rete#215)"
	default n
	help
	  Stops the reactor draining the pulse mailbox, which the observer must then
	  detect as COND_PULSE_UNDRAINED. If this is set in a build you did not
	  deliberately configure, that build is wrong.

config TUVM_WEDGE_TRIGGER_S
	int "Seconds after boot before the synthetic wedge"
	default 15
	depends on TUVM_WEDGE_TRIGGER
```

Create `nodus/tuvm/zephyr/prj_esp32c6_wedgegate.conf`:

```
# The one C6 build that wedges on purpose (rete#215 §8 row 1). Never the default build.
# Layered with run.sh --conf-extra, exactly as prj_esp32c6_faultgate.conf is.
#
# 15 s, and the number is load-bearing: it MUST fire before WedgeProbeC6's own 30 s wedge,
# or the wedge-note scenario silently tests the DETECTOR (which is wedge-detect's job)
# instead of retention across a reset — and it would still pass, which is the worst kind
# of wrong.
CONFIG_TUVM_WEDGE_TRIGGER=y
CONFIG_TUVM_WEDGE_TRIGGER_S=15
```

Implement the trigger in `observer.c`. It needs a file-scope flag beside `g_hw_wdt`:

```c
#ifdef CONFIG_TUVM_WEDGE_TRIGGER
/* Latched at observer_start(), never recomputed. A trigger that re-armed on the boot that
   REPORTS the previous wedge would reboot the node again mid-report, and the gate would
   see the second reboot as a death inside its observation window — a red that says
   nothing about the detector. Latching on "was a note already pending when this boot
   started?" makes the synthetic wedge exactly one reboot per gate run, regardless of when
   the app acks. */
static bool g_trigger_armed;
#endif
```

set in `observer_start()`, immediately before the `armed` printk:

```c
#ifdef CONFIG_TUVM_WEDGE_TRIGGER
    g_trigger_armed = (reboot_note_consecutive() == 0);
    printk("tuvm: observer — SYNTHETIC WEDGE TRIGGER is compiled in, %s\n",
           g_trigger_armed ? "armed" : "disarmed (a note is already pending)");
#endif
```

and read at the top of `observer_thread_fn`'s loop body:

```c
#ifdef CONFIG_TUVM_WEDGE_TRIGGER
        /* The synthetic wedge: after the configured delay, stop answering COND_NONE. It
           forces the VERDICT rather than faking the mailbox — faking the mailbox would
           test the trigger and not the path the verdict takes. */
        if (g_trigger_armed &&
            k_uptime_get_32() > (uint32_t)CONFIG_TUVM_WEDGE_TRIGGER_S * 1000u) {
            g_trigger_armed = false;
            g_deadline_ms = 0; g_margin_ms = 0;
            observer_dump(COND_PULSE_UNDRAINED);
            reboot_note_write(RN_REASON_WATCHDOG, 0, 0, 0, k_uptime_get_32());
            if (reboot_note_consecutive() >= 3) observer_halt();
            sys_reboot(SYS_REBOOT_COLD);
        }
#endif
```

- [ ] **Step 6: Verify the default build is unchanged in the way that matters**

```bash
cd nodus/tuvm/zephyr && bash run.sh --board esp32c6 --build-only 2>&1 | tail -5
grep -c TUVM_WEDGE_TRIGGER build-esp32c6-B/zephyr/.config
grep -E "CONFIG_(TASK_WDT|WATCHDOG|WDT_ESP32)=" build-esp32c6-B/zephyr/.config
```
Expected: the build links; `TUVM_WEDGE_TRIGGER` appears only as `# … is not set`;
`CONFIG_WATCHDOG=y`, `CONFIG_TASK_WDT=y`, `CONFIG_WDT_ESP32=y`.

- [ ] **Step 7: Run the host gates**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py > /tmp/nsl-t5.log 2>&1; echo "exit=$?"; tail -8 /tmp/nsl-t5.log
cd /home/david/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -30
```
Expected: `exit=0`, every C test `PASS`. **Neither gate can see this file** — that is the point
of `tests/target/`, and the tasks below are where it is actually tested.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/zephyr/src/observer.c nodus/tuvm/zephyr/CMakeLists.txt \
        nodus/tuvm/zephyr/src/main.c nodus/tuvm/zephyr/src/reboot_note.c \
        nodus/tuvm/zephyr/prj_esp32c6.conf nodus/tuvm/zephyr/Kconfig \
        nodus/tuvm/zephyr/prj_esp32c6_wedgegate.conf
git commit -m "feat(#215): a node that stops draining now says so before it reboots"
```

---

## Task 6: The prims — reading the note, and acking it

Two prims, not three (plan **D-P6**). The register triple rides as a **preformatted String**,
not as numbers: `mcause` is routinely `0xdeadbeef`-shaped and **an SMI holds only 2^30−1 on a
32-bit target** — `rtc_target_put`'s own comment records that lesson. RECORDS §2.4 already says
`detail` is text, so formatting in C puts the truncation exactly where the protocol sanctions it.

**Files:**
- Modify: `nodus/tuvm/src/prim_ids.h`, `nodus/tuvm/src/dispatch.c`, `nodus/tuvm/src/platform.c`

**Interfaces:**
- Produces:
  - `platform panicNote` → a 4-slot Array `[reason(SMI), bootId(SMI), monoMs(SMI),
    detail(String)]`, or **nil** when no report is pending.
  - `platform panicNoteAck: bootId` → answers `platform`; clears the report and
    `consecutive`, deposits `bootId`.

- [ ] **Step 1: COUNT the next ids — do not compute them**

```bash
cd nodus/tuvm && awk '/^static const char \*const PRIM_NAMES/,/^};/' src/dispatch.c | grep -c '^\s*"'
grep -n "PRIM_pf_cheapStats" src/prim_ids.h src/dispatch.c
```
Expected: the count equals the current value of `PRIM_pf_cheapStats` (152 at the time of
writing). **The two new ids are that count + 1 and + 2 — verified against the count you just
ran, never against this document.** The parity typedef at `dispatch.c:944` will fail the build if
you are wrong, which is the point of it.

- [ ] **Step 2: Add the ids**

At the end of `nodus/tuvm/src/prim_ids.h`:

```c
/* rete#215: the retained panic note. Appended after cheapStats — no pre-existing id
   shifts. 153/154 were arrived at by COUNTING PRIM_NAMES (152 entries before this
   append), not by 152+1 arithmetic, per the standing lesson above.
     panicNote        -> 4-slot Array [reason, bootId, monoMs, detail] or nil. The
                         register triple is FORMATTED TEXT in slot 4, not numbers: mcause
                         does not fit an SMI on a 32-bit target, and RECORDS §2.4 already
                         pins Panic.detail as text and names it the one place truncation
                         is legal.
     panicNoteAck: id -> clears the report AND the loop-breaker count, and deposits this
                         boot's id for the next death to carry. One call because §5.1's
                         report-once clear and §5.2's "reset on a successful check-in" are
                         the same moment. */
#define PRIM_pf_panicNote    153
#define PRIM_pf_panicNoteAck 154
```

- [ ] **Step 3: Add the enum entries, the names, and update the parity check**

In `nodus/tuvm/src/dispatch.c`, at the end of the PRIM enum (after `PRIM_pf_cheapStats,`):

```c
    /* rete#215: the retained panic note — the 153rd/154th PRIM_NAMES entries (counted,
       not computed). Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_panicNote,
    PRIM_pf_panicNoteAck,
```

At the end of `PRIM_NAMES` (after `"cheapStats",`):

```c
    "panicNote",
    "panicNoteAck:",
```

And update the parity typedef at `:944` to name the new last entry:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_panicNoteAck) ? 1 : -1];
```

- [ ] **Step 4: Implement the cases**

In `nodus/tuvm/src/platform.c`, after the `PRIM_pf_cheapStats` case:

```c
    case PRIM_pf_panicNote: {
        /* rete#215 — `platform panicNote`: the pending death, or nil.
           GC-safety follows the spaceChangedSince precedent (:972-988) rather than the
           sysStats one, because this prim allocates TWICE: the String must be built while
           the Array is rooted, or a GC inside space_make_string_pub moves an Array that
           only a C local names. */
        struct RebootNote rn;
        if (!reboot_note_read(&rn)) { *out = NULL; return 1; }   /* no report -> nil */

        char detail[96];
        int dn = snprintf(detail, sizeof detail,
                          "mcause=0x%08x mepc=0x%08x ra=0x%08x consec=%u",
                          (unsigned)rn.mcause, (unsigned)rn.mepc,
                          (unsigned)rn.ra, (unsigned)rn.consecutive);
        if (dn < 0) dn = 0;
        if (dn > (int)sizeof detail - 1) dn = (int)sizeof detail - 1;  /* truncation is
                                                                          legal HERE and
                                                                          nowhere else */
        extern void gc_temp_root_push(oop o);
        extern int gc_temp_roots_count;
        int saved = gc_temp_roots_count;

        oop r = indexable_new(4);
        gc_temp_root_push(r);
        ((struct ObjectInstance *)r)->slots[1] = TAG_SMI((intptr_t)rn.reason);
        ((struct ObjectInstance *)r)->slots[2] = TAG_SMI((intptr_t)rn.boot_id);
        ((struct ObjectInstance *)r)->slots[3] = TAG_SMI((intptr_t)rn.mono_ms);
        ((struct ObjectInstance *)r)->slots[4] = space_make_string_pub(detail, (size_t)dn);
        gc_temp_roots_count = saved;
        *out = r;
        return 1;
    }
    case PRIM_pf_panicNoteAck: {
        /* `platform panicNoteAck: bootId` — the app's "I got far enough to report" signal.
           Answers platform, the same shape timerAfter:/timerEvery: use for a prim whose
           result is the act and not a value. */
        reboot_note_ack((uint32_t)UNTAG_SMI(args[0]));
        *out = platform_object();
        return 1;
    }
```

- [ ] **Step 5: Verify the parity check catches a mistake, then that it passes**

```bash
cd nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -30
```
Expected: every test `PASS`. If the parity typedef fails to compile with a negative array size,
the enum and `PRIM_NAMES` have gone out of step — fix the counting, do not adjust the typedef.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/src/prim_ids.h nodus/tuvm/src/dispatch.c nodus/tuvm/src/platform.c
git commit -m "feat(#215): nsl can read the note the dying node left"
```

---

## Task 7: The report — `Panic` on `sys/panic`, and the ack that closes the loop

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl`
- Modify: `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl`

**Interfaces:**
- Consumes: `PRIM_pf_panicNote` / `PRIM_pf_panicNoteAck` from Task 6.
- Produces on `Kernel`:
  - `panicNote` / `panicNoteAck:` — thin forwarders, the `sysStats` shape.
  - `panic` — answers the `Panic` Record, or `nil` when no report is pending.

- [ ] **Step 1: Add the forwarders**

In `nodus/nsl/lib/kernel.nsl`, immediately after `public sysStats = ( ^ platform sysStats )`:

```
  (* rete#215: the retained panic note. Thin forwarders, the sysStats shape above.
     A wedge raises no fatal error, so nothing in this tree could ever produce
     Panic reason #watchdog before now — RECORDS §2.4 pinned the symbol and the
     record, and both were aspirational. *)
  public panicNote = ( ^ platform panicNote )
  public panicNoteAck: bootId = ( ^ platform panicNoteAck: bootId )
```

- [ ] **Step 2: Add the record builder beside `metrics` and `cheap`**

In `nodus/nsl/lib/kernel.nsl`, after `public cheap = ( … )`:

```
  (* rete#215: the sys/panic report — prim 153 as the Panic record RECORDS §2.4 pins.
     The kernel only BUILDS it; publishing onto a channel is the app's act with the
     channel arriving as a capability, exactly as metrics and cheap are handled —
     the kernel never conjures space access.

     READ WITH basicAt:, NOT through a wrapper class. `CHeapStats fromList:` on the
     check-in path silently HANGS a C6 (rete#192, mechanism still unknown), and this
     record is built on a path that runs at boot on every node. One fewer mechanism,
     and the one that is known to be survivable.

     `core` is nil and stays nil: blobs are off the space (RECORDS §2.4), and a node
     that just died is the last node that should be asked to retain one. *)
  public panic = ( | n reason pairs |
    n:: platform panicNote.
    n isNil ifTrue: [ ^ nil ].
    reason:: self panicReasonFor: (n basicAt: 1).
    pairs:: OrderedCollection new.
    pairs add: #reason -> reason.
    pairs add: #bootId -> (n basicAt: 2).
    pairs add: #monoMs -> (n basicAt: 3).
    pairs add: #detail -> (n basicAt: 4).
    pairs add: #core   -> nil.
    ^ Record tag: #Panic fields: (Map fromPairs: pairs) )

  (* The C side writes a small int because a fatal handler may not allocate; the symbol
     belongs here, where the record is built. The mapping is RN_REASON_* in
     nodus/tuvm/src/reboot_note.h and it is APPEND-ONLY. An unrecognised code answers
     #hardFault rather than a sixth symbol: RECORDS §2.4 pins the enumeration, and a
     reason the gateway cannot name is worth less than an imprecise one it can. *)
  panicReasonFor: code = (
    code = 1 ifTrue: [ ^ #watchdog ].
    code = 3 ifTrue: [ ^ #assert ].
    code = 4 ifTrue: [ ^ #stackOverflow ].
    code = 5 ifTrue: [ ^ #oom ].
    ^ #hardFault )
```

- [ ] **Step 3: Publish and ack from the bridge**

In `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl`, inside `tick`, immediately after
`bridge checkIn.` and its span close:

```
    (* rete#215. TWO ACTS IN ONE PLACE, and the ORDER is the whole point.

       First: if the last boot left a note, publish it — before the ack, or the report
       is lost to the very clear that is supposed to follow it. sys/panic is a
       must-deliver STREAM (RECORDS §2.4), so this is `out:`, not `put:`.

       Then: ack. That is §5.2's loop-breaker reset — "consecutive = 0 on the first
       successful check-in" — and it invents no interval. It is also the honest
       definition of a reboot loop: a node that cannot get far enough to report. This
       node checks in hundreds of times per boot, so it can never trip the breaker
       while it is working; a node that dies before its first check-in halts on the
       third try, which is exactly the dongle that shipped dead through rungs 7-8.

       GUARDED ON A CLEAN CHECK-IN. `bridge misses` not having moved this tick is what
       makes this a REPORT and not a hope: acking after a check-in that never reached
       porta would clear the breaker for a node that is failing exactly the way the
       breaker exists to catch. *)
    (panicAcked not and: [ bridge misses = missesAtTickStart ]) ifTrue: [
      | p |
      p:: platform panic.
      p isNil ifFalse: [ space out: 'sys/panic' value: p ].
      platform panicNoteAck: bridge bootId.
      panicAcked:: true ].
```

Add `panicAcked ::= false.` and `missesAtTickStart ::= 0.` to the instance-slot declaration at
line 47, and capture `missesAtTickStart:: bridge misses.` as the **first** statement of `tick`,
before `bridge checkIn`.

- [ ] **Step 4: Rebuild the images — and read the bake warning first**

**Any Path-A bake overwrites the TRACKED `nodus/tuvm/src/solidified_image.c`.** It has swapped
node identities before and was nearly committed. Before staging anything:

```bash
cd /home/david/workspace/rete && git diff --stat nodus/tuvm/src/solidified_image.c
grep -c "Method_.*Test" nodus/tuvm/src/solidified_image.c
```
If `solidified_image.c` changed and you did not intend it, **restore it** — the tracked fixture
belongs to the host suite, not to this node.

Rebuild the C6 node image (Model B / `.oimg`, which is what the board runs):

```bash
cd nodus/tuvm/zephyr && bash run.sh --board esp32c6 --build-only 2>&1 | tail -5
```

- [ ] **Step 5: Run the host gates**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py > /tmp/nsl-t7.log 2>&1; echo "exit=$?"; tail -8 /tmp/nsl-t7.log
bash tests/porta_sync_host.sh 2>&1 | tail -5
cd /home/david/workspace/rete/porta && go test ./... 2>&1 | tail -5
```
Expected: `exit=0` with a clean aux-gate line, `PORTA SYNC OK`, and all Go packages `ok`.

**A pure kernel.nsl edit passed 265/0 + PORTA SYNC OK and hung a C6 in six check-ins (#213).**
Green here means nothing about the board; Task 11 is where that is settled.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/nsl/lib/kernel.nsl nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl
git commit -m "feat(#215): #watchdog stops being aspirational — a node reports how it died"
```

---

## Task 8: The probe, and §8 row 1 on hardware — the note survives a watchdog reset

One probe node serves rows 1 and 3. This task builds it and gates the **synthetic** trigger,
which isolates *retention across a reset* from *detection*: if this fails, row 3 cannot be read.

**Files:**
- Create: `nodus/tuvm/zephyr/WedgeProbeC6.nsl`
- Create: `nodus/tuvm/tests/target/wedge-note.env`

**Interfaces:**
- Consumes: `platform panicNote` / `platform panicNoteAck:` (Task 6), `Kernel>>panic` (Task 7),
  and `CONFIG_TUVM_WEDGE_TRIGGER_S=15` as already set in Task 5 — **this task must not change
  it.** 15 s is what makes the synthetic trip beat the probe's own 30 s wedge, and reading it as
  a free parameter here is how the scenario would quietly start testing the wrong thing.

- [ ] **Step 1: Write the probe**

Create `nodus/tuvm/zephyr/WedgeProbeC6.nsl`. Copy the `scenarioName` / `tapPlan:` /
`tap:named:ok:detail:` / `tapDone` block **verbatim** from
`nodus/tuvm/zephyr/StaleHandleProbeC6.nsl:140-160`, changing only `scenarioName`, then:

```
(*
expect:
TAP wedge-note done
*)
(* rete#215 — the probe for a failure that has no signature.

   IT EMITS NOTHING ON THE BOOT THAT DIES. That is deliberate and it is what makes the
   gate readable: the runner's collector anchors on the FIRST build-id line and greps
   every `TAP <scenario>` line after it (tests/target/lib/collect_console.sh:41), so a
   plan line emitted on both boots would arrive twice and tap_verify would fail it for
   duplication. Boot 1 has nothing to say; boot 2 is the report.

   IT WEDGES ONLY IF IT WAS NOT ALREADY REPORTING. `platform panicNote` answering
   non-nil means the last boot died, so this boot's job is to testify and then keep
   ticking — not to die again inside the observation window rete#216 added.

   THE WEDGE IS REAL, NOT SIMULATED. It stops returning from a reaction, so pumpEvents
   is never called again and the pulse mailbox fills behind a live 1 s repeater. That is
   COND_PULSE_UNDRAINED, and it is the shape #192/#213 present: no fault, no panic, no
   banner, console still up.

   Holds ONE Timers instance — `platform timers` answers a FRESH one per send, and a
   second instance means dispatch: silently drops every event. Dialect: `[ ... ]
   whileTrue:`, no modulo. *)
class WedgeProbeC6 packageUsing: manifest = (
  | plat space timers tickCh ticks ::= 0. wedging ::= false.
    tapPass ::= 0. tapFail ::= 0. |
) (

  wedgeAtTick = ( ^ 30 )   (* 30 s at the 1 s period below — comfortably past boot and
                              mesh-less bring-up, and 3.75x the 8 s deadline the same
                              period implies, so the trip is unambiguous *)

  public main: platform args: args = (
    | note |
    plat:: platform.
    space:: plat space.
    timers:: plat timers.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].

    note:: plat panicNote.
    note isNil
      ifTrue:  [ 'wedge-probe: no note — this boot is the one that dies' out ]
      ifFalse: [ self reportNote: note ].

    tickCh:: space channel: 'probe/tick'.
    tickCh whenever: '' do: [ :m | self tick ].
    timers every: 1000 post: #t on: tickCh.

    [ true ] whileTrue: [ plat pumpEvents. space drain ].
    ^ self )

  (* The report, and the ONLY boot that emits TAP. Two results, planned before either
     is taken — a probe that dies between them then emits one against a promised two,
     and the runner fails it instead of reading a single green as a pass. *)
  reportNote: note = (
    | reason |
    (* FIRST, before anything can fail: this boot does not wedge. A boot that is
       reporting the last wedge must not become the next one — it would reboot inside
       the observation window and the gate would read a death it caused itself. *)
    wedging:: true.
    self tapPlan: 2.
    self tap: 1 named: 'note survived the reset' ok: true
         detail: ('detail=' , (note basicAt: 4)).
    reason:: plat panic reason.
    self tap: 2 named: 'reason is watchdog' ok: (reason = #watchdog)
         detail: ('reason=' , reason printString).
    self tapDone.
    (* Ack AFTER the report, never before: the ack clears the note, and a clear that
       ran first would leave the gate reading a green against nothing (§5.1). bootId 0
       — this probe has no porta identity and 0 means unknown, which is never invented
       into a number (#217 §6.4). *)
    plat panicNoteAck: 0.
    ^ self )

  tick = (
    ticks:: ticks + 1.
    ('wedge-probe tick ' , ticks printString) out.
    (wedging not and: [ ticks >= self wedgeAtTick ]) ifTrue: [
      wedging:: true.
      'wedge-probe: entering the wedge — nothing below this line is expected' out.
      [ true ] whileTrue: [ ] ].
    ^ self )
```

- [ ] **Step 2: Confirm the two deadlines are still in the right order**

```bash
cd /home/david/workspace/rete/nodus/tuvm
grep TRIGGER_S zephyr/prj_esp32c6_wedgegate.conf
grep -n "wedgeAtTick" zephyr/WedgeProbeC6.nsl
```
Expected: `15` and `30`. The synthetic trip must come first, or this scenario tests detection
and passes anyway.

- [ ] **Step 3: Write the manifest**

Create `nodus/tuvm/tests/target/wedge-note.env`:

```
# nodus/tuvm/tests/target/wedge-note.env — rete#215, design §8 row 1
#
# THE FIRST ROW, AND FIRST FOR A REASON. Everything downstream reports through the
# RebootNote, and the note fails by READING ZERO — which is indistinguishable from "no
# wedge ever happened". So retention across a reset is proven before the detector is
# allowed to depend on it.
#
# It uses the SYNTHETIC trigger (prj_esp32c6_wedgegate.conf), not the probe's own wedge.
# That is the isolation: a failure here is retention, and cannot be detection. The
# detector's own row is wedge-detect.
#
# class=disruptive: it reboots the board on purpose. Disposable boards only.
SCENARIO=wedge-note
NODE=WedgeProbeC6.nsl
BOARD_ID=esp32c6
CONF_EXTRA="prj_esp32c6_wedgegate.conf"
CLASS=disruptive
# 15 s to the synthetic trip, a cold reboot, and boot 2's first tick. 180 s is four times
# the observed path and is the outer bound on a run that has already decided.
TIMEOUT_S=180

# rete#216 — THE OBSERVATION WINDOW. This probe keeps ticking after its verdict, and it
# MUST: a node that reports a watchdog reboot and then dies again is the reboot loop
# §5.2 exists to catch, and a gate that stopped watching at the TAPs could not see it.
# 180 s carries bridge-health's number for the reason recorded there — every death this
# bench has recorded lands inside it, the longest being #199 at 2 min 37 s.
OBSERVE_S=180
OBSERVE_PROGRESS='^.*wedge-probe tick [0-9]+'
```

- [ ] **Step 4: Run it**

```bash
cd /home/david/workspace/rete/nodus/tuvm/tests/target && ./run.sh wedge-note
```
Expected: `TARGET 1/1 PASS`, with `wedge-note PASS 2/2`.

**Read the console log even on a pass.** It must contain, in order: a `tuvm: WEDGE cond=…`
line, a second `tuvm: build-id` banner (the reboot), and a `tuvm: reboot-note reason=1 …`
line. A pass without the `WEDGE` line means the reboot came from somewhere else.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/zephyr/WedgeProbeC6.nsl nodus/tuvm/zephyr/prj_esp32c6_wedgegate.conf \
        nodus/tuvm/tests/target/wedge-note.env
git commit -m "test(#215): the note survives the reset, proven before anything leans on it"
```

---

## Task 9: §8 row 3 — the detector fires on a real wedge

Same probe, **no** synthetic trigger. The difference between this scenario and the last one is
the whole assertion: row 1 proved the note crosses a reset, row 3 proves something *causes* the
reset without any fault being raised.

**Files:**
- Create: `nodus/tuvm/tests/target/wedge-detect.env`

- [ ] **Step 1: Write the manifest**

```
# nodus/tuvm/tests/target/wedge-detect.env — rete#215, design §8 row 3
#
# THE ROW THIS WHOLE RUNG EXISTS FOR. The probe stops returning from a reaction at tick
# 30, so pumpEvents is never called again and the pulse mailbox fills behind a live 1 s
# repeater. Nothing faults. There is no panic, no coredump, no VM Error, no reboot
# banner — the console stays `port=up`. That is exactly how #192 and #213 died, and
# before this rung it produced no evidence at all.
#
# The detector must call it at 8 x 1000 ms, dump WHICH condition fired and by how much,
# write the note, and reboot. Boot 2's TAP is the proof, and reason=#watchdog is what
# separates this from an ordinary fault: no fault handler ran.
#
# NO CONF_EXTRA. This is the DEFAULT build. Design §9 rules out a Kconfig bench/field
# split precisely so this gate exercises the code that ships.
#
# class=disruptive: it wedges and reboots the board on purpose.
SCENARIO=wedge-detect
NODE=WedgeProbeC6.nsl
BOARD_ID=esp32c6
CLASS=disruptive
# 30 s to the wedge, 8 s of deadline, up to 1 s of observer tick, a cold reboot, and
# boot 2's first tick — call it 55 s observed. 300 s is the outer bound.
TIMEOUT_S=300

# rete#216 — as wedge-note, and for the same reason: the failure this rung is chasing is
# a node that looks fine and then stops, so the verdict is provisional until the node
# has been watched well past it.
OBSERVE_S=180
OBSERVE_PROGRESS='^.*wedge-probe tick [0-9]+'
```

- [ ] **Step 2: Run it, and read the dump rather than the verdict**

```bash
cd /home/david/workspace/rete/nodus/tuvm/tests/target && ./run.sh wedge-detect
```
Expected: `wedge-detect PASS 2/2`.

Then find the dump in the collector log and check three things the TAP cannot:

```bash
grep -E "tuvm: WEDGE|tuvm: reboot-note|wedge-probe tick 30" ~/.local/state/rete/console/*.log | tail -20
```
- `cond=pulse-undrained` — **not** `timer-overdue`. The repeater keeps firing; it is the drain
  that stopped. A `timer-overdue` verdict here means the conditions are miswired.
- `overdue=` is small (single-digit seconds). A large margin means the observer thread is not
  being scheduled, which is D-P1's cooperative-priority claim failing.
- `pending=` is non-zero, and `pdr=` is **0** at the moment of the dump if the wedge is caught
  before the mailbox saturates — if `pdr` is large the detector is slower than the shed rate,
  which is worth recording even though it is not a failure.

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/tests/target/wedge-detect.env
git commit -m "test(#215): a wedge with no fault now reboots the node and says which condition fired"
```

---

## Task 10: §8 row 5 — it does NOT fire on a legitimately slow node

Design §2's whole argument, and *"the one an in-tree bridge-shaped test would never exercise."*
A battery sensor waking every ten minutes is the ordinary case this system is aimed at; a
detector that reboots it is worse than no detector.

**Files:**
- Create: `nodus/tuvm/zephyr/SlowTimerNodeC6.nsl`
- Create: `nodus/tuvm/tests/target/wedge-quiet.env`

- [ ] **Step 1: Write the slow node**

Create `nodus/tuvm/zephyr/SlowTimerNodeC6.nsl` — the same TAP block as Task 8, and:

```
(*
expect:
TAP wedge-quiet done
*)
(* rete#215 — the node the detector must NOT reboot.

   ONE repeating timer, at ten minutes, and nothing else. Between ticks this node sits in
   pulse_pump's K_FOREVER with an empty mailbox and no armed deadline in sight — which is
   BYTE-IDENTICAL, from the kernel's point of view, to the wedged node in wedge-detect.
   The only thing that separates them is what the app DECLARED, which is why the detector
   reads timer_target_snapshot() and owns no idea of its own about how often a node should
   wake (design §2/§3.1).

   It prints a line at boot and then one per tick, so the observation window has a
   positive progress predicate — but the load-bearing assertion is the ABSENCE of a
   reboot, and absence is what OBSERVE_S is for. A boot banner appearing twice inside the
   window is the failure. *)
class SlowTimerNodeC6 packageUsing: manifest = (
  | plat space timers tickCh ticks ::= 0. tapPass ::= 0. tapFail ::= 0. |
) (

  public main: platform args: args = (
    plat:: platform.
    space:: plat space.
    timers:: plat timers.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].

    tickCh:: space channel: 'probe/tick'.
    tickCh whenever: '' do: [ :m | self tick ].
    (* TEN MINUTES. Against SLACK=8 that is an 80-minute deadline, so a node this slow
       cannot be judged by any window this gate can afford to sit through — which is the
       point. What the gate proves is that the node is judged against 600000 ms rather
       than against a constant, and the evidence is that it is still alive after a window
       many times longer than any FIXED deadline the detector could plausibly have
       carried. *)
    timers every: 600000 post: #t on: tickCh.

    self tapPlan: 1.
    self tap: 1 named: 'armed one 10-minute timer and nothing else' ok: true
         detail: 'period=600000'.
    self tapDone.
    'slow-node alive' out.

    [ true ] whileTrue: [ plat pumpEvents. space drain ].
    ^ self )

  tick = ( ticks:: ticks + 1. ('slow-node tick ' , ticks printString) out. ^ self )
```

- [ ] **Step 2: Write the manifest**

```
# nodus/tuvm/tests/target/wedge-quiet.env — rete#215, design §8 row 5
#
# THE FALSE-TRIP GATE. §8 names a false trip as "the failure mode that would make this
# instrument worse than nothing", and this is the only scenario that can see it.
#
# THE ASSERTION IS AN ABSENCE, so the TAP is nearly worthless here and OBSERVE_S is the
# whole gate: the node declares one 10-minute timer, says so once, and must still be the
# same boot when the window closes. A reboot inside the window prints a second build-id
# banner, which the observer half of the runner scores as a death.
#
# 600 s is chosen against the DETECTOR, not against feel: the observer's own backstop is
# 30 s and every fixed deadline it could plausibly have carried is smaller still, so a
# node alive at 10 minutes with nothing but a 10-minute timer has demonstrably been judged
# against 600000 ms and not against a constant. Shorter than that and the gate would pass
# for a detector with a 5-minute constant in it.
#
# class=disruptive: it holds the board for ten minutes doing nothing. Nothing here writes,
# reboots or latches, but it is not something to point at the bell.
SCENARIO=wedge-quiet
NODE=SlowTimerNodeC6.nsl
BOARD_ID=esp32c6
CLASS=disruptive
TIMEOUT_S=120

OBSERVE_S=600
OBSERVE_PROGRESS='^.*slow-node alive'
```

`OBSERVE_PROGRESS` matches the **boot** line rather than the tick line: at a 10-minute period
there is no tick to watch inside the window, and a predicate that could never fire would make
the gate red on a healthy node. The runner's fault scan is what carries this row.

- [ ] **Step 3: Confirm the progress predicate cannot make this gate lie, hostlessly**

```bash
python3 -m pytest tests/target/test_observe.py -q 2>&1 | tail -5
```
Expected: pass. If `collect::observe` requires the predicate to fire **repeatedly** inside the
window, this manifest would go red on a healthy node — read `collect_console.sh`'s
`collect::observe` and, if so, extend the probe to print `slow-node alive` on a short heartbeat
that is **not** a `timers every:` registration (or the detector would then have a 1 s period
declared and this gate would test nothing). State in the manifest which you did and why.

- [ ] **Step 4: Run it**

```bash
cd /home/david/workspace/rete/nodus/tuvm/tests/target && ./run.sh wedge-quiet
```
Expected: `wedge-quiet PASS 1/1`, and **no** `tuvm: WEDGE` line anywhere in the window.

```bash
grep -c "tuvm: WEDGE" ~/.local/state/rete/console/*.log
```

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/rete
git add nodus/tuvm/zephyr/SlowTimerNodeC6.nsl nodus/tuvm/tests/target/wedge-quiet.env
git commit -m "test(#215): a node that legitimately sleeps ten minutes is not rebooted"
```

---

## Task 11: §8 rows 2 and 4 — the armed soak on a real node

Two rows, one run, because they need the same thing: a healthy node with the radio up, armed,
for long enough to matter. Row 2 asks whether `task_wdt` + the hardware WDT collide with the
Thread stack — 802.15.4 bring-up is the one subsystem plausibly already installing on
`wdt0`/`wdt1`. Row 4 asks whether the detector false-trips on a node that is simply working.

**This is also the first end-to-end proof that a `Panic` reaches porta**, because the bridge is
the only workload with a porta identity.

**Files:** none. This is a bench run, and its deliverable is a reading recorded in the design's
§6.5-style addendum.

- [ ] **Step 1: Confirm which board is which before flashing anything**

```bash
ls -l /dev/serial/by-id/
cat ~/.config/rete/collector/*.env 2>/dev/null | grep -E "BOARD|PORT|DEV"
```
Resolve boards **through the collector env files, never from a table**: `c6` is the soaking
bell (`port=down` is healthy and it must not be touched), `c6spare` is the attached disposable
one, and a by-id hash matching neither entry means a third board is plugged in.

- [ ] **Step 2: Flash c6spare with the bridge, radio up, detector armed — the DEFAULT build**

```bash
cd /home/david/workspace/rete/nodus/tuvm/zephyr
./run.sh --board c6spare --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --flash
```
Verify from the console, in the first minute:

```bash
grep -E "tuvm: observer armed|tuvm: build-id" ~/.local/state/rete/console/*.log | tail -5
```
Expected: exactly one `observer armed (slack=8 tick=1000ms backstop=30000ms hw=yes)`.
**`hw=no` is a failure of row 2 in the quiet direction** — it means `DEVICE_DT_GET_OR_NULL`
found no `watchdog0` alias or the device was not ready, and the hardware layer this row exists
to test is absent while everything looks green.

- [ ] **Step 3: Prove `Panic` reaches porta, once, before committing to the long window**

Induce one reboot with the synthetic trigger on a second flash, then read the gateway:

```bash
cd /home/david/workspace/rete/nodus/tuvm/zephyr
./run.sh --board c6spare --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf;prj_esp32c6_wedgegate.conf" --flash
```
Then, **on the gateway — all of porta's operational state is on gw85224-01, not the
workstation**:

```bash
ssh gw85224-01 'cd ~/porta-v2 && ls *.db'
ssh gw85224-01 'cd ~/porta-v2 && ./porta log c6-bench --db <the db being served> | grep -i panic | tail -5'
```
**Name the `--db` explicitly.** `porta get`/`log`/`put` with a wrong or absent `--db` do not
fail — they *manufacture* a fully-schema'd empty store and answer from it, so an empty result is
indistinguishable from a wrong database (#221). Read the db name from the `ls` above, never from
memory.

Expected: one `sys/panic` row carrying `reason: #watchdog`, a `detail` naming mcause/mepc/ra,
and `bootId` equal to the boot the node died on.

- [ ] **Step 4: Reflash the DEFAULT build and start the soak**

```bash
cd /home/david/workspace/rete/nodus/tuvm/zephyr
./run.sh --board c6spare --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --flash
date -Is    # record the start; the session clock is UTC and the bench is MST
```

- [ ] **Step 5: Score it after at least 12 hours**

```bash
grep -c "tuvm: build-id" ~/.local/state/rete/console/<the soak log>
grep -c "tuvm: WEDGE" ~/.local/state/rete/console/<the soak log>
grep -E "tick [0-9]+ misses" ~/.local/state/rete/console/<the soak log> | tail -3
ssh gw85224-01 'cd ~/porta-v2 && ./porta get c6-bench gw/ --db <the db being served>'
```
The run **passes** on all four of:
- `tuvm: build-id` appears **once** — one unbroken boot. A second banner is row 2 failing (the
  hardware watchdog resetting a healthy node) or row 4 failing (a false trip); the `WEDGE` count
  separates them.
- `tuvm: WEDGE` count is **0**.
- the tick line is still advancing at the end of the window.
- `gw/liveness` reads `#live`.

**12 h is a floor, not a target.** The longest death this bench has recorded is #199 at
2 min 37 s, and the previous c6spare control ran 25.4 h; hours 26+ bought nothing for the
question they served. What 12 h buys here is ~43 000 observer ticks and ~21 600 check-ins against
a false-trip rate that must be zero.

**Do not treat a slice of this run as a trend.** A flat 10 h run yields ±7σ trends in both
directions when cut into 1.5 h slices; if a number moves, slice a known-flat run before reporting
it.

- [ ] **Step 6: Record the reading in the design doc**

Append a §6.5-style addendum to `docs/design/2026-08-16-vm-liveness-observer-design.md` giving
the boot count, the wedge count, the window length, the tick the node reached, and the
`gw/liveness` value. **A number, not an adjective** — this is the row a later session will cite.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/rete
git add docs/design/2026-08-16-vm-liveness-observer-design.md
git commit -m "docs(#215): the armed detector left a healthy node alone for 12 h"
```

---

## Task 12: §7 step 4 — the HALT arm, with `gw/liveness` watching

The last thing to ship, and the design says why: *"the sweep must be provably working against a
node halted by hand before the breaker is allowed to halt one on its own — otherwise the first
node the breaker stops is also the first node nothing is watching."* That sweep is built and
verified (#217, 25.9 s against a 36 s bound), so this is unblocked.

- [ ] **Step 1: Prove the breaker trips, using a build that cannot check in**

`prj_esp32c6_faultgate.conf` faults at boot, before the VM starts — so the node never reaches a
check-in and never acks. That is precisely §5.2's *"a genuine boot-OOM loop never checks in at
all, so it halts on the third try."*

```bash
cd /home/david/workspace/rete/nodus/tuvm/zephyr
./run.sh --board c6spare --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf;prj_esp32c6_faultgate.conf" --flash
grep -E "tuvm: FATAL|tuvm: HALT|tuvm: build-id" ~/.local/state/rete/console/*.log | tail -12
```
Expected, in order: `build-id`, `FATAL … consec=1`, `build-id`, `FATAL … consec=2`, `build-id`,
`FATAL … consec=3`, `HALT — 3 consecutive faults with no check-in between`. **Three boots, then
nothing.**

- [ ] **Step 2: Prove the HALT actually holds — this is D-P5's gate**

```bash
sleep 300
grep -c "tuvm: build-id" ~/.local/state/rete/console/*.log
```
Expected: the count is **unchanged** from step 1 (three). A fourth banner ~11 s after the HALT
means `wdt_disable()` did not take, and the loop breaker has silently become the reboot loop it
exists to stop — the failure is invisible in the HALT line itself, which prints either way.

- [ ] **Step 3: Prove the corpse is visible from the gateway**

```bash
ssh gw85224-01 'cd ~/porta-v2 && ls *.db'
ssh gw85224-01 'cd ~/porta-v2 && ./porta get c6-bench gw/liveness --db <the db being served>'
ssh gw85224-01 'cd ~/porta-v2 && ./porta log c6-bench --db <the db being served> | tail -10'
```
Expected: `gw/liveness` reads `#silent`, written **once**, within `3 × cadence + 30 s` of the
HALT. This is the sequencing constraint §5.2 named — *"a halted node reports nothing; without
`gw/liveness` the breaker would trade a noisy zombie for an invisible corpse."* **A `#silent`
that never arrives fails this task even though every console line was correct**, because the
breaker is only safe when this is true.

Note the interaction with **#220**: the bridge runs at a 2 s cadence, so its deadline is 6 s and
routine blocking work has been observed crossing it. A spurious `#silent`/`#live` pair *before*
the halt is #220, not a failure here — read the row that persists.

- [ ] **Step 4: Restore c6spare to something useful**

```bash
cd /home/david/workspace/rete/nodus/tuvm/zephyr
./run.sh --board c6spare --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" --flash
```
A halted board left halted is a board someone will later mistake for a dead one.

- [ ] **Step 5: Commit the reading**

```bash
cd /home/david/workspace/rete
git add docs/design/2026-08-16-vm-liveness-observer-design.md
git commit -m "docs(#215): three faults halt the node, and the gateway can see it happen"
```

---

## Task 13: Close-out — promote the knowledge out of `docs/superpowers/`

**A rung is not closed until its durable knowledge reaches a mainline document of record.** The
dated design docs stay behind as the historical "why"; this plan becomes scaffolding.

- [ ] **Step 1: `review/program-sequence.md` — the rung**

Add the rung entry in the ladder's own voice: what was built, what the bench said (numbers, not
adjectives), and what building it **corrected**. At minimum the three corrections this plan
found before it ran:
- `task_wdt_feed()` cannot be called from ISR context, and `CONFIG_ASSERT` is off in this build,
  so the design's implied shape would have corrupted scheduler state silently (D-P1).
- `struct arch_esf` on this SoC carries no `mcause` (D-P3).
- a HALT that leaves the hardware watchdog armed is a reboot loop wearing a halt's costume
  (D-P5).

> `review/` is David's review-agent tree. **Add the rung; never sweep, reorder or tidy anything
> else there.**

- [ ] **Step 2: `docs/design/2026-08-10-node-memory-and-liveness-design.md`**

- §8's constants table gains three rows: `wedge slack` (8), `observer tick` (1000 ms) and
  `observer backstop` (30000 ms), each with its standing and what would earn a revision — the
  same discipline the existing five rows carry.
- §7's staging table: step 4 moves from **UNBLOCKED** to **DONE**, with the readings.
- §5.1's `RebootNote` struct is corrected to match what shipped (`boot_id` semantics, and
  `mcause` from the CSR).

- [ ] **Step 3: `docs/design/2026-08-16-vm-liveness-observer-design.md`**

Record the surviving plan decisions **D-P1** through **D-P6** as an addendum, in the shape #217
used for §6.4/§6.5: what the design said, what building it settled, and what it corrected. §6's
module location and posix stub are superseded by D-P4 and must say so — a design left reading as
though a file exists at `src/backend/zephyr/observer.c` will send the next session looking for it.

- [ ] **Step 4: `protocol/PROTOCOL-NSL-RECORDS.md` §2.4**

`#watchdog` ceases to be aspirational. Name what produces it (the wedge detector), what `detail`
carries (the register triple, or the condition and margin), and that `core` is `nil`.

- [ ] **Step 5: `docs/next-steps-overview.md`**

Update the #215 entry to DONE with its readings, and note that §7's staging is now complete.

- [ ] **Step 6: `CLAUDE.md` — the gate list**

Add the three new target scenarios to the target-gate note, so a later session running
`tests/target/run.sh` knows what it is getting.

- [ ] **Step 7: File what this rung found and did not fix**

At least one is already known: **`monotonicMillis` and `Panic.monoMs` are SMIs, which hold only
2^30−1 on a 32-bit target — so uptime wraps at ~12.4 days.** It is pre-existing (`platform.c:1034`,
and `obs/health`'s `up` has always carried it) and out of scope here, but a field node running
longer than a fortnight reports a wrong uptime with no indication. **File it before citing it
anywhere.**

- [ ] **Step 8: Final gates, then close**

```bash
cd /home/david/workspace/rete/nodus/nsl && python3 tools/run_tests_oevm.py > /tmp/nsl-final.log 2>&1; echo "exit=$?"; tail -8 /tmp/nsl-final.log
cd /home/david/workspace/rete/nodus/tuvm && bash tools/run_c_tests.sh 2>&1 | tail -30
cd /home/david/workspace/rete/nodus/tuvm && bash tests/porta_sync_host.sh 2>&1 | tail -3
cd /home/david/workspace/rete/porta && go test ./... 2>&1 | tail -5
cd /home/david/workspace/rete/nodus/tuvm/tests/target && ./run.sh
```
Expected: `exit=0` with a clean aux-gate line, every C test `PASS`, `PORTA SYNC OK`, all Go
packages `ok`, and `TARGET n/n PASS` across every scenario including the three added here.

- [ ] **Step 9: Commit and close**

```bash
cd /home/david/workspace/rete
git add review/program-sequence.md docs/design/ docs/next-steps-overview.md \
        protocol/PROTOCOL-NSL-RECORDS.md CLAUDE.md
git commit -m "docs(#215): a wedged node has a voice — promoting the rung to the record"
gh issue close 215 --comment "<the readings, and what building it corrected>"
```

---

## Self-review notes

**Spec coverage.** Design §1's table has five rows; four are built here (`Panic`+`#watchdog`,
`RebootNote`, loop breaker, wedge detection) and the fifth (`gw/liveness`) shipped in #217 and is
what Task 12 leans on. §2 is answered by Task 3 + Task 5's `observer_verdict`. §3.1/§3.2/§3.3 are
Task 5. §4's timeout path is Task 5 plus D-P5. §5 is Tasks 6-7. §6's boundary is Task 5 as
amended by D-P4. §7's second loss path shipped as #214 (`pulse_stale`, already in the tree). §8's
five rows are Tasks 8, 11, 9, 11, 10 respectively. §9's exclusions are honoured: no core dumps,
nothing new on the wire beyond `Panic`, no Kconfig bench/field split of the detector, no counter
resets. §10's promotion is Task 13.

**Known gaps, stated rather than hidden.**
- The posix side of `pulse_pending()` (Task 4 Step 2) is the one step that names no exact code,
  because `pulse_posix.c`'s ring bookkeeping has to be read first. It is two lines and the test
  is that the host links.
- `collect::observe`'s exact predicate semantics are assumed in Task 10 and verified in that
  task's Step 3 rather than in advance.
- The wedge detector cannot see a node with **no** declared repeating timer. That is design §2's
  deliberate position, not an oversight — but it means a purely GPIO-driven node is outside this
  instrument, and that should be said out loud in the §10 promotion.
