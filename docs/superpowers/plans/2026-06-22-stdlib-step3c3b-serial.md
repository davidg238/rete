# Std-lib step 3c-3b — Serial (first native capability tier) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a confined (non-ambient) `Serial` native capability — a `.ns` Kernel wrapper over a static C byte-port singleton with a link-time host/device backend (host loopback now) — and establish the per-capability C-module + dispatch-hook composition pattern (also extracting `platform.c`).

**Architecture:** A new static C singleton `&SerialObj` exposes three receiver-gated byte prims (`basicReadByte`/`basicWriteByte:`/`basicAtEnd`) over a `serial_backend.h` link-time seam (`serial_host.c` = in-memory loopback FIFO). A thin Kernel `.ns` class `Serial` (no public accessor) holds `&SerialObj` and supplies the byte/line protocol. Boot constructs the wrapper via a non-public `Kernel>>serialOn:` and hands it to the app's entry as a second capability arg (`usingPlatform: platform serial: serial`). Each native capability becomes its own `.c` module reached by an early receiver-identity hook in `send()`; this cycle also extracts the existing platform prims into `platform.c`.

**Tech Stack:** C99 (oevm VM: `dispatch.c`, `gc.c`, `kernel_boot.c`, `oimg_loader.c`), Python compiler (`ns-oevm/tools/ns_compile_oevm.py`), Newspeak std-lib (`ns-oevm/lib/kernel.ns`), conformance `.ns` (ns-tests).

## Global Constraints

- **Three sibling repos:** `ns-oevm` (compiler/lib/docs, cwd `/home/david/workspace/ns-oevm`), `oevm` (C VM, `/home/david/workspace/oevm`), `ns-tests` (conformance, `/home/david/workspace/ns-tests`). Commit **per-repo**; every commit ends with the trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. **Do NOT push** unless the controller says so.
- **Run conformance from `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py` (defaults to `OEVM_PATH=AB` = both image paths). NO single-test filter — run the full suite, read the summary line. To gate one path during development: `OEVM_PATH=A` or `OEVM_PATH=B`.
- **The runner CLOBBERS `../oevm/src/solidified_image.c`.** Before committing/pushing `oevm`, restore it (`git checkout src/solidified_image.c`) UNLESS this plan's final task deliberately regenerates the committed default image — then commit the regenerated file.
- **Baseline:** suite is **61 pass / 0 fail / 3 skip** on both paths at HEAD (ns-oevm `dd4777c`, oevm `5ce2678`, ns-tests `c634874`). Target end state: **62/0/3** both paths (one new test file `38_serial.ns`), or **63/0/3** if the confinement evidence becomes a runtime pin (Task 5 Step 3).
- **Capability-purity (dialect #8/#9):** apps reach capabilities ONLY through library classes / receiver-side protocol; no native-prim forwarders that leak ambient access. `Serial` is non-ambient: reachable only via the boot-injected `serial:` entry arg, never via `platform`/Kernel.
- **Inside `lib/kernel.ns` NEVER write `Array new:`** (compiles to fixed-slot `OP_NEW`); use `platform basicNew: Array indexable: n`. String results are built via `platform string:`.
- **Nil-check stream/serial results with `== nil`** (the `String`-on-`isNil` VM gap, oevm#6) — never `isNil`/`notNil` on a possibly-String/possibly-nil value.

---

## File structure

| File | Repo | Responsibility |
|---|---|---|
| `src/platform.h` | oevm | extern decls for `platform_object()` (already in oevm.h) + the extraction's promoted internals if a private header is preferred; `platform_dispatch` prototype |
| `src/platform.c` | oevm | **NEW** — `PlatformObj`/`PlatformVtable`/`platform_initialized`/`platform_object()`, `indexable_*`/`IndexableVtable`, and `platform_dispatch(int pid,int argc,oop*args,oop*out)` (the moved block) |
| `src/serial.h` | oevm | **NEW** — `serial_object()`, `serial_dispatch(oop sel,int argc,oop*args)` |
| `src/serial.c` | oevm | **NEW** — `&SerialObj` singleton + lazy `serial_object()` (calls `serial_open()`) + own selector matching calling the backend |
| `src/serial_backend.h` | oevm | **NEW** — the fixed link-time seam (`serial_open`/`serial_read_byte`/`serial_write_byte`/`serial_at_end`) |
| `src/serial_host.c` | oevm | **NEW** — host loopback-FIFO implementation of the seam |
| `src/dispatch.c` | oevm | MODIFY — replace the inline platform block with a hook to `platform_dispatch`; add early `serial_object()` receiver hook; promote `BoundClassVtable`/`boundclass_vtable_initialized` to a header |
| `src/oevm.h` | oevm | MODIFY — add `serial_object()`, `serial_dispatch()`, and the promoted `BoundClassVtable`/`boundclass_vtable_initialized` externs |
| `src/oimg_loader.c` | oevm | MODIFY — Path B: construct the Serial wrapper + thread the 2nd entry arg; format_version 4 `serial_on_sel_ref` |
| `src/kernel_boot.c` | oevm | (read-only ref; vtable wiring is generated — likely no change) |
| build scripts | oevm | MODIFY — add `platform.c`, `serial.c`, `serial_host.c` to BOTH binaries' link lines |
| `lib/kernel.ns` | ns-oevm | MODIFY — add nested `Serial` class + non-public `Kernel>>serialOn:`; **no** `public Serial` accessor |
| `tools/ns_compile_oevm.py` | ns-oevm | MODIFY — Path A: construct the Serial wrapper + thread the 2nd entry arg by entry signature |
| `docs/NEWSPEAK_DIALECT.md` | ns-oevm | MODIFY — add row #13 |
| `04_somns_basic/38_serial.ns` | ns-tests | **NEW** — round-trip + EOF + confinement conformance test |

---

## Task 1: Extract `platform.c` (behaviour-preserving) + establish the module hook

**Files:**
- Create: `oevm/src/platform.c`, `oevm/src/platform.h`
- Modify: `oevm/src/dispatch.c` (remove block + add hook + promote 2 statics), `oevm/src/oevm.h`, both build scripts
- Test: full conformance suite (the safety net for this refactor)

**Interfaces:**
- Produces: `oop platform_object(void)` (unchanged signature, defn moves to platform.c); `int platform_dispatch(int pid, int argc, oop *args, oop *out)` (returns 1 if handled, writing `*out`; 0 to fall through). `extern oop BoundClassVtable_addr;`/`extern int boundclass_vtable_initialized;` promoted to `oevm.h`.

This task only RELOCATES code and adds a hook — no behaviour change. The conformance suite (61/0/3 both paths) is the test.

- [ ] **Step 1: Confirm green baseline**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `61 pass / 0 fail / 3 skip` (both paths). Record it.

- [ ] **Step 2: Promote the two BoundClass statics so platform.c can see them**

In `oevm/src/dispatch.c`, the `basicNew:indexable:` case reads `boundclass_vtable_initialized` and `(oop)&BoundClassVtable` (defined static at `dispatch.c:463-464`). Add a stable accessor rather than exporting the static struct address inconsistently. In `dispatch.c`, just after the `BoundClassVtable` definition, add:

```c
/* Exported so platform.c (and future capability modules) can recognise a
   BoundClass class-object argument without duplicating the vtable. */
int boundclass_is(oop o) {
    return o != NULL && !IS_SMI(o) && boundclass_vtable_initialized &&
           obj_vtable(o) == (oop)&BoundClassVtable;
}
```

In `oevm/src/oevm.h`, near the other dispatch externs, add:
```c
int boundclass_is(oop o);
```

- [ ] **Step 3: Create `platform.h`**

Create `oevm/src/platform.c`'s companion `oevm/src/platform.h`:
```c
#ifndef OEVM_PLATFORM_H
#define OEVM_PLATFORM_H
#include "oevm.h"
/* platform_object() is already declared in oevm.h. */
/* Returns 1 and writes *out if pid is a platform primitive; else 0. */
int platform_dispatch(int pid, int argc, oop *args, oop *out);
/* The platform-facility pid range, defined in dispatch.c's enum and shared so
   the hook can pass pid through. (Kept in dispatch.c's enum; see Step 5.) */
#endif
```

- [ ] **Step 4: Create `platform.c` — move the singleton, the indexable helpers, and the block**

Create `oevm/src/platform.c`. MOVE verbatim from `dispatch.c`: `IndexableVtable` + `indexable_init()` (`dispatch.c:~395-408`), `indexable_new()` (`dispatch.c:411-421`), `PlatformObj`/`PlatformVtable`/`platform_initialized` (`dispatch.c:431-433`), `platform_object()` (`dispatch.c:435-448`). Then add `platform_dispatch` wrapping the moved switch body verbatim (cases from `dispatch.c:699-776`):

```c
#include "platform.h"
#include "oimg.h"
#include <stdio.h>
#include <string.h>

extern oop gc_allocate(oop vtable, size_t size);
extern oop StringVtable, SymbolVtable, VtableVtable;
extern const struct Symbol true_sym_obj, false_sym_obj;   /* match oevm.h forms */
#define true_symbol  ((oop)&true_sym_obj)
#define false_symbol ((oop)&false_sym_obj)

/* ---- moved: IndexableVtable, indexable_init, indexable_new ---- */
/* (paste dispatch.c:395-421 verbatim) */

/* ---- moved: PlatformObj / PlatformVtable / platform_initialized / platform_object ---- */
/* (paste dispatch.c:431-448 verbatim) */

int platform_dispatch(int pid, int argc, oop *args, oop *out) {
    (void)argc;
    switch (pid) {
    /* paste the case bodies from dispatch.c:701-774 verbatim, replacing each
       `return X;` with `{ *out = X; return 1; }` and the BoundClass guard in
       PRIM_pf_basicNewIndexable with boundclass_is(cls). Use the PRIM_* names
       (they stay defined in dispatch.c's enum; include via a shared header or
       re-declare the needed constants — see Step 5). */
    default:
        return 0;
    }
}
```

NOTE for the implementer: `true_symbol`/`false_symbol` in the original are macros to the canonical symbol objects exported in `oevm.h` (`true_sym_obj`/`false_sym_obj`, oevm.h:288-289). Match the exact exported types when writing the externs above. `printf` for `print:`/`printNewline:` stays identical.

- [ ] **Step 5: Make the PRIM_* enum constants visible to platform.c**

The platform pids (`PRIM_ix_array`, `PRIM_pf_identicalTo`, `PRIM_pf_hashcode`, `PRIM_pf_print`, `PRIM_pf_printNewline`, `PRIM_pf_basicNewIndexable`, `PRIM_pf_string`) live in the anonymous enum at `dispatch.c:567-588`. The simplest move that keeps the shared selector→pid cache intact: keep the enum + `PRIM_NAMES` + `selector_prim_id` in `dispatch.c`, and give the seven platform pids stable values via a small shared header. Create `oevm/src/prim_ids.h` with ONLY the platform pid constants as an enum and `#include` it from both `dispatch.c` (replacing those seven enum members with the include) and `platform.c`. Keep all non-platform pids in dispatch.c's enum exactly as they are. (If the implementer finds the enum ordering brittle, the equivalent fallback is `#define PRIM_pf_string 67` etc. with values read off the current enum — but the shared-enum-header is cleaner.)

- [ ] **Step 6: Replace the inline block in dispatch.c with the hook**

In `dispatch.c`, DELETE the whole `if (receiver == (oop)&PlatformObj && platform_initialized) { switch(pid){...} }` block (`dispatch.c:699-778`) and the now-moved `indexable_*`/`PlatformObj`/`platform_object` definitions. Replace the block with:
```c
            /* Platform capability now lives in platform.c (std-lib 3c-3b). */
            if (receiver == platform_object()) {
                oop out;
                if (platform_dispatch(pid, argc, args, &out)) return out;
            }
```
Add `#include "platform.h"` near the top of `dispatch.c`. (Leave the indexable-basics block at `dispatch.c:667-694` untouched — it is receiver-shape-gated, not platform-gated.)

- [ ] **Step 7: Add platform.c to both build scripts**

Path A build (e.g. `oevm/bench.sh` and the runner's Path-A compile in `ns-oevm/tools/run_tests_oevm.py`) and Path B build (e.g. `oevm/tests/test_load_run.sh` and the runner's Path-B compile): add `src/platform.c` to every `gcc ... src/dispatch.c ...` line. Find them: `grep -rn "src/dispatch.c" /home/david/workspace/oevm /home/david/workspace/ns-oevm/tools`.

- [ ] **Step 8: Run the full suite — must be unchanged**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `61 pass / 0 fail / 3 skip` both paths — identical to Step 1. If any regression, the extraction changed behaviour: diff the moved code against `dispatch.c:699-776` byte-for-byte.

- [ ] **Step 9: Restore clobbered image, commit oevm**

```bash
cd /home/david/workspace/oevm && git checkout src/solidified_image.c
git add src/platform.c src/platform.h src/prim_ids.h src/dispatch.c src/oevm.h bench.sh tests/test_load_run.sh
git commit -m "$(cat <<'EOF'
refactor(vm): extract platform capability into platform.c

Move PlatformObj/platform_object/indexable_* and the platform-receiver
dispatch block out of the 66KB dispatch.c into a dedicated platform.c,
reached by a receiver-identity hook. Behaviour-preserving (suite 61/0/3
both paths unchanged); establishes the one-capability=one-module pattern
that serial.c follows. std-lib step 3c-3b task 1.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```
(Also commit the ns-oevm-side runner/build edit if the runner embeds the compile command: `cd /home/david/workspace/ns-oevm && git add tools/run_tests_oevm.py && git commit ...` with the same trailer.)

---

## Task 2: `serial.c` native module + `serial_backend.h` + `serial_host.c` (loopback) + receiver hook

**Files:**
- Create: `oevm/src/serial.h`, `oevm/src/serial.c`, `oevm/src/serial_backend.h`, `oevm/src/serial_host.c`
- Modify: `oevm/src/dispatch.c` (early serial hook), `oevm/src/oevm.h`, both build scripts
- Test: `oevm/tests/test_serial.c` (a small C smoke test) — serial is not yet reachable from `.ns` (no wrapper/boot wiring until Task 3)

**Interfaces:**
- Produces: `oop serial_object(void)` (lazy singleton; calls `serial_open()`); `oop serial_dispatch(oop selector, int argc, oop *args)` (own selector matching for `basicReadByte`/`basicWriteByte:`/`basicAtEnd`; returns SMI/nil/boolean). Backend seam (Consumes from `serial_host.c`): `void serial_open(void); int serial_read_byte(void); void serial_write_byte(int b); bool serial_at_end(void);`.

- [ ] **Step 1: Write the failing C smoke test**

Create `oevm/tests/test_serial.c`:
```c
#include "../src/oevm.h"
#include "../src/serial.h"
#include "../src/serial_backend.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    serial_open();
    /* loopback FIFO: write then read echoes back, in order */
    serial_write_byte('h'); serial_write_byte('i');
    assert(!serial_at_end());
    assert(serial_read_byte() == 'h');
    assert(serial_read_byte() == 'i');
    assert(serial_at_end());
    assert(serial_read_byte() == -1);   /* empty -> -1 */
    printf("test_serial OK\n");
    return 0;
}
```

- [ ] **Step 2: Run it to confirm it fails to build (symbols undefined)**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_serial.c src/backend/posix/serial_posix.c -o /tmp/test_serial 2>&1 | head`
Expected: FAIL — `serial_backend.h` not found / undefined refs.
(Backend path updated post-3c-3b: backends moved to `src/backend/<target>/` — oevm#10.)

- [ ] **Step 3: Write `serial_backend.h` (the seam)**

```c
#ifndef OEVM_SERIAL_BACKEND_H
#define OEVM_SERIAL_BACKEND_H
#include <stdbool.h>
/* Fixed per-target seam. Exactly one implementation is linked per build:
   serial_host.c (loopback mock) now; serial_zephyr.c / serial_esp_uart.c later. */
void serial_open(void);        /* idempotent init at boot */
int  serial_read_byte(void);   /* next byte 0..255, or -1 when no data available */
void serial_write_byte(int b); /* b in 0..255 */
bool serial_at_end(void);      /* true when no byte currently available */
#endif
```

- [ ] **Step 4: Write `serial_host.c` (in-memory loopback FIFO)**

```c
#include "serial_backend.h"
#include <stddef.h>
/* Host mock: a single static byte FIFO. write appends, read consumes (TX->RX
   echo). Deterministic; no stdio/file coupling. Fixed ring buffer; on overflow
   the oldest unread byte is dropped (host test never approaches the bound). */
#define SERIAL_FIFO_CAP 4096
static unsigned char fifo[SERIAL_FIFO_CAP];
static size_t head = 0;   /* next read  */
static size_t tail = 0;   /* next write */
static size_t count = 0;
void serial_open(void) { head = tail = count = 0; }
void serial_write_byte(int b) {
    if (count == SERIAL_FIFO_CAP) { head = (head + 1) % SERIAL_FIFO_CAP; count--; }
    fifo[tail] = (unsigned char)(b & 0xff);
    tail = (tail + 1) % SERIAL_FIFO_CAP; count++;
}
int serial_read_byte(void) {
    if (count == 0) return -1;
    int b = fifo[head];
    head = (head + 1) % SERIAL_FIFO_CAP; count--;
    return b;
}
bool serial_at_end(void) { return count == 0; }
```

- [ ] **Step 5: Write `serial.h`**

```c
#ifndef OEVM_SERIAL_H
#define OEVM_SERIAL_H
#include "oevm.h"
oop serial_object(void);                                  /* lazy &SerialObj singleton */
oop serial_dispatch(oop selector, int argc, oop *args);   /* basicReadByte/basicWriteByte:/basicAtEnd */
#endif
```

- [ ] **Step 6: Write `serial.c` (singleton + own selector match)**

Mirror `platform_object()`'s lazy-static pattern (verified safe: static BSS, GC `mark()` skips non-heap pointers, no GC root needed — `gc.c:388` `is_valid_heap_object`).
```c
#include "serial.h"
#include "serial_backend.h"
#include "oimg.h"
#include <string.h>

extern oop VtableVtable;
extern const struct Symbol true_sym_obj, false_sym_obj;

static struct Object SerialObj;
static struct Vtable SerialVtable;
static int serial_initialized = 0;

oop serial_object(void) {
    if (!serial_initialized) {
        serial_open();
        SerialVtable.base.vtable = VtableVtable;
        SerialVtable.delegate = NULL; SerialVtable.bindings = NULL;
        SerialVtable.capacity = 0; SerialVtable.size = 0; SerialVtable.is_nested = 0;
        SerialObj.vtable = (oop)&SerialVtable;
        serial_initialized = 1;
    }
    return (oop)&SerialObj;
}

oop serial_dispatch(oop selector, int argc, oop *args) {
    (void)argc;
    const char *sel = symbol_name((struct Symbol *)selector);
    if (strcmp(sel, "basicReadByte") == 0) {
        int b = serial_read_byte();
        return (b < 0) ? NULL : TAG_SMI(b);          /* nil when empty */
    }
    if (strcmp(sel, "basicWriteByte:") == 0) {
        if (IS_SMI(args[0])) serial_write_byte((int)UNTAG_SMI(args[0]));
        return (oop)&SerialObj;
    }
    if (strcmp(sel, "basicAtEnd") == 0) {
        return serial_at_end() ? (oop)&true_sym_obj : (oop)&false_sym_obj;
    }
    /* Unknown selector on the serial capability: DNU. */
    fprintf(stderr, "VM Error: Serial doesNotUnderstand #%s\n", sel);
    abort();
}
```
(Match the exact `true_sym_obj`/`false_sym_obj` extern forms used in dispatch.c/oevm.h. `symbol_name` is the blob-resolving inline from `oimg.h`.)

- [ ] **Step 7: Run the C smoke test — passes**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_serial.c src/backend/posix/serial_posix.c -o /tmp/test_serial && /tmp/test_serial`
Expected: `test_serial OK`

- [ ] **Step 8: Add the early receiver hook in `send()` + declare in oevm.h**

In `oevm/src/dispatch.c`, immediately AFTER the BoundClass check (`dispatch.c:649-652`) and BEFORE the selector-interning `if` (`dispatch.c:654`), insert:
```c
    /* Native capability: serial. Own selector matching; adds nothing to
       PRIM_NAMES. Receiver-identity gate (std-lib 3c-3b). */
    if (receiver == serial_object()) return serial_dispatch(selector, argc, args);
```
Add `#include "serial.h"` near the top of `dispatch.c`. In `oevm/src/oevm.h` add `oop serial_object(void); oop serial_dispatch(oop, int, oop *);` (or rely on serial.h include).

- [ ] **Step 9: Add serial.c + serial_host.c to both build scripts**

Add `src/serial.c src/serial_host.c` to every link line that already lists `src/platform.c` (from Task 1 Step 7).

- [ ] **Step 10: Full suite still green (serial unreached but linked)**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `61 pass / 0 fail / 3 skip` both paths (serial code linked but no `.ns` reaches it yet).

- [ ] **Step 11: Restore image, commit oevm**

```bash
cd /home/david/workspace/oevm && git checkout src/solidified_image.c
git add src/serial.c src/serial.h src/serial_backend.h src/serial_host.c tests/test_serial.c src/dispatch.c src/oevm.h bench.sh tests/test_load_run.sh
git commit -m "$(cat <<'EOF'
feat(vm): serial native capability module + host loopback backend

&SerialObj static singleton with basicReadByte/basicWriteByte:/basicAtEnd
prims over a link-time serial_backend.h seam (serial_host.c = in-memory
loopback FIFO). Reached by an early receiver-identity hook in send();
adds nothing to PRIM_NAMES. Not yet reachable from .ns (wrapper + boot
wiring land in task 3). std-lib step 3c-3b task 2.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `Serial` Kernel wrapper + Path A boot threading → `38_serial.ns` green on Path A

**Files:**
- Modify: `ns-oevm/lib/kernel.ns` (nested `Serial` class + non-public `Kernel>>serialOn:`), `ns-oevm/tools/ns_compile_oevm.py` (Path A boot)
- Create: `ns-tests/04_somns_basic/38_serial.ns`
- Test: `38_serial.ns` on Path A (`OEVM_PATH=A`)

**Interfaces:**
- Consumes: `serial_object()` (Task 2); the boot two-stage anchor in `ns_compile_oevm.py:1940-1970`.
- Produces: `Kernel>>serialOn: aPort` (non-public) → a `Serial` instance; `Serial` protocol `writeByte:`/`write:`/`readByte`/`read:`/`readLine`/`atEnd`. Entry classes may declare `usingPlatform: platform serial: serial`.

**EMPIRICAL PROBE FIRST (the established verify-the-machinery-first pattern).** Before editing, confirm the construction mechanism with a throwaway run; bake any anchor corrections into the steps below.

- [ ] **Step 1: Probe — confirm nested-class construction + 2-arg entry threading**

Write a throwaway `/tmp/probe.ns` whose entry is `class P usingPlatform: platform serial: serial = () ( public main: a args: b = ( ^ 42 ) )` and a trivial 2-slot Kernel-nested class, compile Path A only, and confirm: (a) the compiler derives `params=["platform","serial"]` and selector `init_platform:serial:` at `ns_compile_oevm.py:1960`; (b) a nested-class instance built by `send(kernel_inst, #someCtor:, 1, {x})` reaches `platform` in its instance methods (already proven by 3c-3a ReadStream — re-confirm with `platform string:` inside a method). Record the exact generated-C arg-array shape. If construction differs from the assumption below, adjust Steps 4-5.

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py` after wiring a temporary probe test, or use a direct compile. Remove the probe before committing.

- [ ] **Step 2: Write the failing conformance test `38_serial.ns`**

Create `ns-tests/04_somns_basic/38_serial.ns`:
```
(*
Serial — confined native capability, byte/line port over a host loopback.
std-lib step 3c-3b. Entry declares the second capability `serial:`.
*)
class SerialTest usingPlatform: platform serial: serial = () (
  public main: platform2 args: args = (
    | line |
    "round-trip: write a line, read it back"
    serial write: 'hello'.
    serial writeByte: 10.                          "LF"
    line:: serial readLine.
    (line = 'hello') ifFalse: [ ^ self fail: 'readLine round-trip' ].
    serial atEnd ifFalse: [ ^ self fail: 'atEnd after drain' ].

    "readByte byte round-trip + nil at empty"
    serial writeByte: 65.
    (serial readByte = 65) ifFalse: [ ^ self fail: 'readByte' ].
    (serial readByte == nil) ifFalse: [ ^ self fail: 'readByte nil at empty' ].

    "read: returns a short String of what is available; '' when empty"
    serial write: 'ab'.
    (serial read: 5) = 'ab' ifFalse: [ ^ self fail: 'read: short string' ].
    (serial read: 5) = '' ifFalse: [ ^ self fail: 'read: empty string' ].

    "readLine strips CRLF and answers nil at EOF"
    serial write: 'x'. serial writeByte: 13. serial writeByte: 10.
    (serial readLine = 'x') ifFalse: [ ^ self fail: 'readLine CRLF strip' ].
    (serial readLine == nil) ifFalse: [ ^ self fail: 'readLine nil at EOF' ].

    platform printNewline: (platform print: 'SERIAL OK').
    ^ 0
  )
  fail: msg = ( platform printNewline: (platform print: msg). ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```
(Match the exact entry/harness idiom of the sibling tests — check `36_readstream.ns`'s `packageUsing:`/`main:args:` shape and adapt; the harness convention, not the asserts, is what must match.)

- [ ] **Step 3: Run it — confirm it fails**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py`
Expected: `38_serial` FAILS (Serial class + serial: arg threading not yet present) — suite shows one new failure.

- [ ] **Step 4: Add the `Serial` class + `serialOn:` to `lib/kernel.ns`**

Add a nested class (place it beside `WriteStream`, before the public accessors at `kernel.ns:210`). NO `public Serial` accessor (confinement). The port slot holds the native `&SerialObj`; byte/line logic builds Strings via `platform string:`:
```
  class Serial = ( | port | ) (
    (* Confined native capability: byte/line port over a link-time backend.
       `port` holds the native serial singleton; raw bytes go through its
       basicReadByte/basicWriteByte:/basicAtEnd prims. Strings via platform. *)
    setPort: aPort = ( port:: aPort. ^ self )
    public writeByte: aByte = ( port basicWriteByte: aByte. ^ self )
    public write: aString = ( 1 to: aString size do: [ :i | port basicWriteByte: (aString at: i) ]. ^ self )
    public atEnd = ( ^ port basicAtEnd )
    public readByte = ( port basicAtEnd ifTrue: [ ^ nil ]. ^ port basicReadByte )
    public read: n = ( | bytes |
      bytes:: OrderedCollection new.
      [ bytes size < n and: [ port basicAtEnd not ] ] whileTrue: [ bytes add: port basicReadByte ].
      ^ platform string: bytes asArray )
    public readLine = ( | bytes b done |
      port basicAtEnd ifTrue: [ ^ nil ].
      bytes:: OrderedCollection new. done:: false.
      [ done not and: [ port basicAtEnd not ] ] whileTrue: [
        b:: port basicReadByte.
        b = 10 ifTrue: [ done:: true ] ifFalse: [ bytes add: b ] ].
      "strip a trailing CR if present (CRLF)"
      (bytes size > 0 and: [ (bytes at: bytes size) = 13 ]) ifTrue: [ bytes removeLast ].
      ^ platform string: bytes asArray )
  ) : (
    public onPort: aPort = ( ^ self new setPort: aPort )
  )
```
Then add the non-public mint method to the `Kernel` class body (NOT in the public-accessor region):
```
  serialOn: aPort = ( ^ Serial onPort: aPort )
```
Verify `OrderedCollection` exposes `removeLast` (it does — `kernel.ns:66`) and `bytes asArray` (Collection, `kernel.ns:48`). Confirm `nil` literal is usable as a return (it is; `kernel.ns` uses `== nil`).

- [ ] **Step 5: Wire Path A boot to construct the wrapper + thread the 2nd arg**

In `ns-oevm/tools/ns_compile_oevm.py`, in the two-stage boot block (`~1940-1970`), AFTER `kernel_inst` is constructed (`~1954`) and BEFORE the entry instantiation (`~1956`), conditionally build the serial capability and the entry init args by the entry's declared params:
```python
        params = compiler.all_classes[entry_class].get("params", [])
        # Map canonical capability names -> the C expression that yields them.
        cap_expr = {"platform": "kernel_inst"}
        if "serial" in params:
            serial_on_sym = compiler.solidify.symbols["serialOn:"]
            c_code += "    extern oop serial_object(void);\n"
            c_code += "    oop serial_port = serial_object();\n"
            c_code += f"    oop serial_args[1] = {{ serial_port }};\n"
            c_code += f"    oop serial_inst = send(kernel_inst, (oop)&{serial_on_sym}, 1, serial_args);\n"
            cap_expr["serial"] = "serial_inst"
        # Build init_args in the entry's declared param order.
        argv = ", ".join(cap_expr.get(p, "fake_args") for p in params) or "kernel_inst"
        c_code += f"    oop init_args[{max(len(params),1)}] = {{ {argv} }};\n"
        init_sel = "".join(f"{p}:" for p in params) or "new"
        init_sym_var = compiler.solidify.symbols[f"init_{init_sel}"]
        c_code += f"    oop entry_inst = gc_allocate((oop)&Vtable_{entry_class}, sizeof(struct ObjectInstance) + {len(compiler.class_slots[entry_class])} * sizeof(oop));\n"
        c_code += f"    send(entry_inst, (oop)&{init_sym_var}, {len(params)}, init_args);\n"
```
Then keep the existing `main:args:` invocation (`~1967-1969`) unchanged (it passes `{kernel_inst, fake_args}` as `main: platform args:` — the entry's `main:args:` still receives the Kernel as its platform). Replace the OLD lines that built `init_args`/`entry_inst`/`init` send with the block above (the implementer should delete `~1956-1965` and insert this). Confirm `compiler.solidify.symbols` contains `"serialOn:"` (Kernel defines it) — if symbol registration needs the method referenced, the Kernel method ensures it.

- [ ] **Step 6: Run Path A — `38_serial` passes**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=A python3 tools/run_tests_oevm.py`
Expected: `62 pass / 0 fail / 3 skip` on Path A. (Path B not yet wired — gate to A.)

- [ ] **Step 7: Confirm no Path-A regression for 1-cap entries**

The change is guarded by `"serial" in params`; entries declaring only `usingPlatform: platform` take the `else` (argv = `kernel_inst`). Confirm the 61 baseline tests still pass on Path A (Step 6's count already includes them: 62 = 61 + 1).

- [ ] **Step 8: Commit (ns-oevm + ns-tests; oevm image restored)**

```bash
cd /home/david/workspace/oevm && git checkout src/solidified_image.c
cd /home/david/workspace/ns-oevm && git add lib/kernel.ns tools/ns_compile_oevm.py
git commit -m "$(cat <<'EOF'
feat(stdlib): Serial Kernel wrapper + Path A boot threading

Nested Kernel `Serial` class (writeByte:/write:/readByte/read:/readLine/
atEnd over the native &SerialObj prims; Strings via platform string:),
minted by non-public Kernel>>serialOn: and handed to the entry as a
second capability arg (usingPlatform: platform serial: serial). No public
Serial accessor -> confined. Path A boot threads the 2nd cap by entry
signature. Path B follows in task 4. std-lib step 3c-3b task 3.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
cd /home/david/workspace/ns-tests && git add 04_somns_basic/38_serial.ns
git commit -m "$(cat <<'EOF'
test(stdlib): 38_serial — Serial round-trip, EOF, CRLF strip (3c-3b)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Path B (.oimg) boot threading → `38_serial.ns` green on BOTH paths

**Files:**
- Modify: `oevm/src/oimg_loader.c` (construct wrapper + thread 2nd arg), `oevm/tools/` `.oimg` serializer (emit `serial_on_sel_ref`; format_version 3→4), `oevm/src/oimg.h` if the header struct/version constant lives there
- Test: `38_serial.ns` on Path B, then full suite both paths

**Interfaces:**
- Consumes: `oimg_bootstrap` two-stage boot (`oimg_loader.c:147-169`); `img_init_argc` (already read, `oimg_loader.c:123`); `serial_object()`.
- Produces: format_version 4 image header carrying `serial_on_sel_ref`; loader constructs the Serial wrapper and threads it as the 2nd entry arg when `img_init_argc >= 2`.

**EMPIRICAL PROBE FIRST.** Path B reconstructs the entry from image metadata. The Serial wrapper is built by sending `#serialOn:` to `kernel_inst`; the loader needs that selector's ref. The nested `Serial` vtable rides the Kernel graph for free (it is a literal in `serialOn:`'s method) — only the selector ref is new. Confirm before editing.

- [ ] **Step 1: Probe — verify `38_serial` currently fails on Path B and why**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=B python3 tools/run_tests_oevm.py`
Expected: `38_serial` FAILS on Path B (loader passes only `kernel_inst`; entry expects 2 args). Confirm the failure mode (arity/DNU), and inspect the current `.oimg` header layout in `oevm/src/oimg_loader.c:117-128` + the serializer.

- [ ] **Step 2: Bump format_version to 4 and add `serial_on_sel_ref` to the header (serializer side)**

Find the `.oimg` serializer (`grep -rn "format_version\|init_sel_ref\|kernel_init_sel" /home/david/workspace/oevm/tools`). Add a header word `serial_on_sel_ref` AFTER the format_version-3 Kernel descriptor words (`img_kernel_*`), written only when an entry declares `serial:` (else 0). Bump the written `format_version` to 4. Keep the version constant in sync (`grep -rn "format_version\|FORMAT_VERSION\|>= 3" /home/david/workspace/oevm/src/oimg_loader.c`).

- [ ] **Step 3: Read the new word in the loader**

In `oevm/src/oimg_loader.c`, after `img_kernel_nslots` (`:127`), add:
```c
    /* std-lib step 3c-3b: ref to Kernel>>serialOn: for minting the serial
       capability (format_version 4+). 0 when the entry takes no serial. */
    img_serial_on_sel_ref = (format_version >= 4) ? read_word(base + a + 7 * W) : 0;
```
Declare `img_serial_on_sel_ref` alongside the other `img_*` statics. Update `BUILTIN_REGISTRY_VERSION`/version-accept logic only if the loader gates on exact version (it must ACCEPT 4; reject < the new floor per the M2 precedent).

- [ ] **Step 4: Construct the wrapper + thread the arg in `oimg_bootstrap`**

In `oevm/src/oimg_loader.c:147-169`, expand `init_args` and conditionally mint serial:
```c
    oop serial_inst = NULL;
    if (img_init_argc >= 2 && img_serial_on_sel_ref != 0) {
        extern oop serial_object(void);
        oop serial_args[1] = { serial_object() };
        serial_inst = send(kernel_inst, oimg_resolve(img_serial_on_sel_ref), 1, serial_args);
    }
    oop init_args[3] = { kernel_inst, serial_inst, TAG_SMI(0) };
    send(inst, oimg_resolve(img_init_sel_ref), (int)img_init_argc, init_args);
```
(`main_args` stays `{ kernel_inst, TAG_SMI(0) }`, argc 2 — unchanged.)

- [ ] **Step 5: Run Path B — `38_serial` passes**

Run: `cd /home/david/workspace/ns-oevm && OEVM_PATH=B python3 tools/run_tests_oevm.py`
Expected: `62 pass / 0 fail / 3 skip` on Path B.

- [ ] **Step 6: Run BOTH paths — full green**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `62 pass / 0 fail / 3 skip` both paths.

- [ ] **Step 7: Confirm two-path layout parity for the new header word**

Verify a `serial:`-declaring entry produces identical observable behaviour A vs B (the `38_serial` asserts are the proof) and that 1-cap entries still emit `serial_on_sel_ref = 0` and load unchanged.

- [ ] **Step 8: Restore image, commit oevm**

```bash
cd /home/david/workspace/oevm && git checkout src/solidified_image.c
git add src/oimg_loader.c src/oimg.h tools/
git commit -m "$(cat <<'EOF'
feat(vm): Path B .oimg boot threads the serial capability (format_version 4)

oimg header gains serial_on_sel_ref (ref to Kernel>>serialOn:); the loader
mints the Serial wrapper and threads it as the entry's 2nd capability when
img_init_argc>=2. Serial's nested vtable rides the Kernel graph (no extra
ref). 38_serial green on both paths (62/0/3). std-lib step 3c-3b task 4.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Dialect #13 + regenerate committed default image + carry-forward cleanups

**Files:**
- Modify: `ns-oevm/docs/NEWSPEAK_DIALECT.md` (row #13), `oevm/src/solidified_image.c` (regenerated committed default)
- Optional: `oevm/src/dispatch.c` (carry-forward minors)

- [ ] **Step 1: Add dialect row #13**

In `ns-oevm/docs/NEWSPEAK_DIALECT.md`, append row #13 after #12 (verbatim text from the spec §6 — the five-column row: number, rule, full-Newspeak, ns-oevm behaviour, why, revisit-when).

- [ ] **Step 2: Regenerate the committed default `solidified_image.c` from `38_serial.ns`**

Per the per-step convention (each step regenerates the committed default image from its headline test). Find the regeneration command (the runner or a `solidify` invocation): `grep -rn "solidified_image" /home/david/workspace/ns-oevm/tools`. Generate from `38_serial.ns`, then:

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `62 pass / 0 fail / 3 skip` both paths with the regenerated default image committed (do NOT `git checkout` it this time).

- [ ] **Step 3: Confinement evidence (spec §7 / §9 item 4) — empirical, then pin**

Demonstrate that a class declaring only `usingPlatform: platform` cannot reach `serial`. First determine empirically what the toolchain produces for `class NoSerial usingPlatform: platform = () (...)` whose body references `serial` (compile-time unresolved-name error vs runtime DNU). THEN pin whatever it actually is:
- If it is a **runtime DNU/abort** reachable from a runnable entry, add `04_somns_basic/39_serial_confined.ns` in the spirit of `33_string_not_collection.ns` (a passing pin that asserts the failure), making the suite **63/0/3**.
- If it is a **compile-time error** (no runnable image), it cannot be a suite test — instead record it as a one-line note in the test file header / dialect #13 and in the merge summary (the compiler rejecting the name IS the confinement proof). Suite stays **62/0/3**.
Pick the branch the toolchain dictates; do not force a runtime failure if the compiler already rejects it.

- [ ] **Step 4: (Optional) sweep carry-forward minors if touched**

If convenient and in already-touched code: the step-1 deferred `fflush(stdout)` on platform `print:`/`printNewline:` (now in `platform.c`) and the `indexable_new` prototype. Only if it does not expand the diff materially; otherwise leave for a dedicated cleanup.

- [ ] **Step 5: Commit (ns-oevm doc + oevm image; + ns-tests if a confinement pin was added)**

```bash
cd /home/david/workspace/ns-oevm && git add docs/NEWSPEAK_DIALECT.md
git commit -m "$(cat <<'EOF'
docs(stdlib): NEWSPEAK_DIALECT #13 — native capability tier / Serial

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
cd /home/david/workspace/oevm && git add src/solidified_image.c
git commit -m "$(cat <<'EOF'
chore(vm): regenerate committed default image from 38_serial (3c-3b)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Final whole-suite confirmation**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `62 pass / 0 fail / 3 skip` both paths (or `63/0/3` if a confinement pin was added in Step 3). This is the merge bar.

---

## Notes for the executor

- **Verify-the-machinery-first** is mandatory for Tasks 3 and 4 (the probe steps): run a throwaway through compiler+VM and bake corrected anchors into the edits before writing the real code. This is the pattern that has carried every prior std-lib step.
- **Subagents locate edits by QUOTED CODE**, not the line numbers in this plan (the runner and prior edits shift lines). Every anchor here is paired with a quoted snippet or a `grep` to relocate it.
- **Escalate, do not silently reverse, any ratified decision** (the `.ns`-wrapper architecture, the confinement-by-no-accessor design, the nil-at-empty EOF rule). If the Path B wrapper construction proves harder than format_version 4, raise it — do not fall back to an all-C Serial without sign-off.
- **Two-path discipline:** Task 3 leaves Path B red by design (gate `OEVM_PATH=A`); both green only after Task 4. Mirrors 3b/3c-1.
```
