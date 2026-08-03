# Milestone D — Supervisor + Crash Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the let-it-crash supervisor and its `setjmp`-based crash boundary on the host reactor, plus timers, so the end-to-end `event → reaction → supervisor` loop runs with a crashing handler contained — closing the first step (first-step §7).

**Architecture:** A new C primitive `protectedFire:value:` wraps one reaction firing in `setjmp`; `self error:` (today `PRIM_error`, which `abort()`s) becomes the raise primitive that `longjmp`s to the nearest protection frame when one is active (else preserves today's print+abort). The nsl reactor loop (`Space>>drain`) fires each reaction protected and, on a caught fault, posts a first-class crash record (`Record tag: #crash`) onto the reserved `sys/crash` channel. A new nsl `Supervisor` class observes `sys/crash`, sweeps the crashed job's owner-tagged reactions (`spaceDropReactionsOwnedBy:`), and restarts or quarantines it. Timers land as a host virtual-clock event source posting onto `sys/timer`.

**Tech Stack:** C11 (the tuvm VM in `/home/david/workspace/tuvm/src`), the nsl dialect (`/home/david/workspace/nsl-tuvm/lib/kernel.ns`), the Python test runner (`nsl-tuvm/tools/run_tests_oevm.py`), black-box `.ns` conformance tests (`/home/david/workspace/nsl-tests`).

## Global Constraints

- **The crash boundary MUST be "unwind to a marked frame" with exactly one marked frame (the reactor's)** — never hard-coded "crash = reset the whole stack" (spec §2; #138; exceptions.md C4). Only `PRIM_error` becomes catchable; structural VM faults (stack overflow, corrupt home link, OOM, DNU) stay hard aborts (spec §11).
- **Terminate-only; handler runs after unwind; a crash never crosses a dispatch boundary** (exceptions.md C2/C3/C5). Each fan-out firing is wrapped individually — reaction 1 crashing must not rob reaction 2 of the event (D-C3).
- **Prim numbering is append-only.** Never insert mid-list or shift an existing id. The enum in `dispatch.c` and `PRIM_NAMES[]` must stay index-parallel (`index i → id i+1`); `prim_ids.h` mirrors the platform-prim numbers. Six predicate slots (ids 73–78) are in the enum but NOT mirrored in `prim_ids.h`; the last mirrored id is `PRIM_pf_spaceDequeue = 82`. **New platform prims start at 83.** Verify by counting `PRIM_NAMES` entries — do not trust mirrored-id arithmetic (that trap caused the 73→79 renumber).
- **Reserved channels are plain prefixes, not grammar** (Milestone C §9 Q1): `sys/timer` (timer seam) and `sys/crash` (crash observation). The supervisor owns `sys/` by holding root and never granting `sys/**` to a job. No character carries reserved meaning; A1 canonicalization is unchanged. The staged `%sys/*` spellings in `timers.ns` and `exceptions.md` are respelled `sys/*` by this plan.
- **Crash-record shape is FROZEN:** `Record tag: #crash` with fields `#text` (String — the `self error:` argument) and `#channel` (String — the crashing reaction's channel path). `#job` and `#state` are forward-reserved (not built in D — a capability is valueRank −1 and cannot be a record field; the supervisor resolves the job by prefix-matching `#channel`).
- **Owner-tag identity is a structural invariant** (spec §6; #9.2): the supervisor's grant-at-setup is the ONLY path by which a job obtains its root capability, and the registry holds that exact object; the sweep matches reactions by `==` (raw pointer identity) against it. D scopes the sweep to `owner == rootCap` (jobs register through their root capability, per demo-node / A1 discipline); prefix-tree sweep over attenuated sub-caps is deferred.
- **Quarantine constant `K = 3`** consecutive crashes (spec §5), a single named ivar; reset on job progress.
- **Tests run solo and foreground** (harness not concurrency-safe). Full-suite gate: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`. New `.ns` files under `nsl-tests/` are auto-discovered by glob — no manifest edit. Both execution paths (A = solidified, B = `.oimg`) must be byte-identical green.
- **Precise non-moving GC, exhaustion-triggered:** a C local holding an oop across an allocation is NOT rooted unless explicitly rooted (`gc_temp_root_push` / a global marked in `gc_collect`). This is the recurring project bug; the crash boundary's saved oops and the raised condition MUST be rooted.

---

## Task 1: Raise + protected-fire C machinery

**Files:**
- Modify: `/home/david/workspace/tuvm/src/vm.c` (add `vm_protected_fire` + `vm_raise` + two globals near the other interpreter globals ~`vm.c:31-48`)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (declare `vm_protected_fire`, `vm_raise`, `extern oop g_raised_condition`)
- Modify: `/home/david/workspace/tuvm/src/gc.c` (root `g_raised_condition` in the root scan, beside the other singleton `mark()`s ~`gc.c:742`)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (rewrite the `PRIM_error` case body at `dispatch.c:998-1018` to call `vm_raise`; append `PRIM_pf_protectedFire` to the enum `~:640` and `PRIM_NAMES` `~:666`)
- Modify: `/home/david/workspace/tuvm/src/prim_ids.h` (add `#define PRIM_pf_protectedFire 83` after `:55`)
- Modify: `/home/david/workspace/tuvm/src/platform.c` (add `case PRIM_pf_protectedFire` in `platform_dispatch` before `default:` `~:275`)
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (add a Kernel-instance forwarder `protectedFire:value:` for direct testing; ~`kernel.ns:968` beside `readLine`)
- Test: `/home/david/workspace/nsl-tests/11_supervision/00_protected_fire.ns`

**Interfaces:**
- Produces (C): `oop vm_protected_fire(oop block, oop value)` — runs `block value: value` to completion; returns `NIL_OOP` (the canonical nil) on clean return, or the raised condition oop (the `self error:` argument, a String) on a caught fault. `void vm_raise(oop condition)` — if a protection frame is active, stash `condition` and `longjmp`; else print `Error: <text>` and `abort()` (today's behavior). `extern oop g_raised_condition`.
- Produces (nsl): `platform protectedFire: aBlock value: v` → the above prim; the Kernel forwarder `protectedFire:value:`.

- [ ] **Step 1: Write the failing test**

Create `/home/david/workspace/nsl-tests/11_supervision/00_protected_fire.ns`:

```
(*
expect:
clean-nil
caught-boom
survived
*)
(* T-D core — the protected-fire primitive: a clean firing answers nil (the block's
   own result is discarded), a firing that `self error:`s is CAUGHT and answers the
   error text, and the process survives to run the next line. This is the setjmp
   boundary in isolation, before the reactor wraps it. *)
class ProtectedFire packageUsing: manifest = () (
public main: platform args: args = (
  | r1 r2 |
  r1:: platform protectedFire: [ 42 ] value: nil.
  (r1 == nil) ifTrue: [ 'clean-nil' out ] ifFalse: [ 'clean-WRONG' out ].
  r2:: platform protectedFire: [ :v | self error: 'boom' ] value: nil.
  ('caught-' , r2) out.
  'survived' out.
)
) : (
)
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/11_supervision/00_protected_fire.ns')))"`
Expected: FAIL — `protectedFire:value:` is not understood (DNU) or `self error: 'boom'` aborts the process.

- [ ] **Step 3: Add the two globals and `vm_raise`/`vm_protected_fire` in `vm.c`**

Near the interpreter globals (after `current_frame` / `vm_stack_ptr` are declared, ~`vm.c:48`), add:

```c
#include <setjmp.h>

/* Milestone D crash boundary: one active protection frame (the reactor's).
   Built as "unwind to a marked frame" — in D exactly one marker exists, but the
   code path is the general one so phase-2 on:do: generalizes it (exceptions.md C4). */
static jmp_buf *g_protect_top = NULL;   /* NULL = no protection active */
oop g_raised_condition = NULL;          /* rooted by gc_collect; see gc.c */

extern int gc_temp_roots_count;         /* gc.c:645 — save/restore across a fire */

/* The raise primitive. `self error:` calls this. Terminate-only: never returns to
   the raise point when protected. Unprotected -> preserve today's print+abort. */
void vm_raise(oop condition) {
    if (g_protect_top != NULL) {
        g_raised_condition = condition;
        longjmp(*g_protect_top, 1);
    }
    /* no protection frame: behave exactly as PRIM_error did before Milestone D */
    if (condition && StringVtable && obj_vtable(condition) == StringVtable) {
        struct String *s = (struct String *)condition;
        fprintf(stderr, "Error: ");
        fwrite(s->bytes, 1, s->length, stderr);
        fprintf(stderr, "\n");
    } else if (condition && SymbolVtable && obj_vtable(condition) == SymbolVtable) {
        fprintf(stderr, "Error: %s\n", symbol_name((struct Symbol *)condition));
    } else {
        fprintf(stderr, "Error: ?\n");
    }
    abort();
}

/* Run one block to completion under a fresh protection frame. Clean -> nil;
   caught fault -> the raised condition. Unwind = restore the three scalars that
   define live execution state (mirrors exit_frame, vm.c:887-888) + the temp-root
   count. The half-built object graph of the crashed block becomes unreachable and
   is swept normally (non-moving GC; nothing to fix up). */
oop vm_protected_fire(oop block, oop value) {
    jmp_buf env;
    jmp_buf *saved_top          = g_protect_top;
    size_t saved_sp             = vm_stack_ptr;
    struct MethodContext *saved_frame = current_frame;
    int saved_temp_roots        = gc_temp_roots_count;
    oop result;
    g_protect_top = &env;
    if (setjmp(env) == 0) {
        (void)run_interpreter(block, block, 1, &value);
        result = NIL_OOP;                 /* block result discarded; clean == nil */
    } else {
        vm_stack_ptr        = saved_sp;
        current_frame       = saved_frame;
        gc_temp_roots_count = saved_temp_roots;
        result              = g_raised_condition;
        g_raised_condition  = NULL;
    }
    g_protect_top = saved_top;
    return result;
}
```

Notes for the implementer: confirm the exact spellings of `vm_stack_ptr` (the cursor), `current_frame`, `NIL_OOP` (the canonical nil — grep `vm.c`/`oevm.h`; it may be `nil_object()` or a macro), `run_interpreter` signature (`vm.c:226`), and `struct MethodContext` (frame). Use whatever the tree already names them.

- [ ] **Step 4: Declare the new symbols in `oevm.h`**

Add near the other `vm.c` externs:

```c
oop vm_protected_fire(oop block, oop value);
void vm_raise(oop condition);
extern oop g_raised_condition;
```

- [ ] **Step 5: Root `g_raised_condition` in `gc.c`**

In `gc_collect`'s root scan, beside the other singleton marks (~`gc.c:742`, where `Symbol_accessViolation` is conditionally marked), add:

```c
    if (g_raised_condition) mark(g_raised_condition);
```

Add `extern oop g_raised_condition;` at the top of `gc.c` if it is not already visible via `oevm.h`.

- [ ] **Step 6: Convert `PRIM_error` to call `vm_raise`**

Replace the body of `case PRIM_error:` (`dispatch.c:998-1018`, the block ending in `abort();`) with:

```c
case PRIM_error: {
    vm_raise(args[0]);      /* longjmp to the reactor if protected, else print+abort */
    return NIL_OOP;         /* unreachable, but keeps the switch well-formed */
}
```

- [ ] **Step 7: Register the `protectedFire:value:` platform prim**

In `dispatch.c`, append to the `PRIM_*` enum (immediately after `PRIM_pf_spaceDequeue`, ~`:640`):

```c
    PRIM_pf_protectedFire,
```

and append the matching name at the SAME index position in `PRIM_NAMES[]` (~`:666`, after `"spaceDequeue"`):

```c
    "protectedFire:value:",
```

In `prim_ids.h` (after `:55`) add:

```c
#define PRIM_pf_protectedFire    83
```

Verify: count `PRIM_NAMES` entries so `"protectedFire:value:"` sits at index 82 (→ id 83). If the count says otherwise, the id is wrong — fix `prim_ids.h` to match the true position.

- [ ] **Step 8: Handle the prim in `platform.c`**

In `platform_dispatch`'s switch (before `default: return 0;`, ~`platform.c:275`):

```c
case PRIM_pf_protectedFire: {
    /* args[0] = block, args[1] = value */
    *out = vm_protected_fire(args[0], args[1]);
    return 1;
}
```

- [ ] **Step 9: Add the nsl forwarder**

In `kernel.ns`, beside `public readLine = ( ^ platform readLine )` (~`:968`), add:

```
public protectedFire: aBlock value: v = ( ^ platform protectedFire: aBlock value: v )
```

- [ ] **Step 10: Run the test on both paths and confirm it passes**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/11_supervision/00_protected_fire.ns'))); print(r.run_test_file_oimg(Path('../nsl-tests/11_supervision/00_protected_fire.ns')))"`
Expected: both print `('PASS', ...)`.

- [ ] **Step 11: Commit**

```bash
cd /home/david/workspace/tuvm && git add -A && git commit -m "feat(vm): crash boundary — self error: raises to a protectedFire setjmp frame (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "feat(kernel): protectedFire:value: forwarder + 00_protected_fire test (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 2: Protected reactor — drain wraps each firing and posts crash records

**Files:**
- Modify: `/home/david/workspace/tuvm/src/space.c` (`space_dequeue` `~:164-217` — add the channel path as a third slot of the result indexable)
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (`Space>>drain` `~:645-653` → protected firing + crash-record post; add a module-internal `Channel>>rootPath` accessor `~:598`)
- Test: `/home/david/workspace/nsl-tests/11_supervision/01_containment.ns`

**Interfaces:**
- Consumes: `vm_protected_fire` via `platform protectedFire:value:` (Task 1); `Record tag:#... with:#k -> v ...` (kernel.ns:400-406); `Map`, `Association` `->` sugar.
- Produces: `space_dequeue` now returns a 3-slot `{value, blocksSnapshot, channelPath}` indexable (`basicAt: 3` = the channel path String). `Space>>drain` fires each reaction via `protectedFire:` and, on a non-nil return (a crash), posts `Record tag: #crash with: #text -> cond with: #channel -> chPath` onto `sys/crash` — UNLESS `chPath` is under `sys/` (loop guard). `Channel>>rootPath` answers the capability's root String.

- [ ] **Step 1: Write the failing test**

Create `/home/david/workspace/nsl-tests/11_supervision/01_containment.ns`:

```
(*
expect:
good 10
bad 0
crashes 10
done
*)
(* T-D.1 containment baseline — two handlers on two channels; `bad` crashes on EVERY
   firing. Across 10 rounds `good` counts all 10 (unaffected), `bad`'s own counter
   stays 0 (its body is abandoned at the error, proving the unwind), and a counting
   observer on sys/crash sees one crash record per contained crash. The node never
   exits. Observation is a reaction on sys/crash (not take:) because drain dispatches
   — and would drop — the transient crash records otherwise. *)
class Containment packageUsing: manifest = () (
  | goodHits ::= 0. badHits ::= 0. crashes ::= 0. |
  public main: platform args: args = (
    | space good bad sys |
    space:: platform space.
    good:: space channel: 'good'.
    bad::  space channel: 'bad'.
    sys::  space channel: 'sys'.
    good whenever: '' do: [ :m | goodHits:: goodHits + 1 ].
    bad  whenever: '' do: [ :m | self error: 'boom'. badHits:: badHits + 1 ].
    sys  whenever: 'crash' do: [ :rec | crashes:: crashes + 1 ].
    1 to: 10 do: [ :i | good out: '' value: i. bad out: '' value: i ].
    space drain.
    ('good ' , goodHits printString) out.
    ('bad ' , badHits printString) out.
    ('crashes ' , crashes printString) out.
    'done' out.
  )
) : (
)
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/11_supervision/01_containment.ns')))"`
Expected: FAIL — today `self error:` inside a firing aborts the process (drain is unprotected), so no output / crash.

- [ ] **Step 3: Add the channel path to `space_dequeue`'s result**

In `space.c` `space_dequeue` (`~:164-217`), the result is currently a 2-slot indexable (`slots[1]=value`, `slots[2]=snapshot`). Widen it to 3 slots and store the channel path String as slot 3. The channel struct already holds `path`/`len` (`space.c:53`); build an nsl String from it via the same helper the rest of the module uses to make Strings (grep `space.c`/`platform.c` for the string-construction helper, e.g. `string_new`/`make_string`). Root the new String across the subsequent allocation exactly as `snapshot` is rooted (`gc_temp_root_push` / count-restore, `space.c:190,205`). Concretely, after building `snapshot` and before returning, allocate `result` with 3 slots and set:

```c
    /* slots[1] = value (already), slots[2] = snapshot (already) */
    indexable_at_put(result, 3, path_string);   /* channel path for crash records */
```

Adjust the `indexable_new` size argument for `result` from 2 to 3. Keep the existing GC-root discipline: `value`, `snapshot`, AND `path_string` must all be rooted until `result` is populated.

- [ ] **Step 4: Add `Channel>>rootPath` and make `Space>>drain` protected**

In `kernel.ns`, inside `class Channel` (~`:598`, beside `setRoot:`), add a module-internal accessor:

```
rootPath = ( ^ root )
```

Replace `Space>>drain` (`~:645-653`) with the protected reactor:

```
public drain = (
  | ev v bs ch i cond |
  [ ev:: platform spaceDequeue. (ev == nil) not ] whileTrue: [
    v::  ev basicAt: 1.
    bs:: ev basicAt: 2.
    ch:: ev basicAt: 3.
    i:: 1.
    [ i <= bs basicSize ] whileTrue: [
      cond:: platform protectedFire: (bs basicAt: i) value: v.
      cond == nil ifFalse: [ self postCrash: cond channel: ch ].
      i:: i + 1 ] ].
  ^ self )

(* A contained crash posts a first-class crash record on sys/crash — EXCEPT a crash
   raised while firing a sys/-rooted reaction (a supervisor bug): re-posting would
   amplify into a loop, so it is reported to stderr and dropped. *)
postCrash: text channel: chPath = (
  (self isSysPath: chPath)
    ifTrue: [ ('supervisor-crash ' , chPath , ' ' , text) outError ]
    ifFalse: [
      self out: 'sys/crash'
           value: (Record tag: #crash with: #text -> text with: #channel -> chPath) ].
  ^ self )

isSysPath: p = ( ^ (p = 'sys') or: [ p startsWith: 'sys/' ] )
```

Notes: `self out: 'sys/crash' value: ...` uses the existing `Channel>>out:value:` (drain runs on the root `Space`, which addresses `sys/crash`). Confirm the actual selectors: nil-canonical comparison is `== nil` (used already in drain); `startsWith:` and `outError` (stderr print) — grep `kernel.ns` for the real names (`startsWith:` likely exists for A1 path checks near `checkPath:` `~:801`; for stderr use whatever `error:`-adjacent print exists, or fall back to a plain `out` if no stderr channel exists — the loop guard's job is preventing the re-post, the print is secondary).

- [ ] **Step 5: Run the containment test on both paths**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r; print(r.run_test_file(Path('../nsl-tests/11_supervision/01_containment.ns'))); print(r.run_test_file_oimg(Path('../nsl-tests/11_supervision/01_containment.ns')))"`
Expected: both `('PASS', ...)`.

- [ ] **Step 6: Run the FULL suite to confirm no Milestone-C regression**

The `space_dequeue` shape changed (2→3 slots) and `drain` changed; the existing `09_channels/*` tests exercise `drain` heavily. Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`
Expected: `Failed: 0`; the `grant`/`debugger-integration`/`portability-32`/`boot-gc-asan` gates all PASS; count is the prior baseline + the two new supervision tests.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/tuvm && git add -A && git commit -m "feat(space): expose channel path in space_dequeue for crash records (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "feat(kernel): protected reactor — drain contains crashes, posts sys/crash records (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 3: Crash-at-depth and crash-mid-allocation (D.2/D.3, GC coherence)

**Files:**
- Test: `/home/david/workspace/nsl-tests/11_supervision/02_crash_at_depth.ns`
- Test: `/home/david/workspace/nsl-tests/11_supervision/03_crash_mid_allocation.ns`

**Interfaces:** Consumes only the protected reactor (Task 2). No new production code — these tests prove the boundary is clean from any stack shape and that the GC and the `setjmp` unwind agree (the likeliest bug-nest; spec §9). If either fails, the defect is in Task 1's scalar/temp-root restore or Task 2's `space_dequeue` rooting — fix there.

- [ ] **Step 1: Write the crash-at-depth test**

Create `/home/david/workspace/nsl-tests/11_supervision/02_crash_at_depth.ns`:

```
(*
expect:
after 1
crashes 1
done
*)
(* T-D.3 crash at depth — a handler that recurses deeply via nested block
   invocations and then faults at the bottom. Unwind to the reactor is clean from a
   deep stack shape; the healthy handler on `after` still fires, and one crash record
   is observed. *)
class CrashAtDepth packageUsing: manifest = () (
  | afterHits ::= 0. crashes ::= 0. |
  public main: platform args: args = (
    | space deep after sys |
    space:: platform space.
    deep::  space channel: 'deep'.
    after:: space channel: 'after'.
    sys::   space channel: 'sys'.
    deep whenever: '' do: [ :m | self descend: 200 ].
    after whenever: '' do: [ :m | afterHits:: afterHits + 1 ].
    sys whenever: 'crash' do: [ :rec | crashes:: crashes + 1 ].
    deep  out: '' value: 1.
    after out: '' value: 1.
    space drain.
    ('after ' , afterHits printString) out.
    ('crashes ' , crashes printString) out.
    'done' out.
  )
  descend: n = (
    n <= 0 ifTrue: [ ^ self error: 'bottom' ].
    ^ [ self descend: n - 1 ] value.
  )
) : (
)
```

- [ ] **Step 2: Write the crash-mid-allocation test**

Create `/home/david/workspace/nsl-tests/11_supervision/03_crash_mid_allocation.ns`:

```
(*
expect:
built 100
crashes 5
alive
done
*)
(* T-D.2 crash mid-allocation — the crashing handler allocates nested messages in a
   tight loop and then faults, 5 times. After recovery, allocate heavily to force GC
   cycles and verify the heap is coherent (a large structure builds and reads back).
   Proves the setjmp boundary and the precise GC agree: the half-built graph of each
   crashed firing is unreachable and swept, roots survive. Runs under the suite's
   boot-gc-asan gate. *)
class CrashMidAllocation packageUsing: manifest = () (
  | crashes ::= 0. |
  public main: platform args: args = (
    | space boom sys big |
    space:: platform space.
    boom:: space channel: 'boom'.
    sys::  space channel: 'sys'.
    boom whenever: '' do: [ :m |
      | acc |
      acc:: List with: m.
      1 to: 20 do: [ :i | acc:: List with: acc with: m ].   (* nested allocation *)
      self error: 'boom-after-alloc' ].
    sys whenever: 'crash' do: [ :rec | crashes:: crashes + 1 ].
    1 to: 5 do: [ :i | boom out: '' value: i ].
    space drain.
    (* heavy post-recovery allocation + readback *)
    big:: OrderedCollection new.
    1 to: 100 do: [ :i | big add: (List with: i with: (List with: i)) ].
    ('built ' , big size printString) out.
    ('crashes ' , crashes printString) out.
    (big size = 100) ifTrue: [ 'alive' out ] ifFalse: [ 'DEAD' out ].
    'done' out.
  )
) : (
)
```

Note: confirm `List with:with:` nesting is legal within the depth bound (≤32; `messages.md`); if 20 levels exceeds it, lower the loop bound — the point is repeated allocation before the fault, not maximal depth.

- [ ] **Step 3: Run both tests, both paths**

Run: `cd /home/david/workspace/nsl-tuvm && python3 -c "from pathlib import Path; import tools.run_tests_oevm as r;\nfor f in ['02_crash_at_depth.ns','03_crash_mid_allocation.ns']:\n  p=Path('../nsl-tests/11_supervision/'+f); print(f, r.run_test_file(p), r.run_test_file_oimg(p))"`
Expected: both PASS on both paths.

- [ ] **Step 4: Run the boot-gc-asan gate explicitly**

Run the full suite (which includes the ASAN gate): `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`
Expected: `boot-gc-asan: PASS`, `Failed: 0`. If ASAN reports a use-after-free or leak around the crash path, revisit Task 1 Step 3 (scalar/temp-root restore) and Task 2 Step 3 (path-String rooting).

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "test(supervision): crash-at-depth + crash-mid-allocation (D.2/D.3, GC coherence)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 4: Owner-sweep verb — `spaceDropReactionsOwnedBy:`

**Files:**
- Modify: `/home/david/workspace/tuvm/src/space.h` (declare `void space_drop_reactions_owned_by(oop owner);`)
- Modify: `/home/david/workspace/tuvm/src/space.c` (implement it — in-place compaction of each channel's `reactions[]`)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (enum + `PRIM_NAMES` append)
- Modify: `/home/david/workspace/tuvm/src/prim_ids.h` (`#define PRIM_pf_spaceDropReactionsOwnedBy 84`)
- Modify: `/home/david/workspace/tuvm/src/platform.c` (`case`)
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (module-internal forwarder — used by the Supervisor, Task 5)
- Test: `/home/david/workspace/nsl-tests/11_supervision/04_owner_sweep.ns`

**Interfaces:**
- Produces (C): `void space_drop_reactions_owned_by(oop owner)` — for every channel, remove each reaction whose `owner == owner` (raw pointer identity), compacting `reactions[]` in place and decrementing `n_reactions`.
- Produces (nsl): `platform spaceDropReactionsOwnedBy: cap`. A `Channel>>dropMyReactions` module-internal helper that calls it with `self` — but the Supervisor calls the platform prim directly with the job's `rootCap`.

- [ ] **Step 1: Write the failing test**

Create `/home/david/workspace/nsl-tests/11_supervision/04_owner_sweep.ns`:

```
(*
expect:
before 3
after 0
other 1
done
*)
(* T-D owner sweep — three reactions registered through capability `job` on its own
   channels, plus one through a different capability `other`. Dropping reactions owned
   by `job` removes exactly the three (they no longer fire on drain); `other`'s
   reaction is untouched. Sweep matches by == against the exact capability object. *)
class OwnerSweep packageUsing: manifest = () (
  | jobHits ::= 0. otherHits ::= 0. |
  public main: platform args: args = (
    | space job other |
    space:: platform space.
    job::   space channel: 'job'.
    other:: space channel: 'other'.
    job whenever: 'a' do: [ :m | jobHits:: jobHits + 1 ].
    job whenever: 'b' do: [ :m | jobHits:: jobHits + 1 ].
    job whenever: 'a' do: [ :m | jobHits:: jobHits + 1 ].
    other whenever: '' do: [ :m | otherHits:: otherHits + 1 ].
    job out: 'a' value: 1. job out: 'b' value: 1. other out: '' value: 1.
    space drain.
    ('before ' , jobHits printString) out.            (* 3 *)
    jobHits:: 0.
    platform spaceDropReactionsOwnedBy: job.
    job out: 'a' value: 2. job out: 'b' value: 2. other out: '' value: 2.
    space drain.
    ('after ' , jobHits printString) out.             (* 0 — swept *)
    ('other ' , otherHits printString) out.           (* 1 — first drain only... *)
    'done' out.
  )
) : (
)
```

Note: `otherHits` will be 2 after both drains — adjust the expected block to `other 2` (recount when writing: `other` fires once per drain, twice total). Set the `expect:` value to the true count; the test's purpose is that `after` is 0, not the exact `other` number.

- [ ] **Step 2: Run it and confirm it fails**

Run the single-file command from Task 1 Step 2 with this path.
Expected: FAIL — `spaceDropReactionsOwnedBy:` not understood.

- [ ] **Step 3: Implement `space_drop_reactions_owned_by` in `space.c`**

Add (near `space_register`, `~:152`):

```c
void space_drop_reactions_owned_by(oop owner) {
    for (int c = 0; c < n_channels; c++) {
        struct SpaceChannel *ch = &channels[c];
        int w = 0;
        for (int rd = 0; rd < ch->n_reactions; rd++) {
            if (ch->reactions[rd].owner != owner) {
                if (w != rd) ch->reactions[w] = ch->reactions[rd];
                w++;
            }
        }
        ch->n_reactions = w;
    }
}
```

Declare in `space.h`: `void space_drop_reactions_owned_by(oop owner);`

- [ ] **Step 4: Wire the prim** (enum, names, id, case)

`dispatch.c` enum append: `PRIM_pf_spaceDropReactionsOwnedBy,`
`PRIM_NAMES` append at the same index: `"spaceDropReactionsOwnedBy:",`
`prim_ids.h`: `#define PRIM_pf_spaceDropReactionsOwnedBy 84`
`platform.c` case:

```c
case PRIM_pf_spaceDropReactionsOwnedBy: {
    space_drop_reactions_owned_by(args[0]);
    *out = platform_object();
    return 1;
}
```

Verify id 84 by counting `PRIM_NAMES` (it must sit right after `"protectedFire:value:"`).

- [ ] **Step 5: Run the test both paths, then the full suite**

Single-file both paths (expect PASS), then `OEVM_PATH=AB python3 tools/run_tests_oevm.py` (expect `Failed: 0`).

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/tuvm && git add -A && git commit -m "feat(space): spaceDropReactionsOwnedBy: — the restart sweep verb (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "test(supervision): owner sweep drops exactly one job's reactions (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 5: The Supervisor class — restart + quarantine

**Files:**
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (add `class SupervisedJob` and `class Supervisor` as flat kernel-module siblings of `Space`, ~after `:654`; add a `Kernel>>supervisor` accessor beside `space` `~:990`)
- Test: `/home/david/workspace/nsl-tests/11_supervision/05_restart_quarantine.ns`

**Interfaces:**
- Consumes: `platform spaceDropReactionsOwnedBy:` (Task 4); the protected reactor posting `Record tag:#crash` on `sys/crash` (Task 2); `Channel>>rootPath` (Task 2); Record field read (see Step 3 — confirm the accessor).
- Produces (nsl):
  - `Kernel>>supervisor` → a fresh `Supervisor` holding the root space.
  - `Supervisor>>register: rootCap restart: aThunk progress: progressCap` → records a `SupervisedJob {rootCap, rootPath, restartThunk, crashes:=0, quarantined:=false}`; subscribes on `progressCap` to reset that job's crash count on any progress event; answers the job.
  - `Supervisor>>start` → registers the `sys/crash` reaction; answers self.
  - Internal: `handleCrash:`, `restart:`, `quarantine:`, `jobAddressing:`.
  - Quarantine limit ivar `quarantineLimit ::= 3`.

- [ ] **Step 1: Write the failing test**

Create `/home/david/workspace/nsl-tests/11_supervision/05_restart_quarantine.ns`:

```
(*
expect:
starts 3
quarantined true
survivor 6
done
*)
(* T-D.4 restart + quarantine stub — a job that crashes on every #tick. The supervisor
   restarts it after each crash (re-running its start thunk, which re-registers the
   reaction) until K=3 consecutive crashes, then quarantines it (no further restart).
   `starts` counts how many times the job's start thunk ran AFTER the first (i.e.
   restarts): 3 restarts then quarantine. A second, healthy job keeps running
   throughout (survivor), proving the supervisor isolates policy per job. *)
class RestartQuarantine packageUsing: manifest = () (
  | starts ::= 0. survivorHits ::= 0. |
  public main: platform args: args = (
    | space sup crasher survivor |
    space:: platform space.
    sup:: platform supervisor.
    crasher::  space channel: 'crasher'.
    survivor:: space channel: 'survivor'.
    (* the crasher's (re)start: register a reaction that crashes on any event *)
    sup register: crasher
        restart: [ starts:: starts + 1.
                   crasher whenever: 'tick' do: [ :m | self error: 'always' ] ]
        progress: (space channel: 'crasher/never').   (* never posted -> never resets *)
    survivor whenever: '' do: [ :m | survivorHits:: survivorHits + 1 ].
    sup start.
    (* prime the crasher's first registration, then drive events *)
    crasher whenever: 'tick' do: [ :m | self error: 'always' ].
    1 to: 6 do: [ :i | crasher out: 'tick' value: i. survivor out: '' value: i ].
    space drain.
    ('starts ' , starts printString) out.                     (* 3 restarts then quarantine *)
    ('quarantined ' , (sup isQuarantined: 'crasher') printString) out.
    ('survivor ' , survivorHits printString) out.             (* 6 — unaffected *)
    'done' out.
  )
) : (
)
```

Note on the exact `starts` count: the interplay of restart-re-registration and FIFO dispatch determines the precise number of restarts before the 3rd crash quarantines. When implementing, run the test, read the actual deterministic count, and pin the `expect:` block to it (the invariants to assert are: restarts stop at quarantine, `quarantined` is true, survivor untouched). Do NOT hand-wave the number — set it to the observed deterministic value and confirm it is identical on both paths.

- [ ] **Step 2: Run it and confirm it fails**

Single-file command with this path. Expected: FAIL — `platform supervisor` not understood.

- [ ] **Step 3: Confirm the Record field-read accessor**

The supervisor reads `#channel` out of the crash record. Grep `kernel.ns` for how a `Record`'s fields are read (`grep -n 'class Record' -A40 lib/kernel.ns`). If `Record>>at:` exists, use `rec at: #channel`. If only `Record>>fields` (the Map) exists, use `(rec fields) at: #channel ifAbsent: [ nil ]`, and confirm `Map>>at:ifAbsent:` exists (`grep -n 'class Map' -A60 lib/kernel.ns`). Use the real selectors below.

- [ ] **Step 4: Implement `SupervisedJob` and `Supervisor` in `kernel.ns`**

Add as flat siblings of `Space` (after `:654`). Use the Record/Map accessor confirmed in Step 3 in place of `recChannelOf:`:

```
class SupervisedJob = ( | rootCap rootPath restartThunk crashes ::= 0. quarantined ::= false. | ) (
  public rootCap = ( ^ rootCap )
  public rootPath = ( ^ rootPath )
  public restartThunk = ( ^ restartThunk )
  public crashes = ( ^ crashes )
  public quarantined = ( ^ quarantined )
  public bumpCrashes = ( crashes:: crashes + 1. ^ crashes )
  public noteProgress = ( crashes:: 0. ^ self )
  public markQuarantined = ( quarantined:: true. ^ self )
  addresses: chPath = (
    ^ (chPath = rootPath) or: [ chPath startsWith: (rootPath , '/') ] )
  setRootCap: c restart: t = (
    rootCap:: c. rootPath:: c rootPath. restartThunk:: t. ^ self )
) : (
  public rootCap: c restart: t = ( ^ self new setRootCap: c restart: t )
)

class Supervisor = ( | space jobs quarantineLimit ::= 3. | ) (
  setSpace: s = ( space:: s. jobs:: OrderedCollection new. ^ self )

  public register: rootCap restart: aThunk progress: progressCap = (
    | j |
    j:: SupervisedJob rootCap: rootCap restart: aThunk.
    jobs add: j.
    progressCap whenever: '' do: [ :e | j noteProgress ].
    ^ j )

  public start = (
    (space channel: 'sys') whenever: 'crash' do: [ :rec | self handleCrash: rec ].
    ^ self )

  public isQuarantined: aRootPath = (
    | j |
    j:: jobs detect: [ :x | x rootPath = aRootPath ] ifNone: [ nil ].
    ^ (j == nil) ifTrue: [ false ] ifFalse: [ j quarantined ] )

  handleCrash: rec = (
    | chPath j |
    chPath:: rec at: #channel.                    (* Step 3: real accessor *)
    j:: self jobAddressing: chPath.
    j == nil ifTrue: [ ^ self ].                  (* crash on an unowned channel: already logged *)
    j quarantined ifTrue: [ ^ self ].
    j bumpCrashes.
    (j crashes >= quarantineLimit)
      ifTrue: [ self quarantine: j ]
      ifFalse: [ self restart: j ].
    ^ self )

  restart: j = (
    platform spaceDropReactionsOwnedBy: j rootCap.
    j restartThunk value.
    ^ self )

  quarantine: j = (
    platform spaceDropReactionsOwnedBy: j rootCap.
    j markQuarantined.
    (space channel: 'sys') out: 'log'
        value: (Record tag: #quarantine with: #channel -> j rootPath).
    ^ self )

  jobAddressing: chPath = (
    ^ jobs detect: [ :j | j addresses: chPath ] ifNone: [ nil ] )
) : (
)
```

Add the accessor beside `space` (`~:990`):

```
public supervisor = ( ^ Supervisor new setSpace: self space )
```

Notes: confirm `OrderedCollection`, `detect:ifNone:`, `startsWith:`, and `->`/`Record`/`Map` selectors exist (they are used across the landed kernel; `detect:ifNone:` — grep to confirm, else use an `inject:into:`/`do:` scan in the found-flag style the no-NLR dialect requires — NO `^` inside a passed block). The `progress:` reset uses an ordinary reaction on the job's progress channel — clean-firing evidence delivered as a normal reactor event (spec §5), no special mechanism.

- [ ] **Step 5: Run the test, pin the deterministic count, both paths**

Run the single-file command, read the real `starts` count from the FAIL diff, set `expect:` to it, re-run until PASS on both A and B (they must match — if they diverge, the supervisor is depending on non-deterministic ordering; fix).

- [ ] **Step 6: Full suite**

`OEVM_PATH=AB python3 tools/run_tests_oevm.py` → `Failed: 0`.

- [ ] **Step 7: Commit**

```bash
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "feat(kernel): Supervisor — restart + quarantine over the crash channel (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 6: Timers — host virtual-clock event source + land the Timers class

**Files:**
- Modify: `/home/david/workspace/tuvm/src/platform.c` (host virtual-clock state + `host_timer_after/every/cancel`, `host_monotonic_millis`, `host_advance`; and the prim cases)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c`, `prim_ids.h` (five new prims, ids 85–89)
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (Kernel-instance forwarders `timerAfter:token:`, `timerEvery:token:`, `timerCancel:`, `monotonicMillis`, `advanceMillis:`; land `class Timers` from `tuvm/review/lib/timers.ns`, respelling `%sys/timer` → `sys/timer`; add a `Kernel>>timers` accessor)
- Test: `/home/david/workspace/nsl-tests/11_supervision/06_timers.ns`
- Test: `/home/david/workspace/nsl-tests/11_supervision/07_event_seam.ns`

**Interfaces:**
- Produces (C, host): `void host_timer_after(int token, uint64_t ms)`, `host_timer_every(int token, uint64_t ms)`, `host_timer_cancel(int token)`, `uint64_t host_monotonic_millis(void)`, `int host_advance(uint64_t ms)` — advances the virtual clock and posts each due timer's token (as `TAG_SMI(token)`) onto `sys/timer` via `space_post`, rescheduling repeaters. Prims: `timerAfter:token:` (85), `timerEvery:token:` (86), `timerCancel:` (87), `monotonicMillis` (88), `advanceMillis:` (89).
- Produces (nsl): `Timers usingSpace: space seam: platform` (the landed staged class); `Kernel>>timers` convenience if useful; the five forwarders. `Timers` owns token allocation, the pending registry, cancel bookkeeping, and `dispatch:` (posts the real (channel,message) for a fired token). Wiring: `space whenever: 'sys/timer' do: [:e | timers dispatch: e]`.

- [ ] **Step 1: Write the failing timer test**

Create `/home/david/workspace/nsl-tests/11_supervision/06_timers.ns`:

```
(*
expect:
early nil
tick 1
tick 2
cancelled
done
*)
(* T-D timers — a one-shot fires only after the virtual clock passes its due time; a
   repeating timer fires each interval; cancel stops future fires. The seam carries
   only (token, ms); Timers owns the fan-out onto the target channel. Determinism via
   advanceMillis: (host virtual clock), never wall time. *)
class TimerTest packageUsing: manifest = () (
  | ticks ::= 0. |
  public main: platform args: args = (
    | space timers ping oneShot repeater |
    space:: platform space.
    timers:: Timers usingSpace: space seam: platform.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    ping:: space channel: 'ping'.
    ping whenever: '' do: [ :m | ticks:: ticks + 1. ('tick ' , ticks printString) out ].
    oneShot:: timers after: 50 post: #t on: ping.
    (* not yet due *)
    platform advanceMillis: 10. space drain.
    ('early ' , (ticks = 0) printString) out.        (* nil==0 -> print 'early nil' via helper below *)
    (* now due *)
    platform advanceMillis: 100. space drain.        (* tick 1 *)
    repeater:: timers every: 30 post: #r on: ping.
    platform advanceMillis: 30. space drain.         (* tick 2 *)
    timers cancel: repeater.
    platform advanceMillis: 300. space drain.        (* no more ticks *)
    'cancelled' out.
    'done' out.
  )
) : (
)
```

Note: the `early nil` line is cosmetic — replace with a clean assertion that prints `early ` then `nil` only when `ticks = 0`; simplest is `('early ' , (ticks = 0) ifTrue: [ 'nil' ] ifFalse: [ 'NO' ]) out.` Adjust the `expect:` block to the exact strings your asserts print, and recount ticks against the drain sequence when finalizing.

- [ ] **Step 2: Run it and confirm it fails** (single-file command). Expected: FAIL — `Timers` / `advanceMillis:` absent.

- [ ] **Step 3: Add the host virtual-clock timer facility in `platform.c`**

Near the top of `platform.c` (after includes):

```c
#include <stdint.h>
#define HOST_MAX_TIMERS 32
struct HostTimer { int token; uint64_t due_ms; uint64_t interval_ms; int repeating; int live; };
static struct HostTimer host_timers[HOST_MAX_TIMERS];
static int host_n_timers = 0;
static uint64_t host_clock_ms = 0;

static void host_timer_add(int token, uint64_t ms, int repeating) {
    for (int i = 0; i < host_n_timers; i++) {
        if (!host_timers[i].live) {
            host_timers[i] = (struct HostTimer){ token, host_clock_ms + ms, ms, repeating, 1 };
            return;
        }
    }
    if (host_n_timers < HOST_MAX_TIMERS)
        host_timers[host_n_timers++] = (struct HostTimer){ token, host_clock_ms + ms, ms, repeating, 1 };
    /* else: silently drop — host test rig; on-target uses a real timer facility */
}
static void host_timer_after(int token, uint64_t ms) { host_timer_add(token, ms, 0); }
static void host_timer_every(int token, uint64_t ms) { host_timer_add(token, ms, 1); }
static void host_timer_cancel(int token) {
    for (int i = 0; i < host_n_timers; i++)
        if (host_timers[i].live && host_timers[i].token == token) host_timers[i].live = 0;
}
static uint64_t host_monotonic_millis(void) { return host_clock_ms; }
/* advance the virtual clock; post each now-due timer's token onto sys/timer */
static int host_advance(uint64_t ms) {
    host_clock_ms += ms;
    int fired = 0;
    for (int i = 0; i < host_n_timers; i++) {
        struct HostTimer *t = &host_timers[i];
        while (t->live && t->due_ms <= host_clock_ms) {
            oop tok = TAG_SMI(t->token);
            space_post("sys/timer", 9 /* strlen("sys/timer") */, tok);
            fired++;
            if (t->repeating) t->due_ms += t->interval_ms; else { t->live = 0; break; }
        }
    }
    return fired;
}
```

Confirm `TAG_SMI` and `space_post` are already visible in `platform.c` (the space prims use both). Confirm the `struct` compound-literal style compiles under the project's C standard; if not, assign fields individually.

- [ ] **Step 4: Wire the five prims** (append-only; verify ids by counting `PRIM_NAMES`)

`dispatch.c` enum, in order: `PRIM_pf_timerAfter, PRIM_pf_timerEvery, PRIM_pf_timerCancel, PRIM_pf_monotonicMillis, PRIM_pf_advanceMillis,`
`PRIM_NAMES` at matching indices: `"timerAfter:token:", "timerEvery:token:", "timerCancel:", "monotonicMillis", "advanceMillis:",`
`prim_ids.h`: `85..89` respectively.
`platform.c` cases:

```c
case PRIM_pf_timerAfter:   host_timer_after (SMI_VAL(args[1]), (uint64_t)SMI_VAL(args[0])); *out = platform_object(); return 1;
case PRIM_pf_timerEvery:   host_timer_every (SMI_VAL(args[1]), (uint64_t)SMI_VAL(args[0])); *out = platform_object(); return 1;
case PRIM_pf_timerCancel:  host_timer_cancel(SMI_VAL(args[0]));                             *out = platform_object(); return 1;
case PRIM_pf_monotonicMillis: *out = TAG_SMI((long)host_monotonic_millis());               return 1;
case PRIM_pf_advanceMillis: *out = TAG_SMI(host_advance((uint64_t)SMI_VAL(args[0])));       return 1;
```

Confirm the SMI accessor macro name (`SMI_VAL` / `UNTAG_SMI` — grep `dispatch.c` where `PRIM_millis` or an arithmetic prim untags). `timerAfter:token:` arg order: `args[0]=ms`, `args[1]=token` (selector `timerAfter:token:`).

- [ ] **Step 5: Add the nsl forwarders and land the `Timers` class**

In `kernel.ns`, add forwarders beside `readLine` (`~:968`):

```
public timerAfter: ms token: tok = ( ^ platform timerAfter: ms token: tok )
public timerEvery: ms token: tok = ( ^ platform timerEvery: ms token: tok )
public timerCancel: tok = ( ^ platform timerCancel: tok )
public monotonicMillis = ( ^ platform monotonicMillis )
public advanceMillis: ms = ( ^ platform advanceMillis: ms )
```

Land `class Timers` from `/home/david/workspace/tuvm/review/lib/timers.ns` into `kernel.ns` as a flat kernel-module sibling (the staged file's `class Timers` body is final nsl; paste `class Timers` and its nested `class Pending`, keeping the public surface `usingSpace:seam:`, `after:post:on:`, `every:post:on:`, `cancel:`, `now`, `dispatch:`). The staged file already targets the seam selectors added in Step 4. One respell: the staged SYNC NOTE and wiring reference `%sys/timer` — this plan's wiring uses `sys/timer` (Global Constraints); the `Timers` class body itself does not name the reserved channel (the wiring line `space whenever: 'sys/timer' do:` lives in the test/supervisor), so no change inside the class — but grep the pasted block for any `%sys` literal and drop the `%`.

- [ ] **Step 6: Write the event-seam test (D.6)**

Create `/home/david/workspace/nsl-tests/11_supervision/07_event_seam.ns`:

```
(*
expect:
observed 42
done
*)
(* T-D.6 event-source seam — the full host loop: a timer (C seam) posts onto sys/timer,
   Timers dispatches it into a job's channel, the job's reaction posts a result, and a
   supervisor-side observer sees it: event -> C seam -> out: -> reaction -> out: ->
   observed. Deterministic input via advanceMillis:. *)
class EventSeam packageUsing: manifest = () (
  public main: platform args: args = (
    | space timers job result |
    space:: platform space.
    timers:: Timers usingSpace: space seam: platform.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    job::    space channel: 'job'.
    result:: space channel: 'result'.
    job whenever: 'go' do: [ :m | result out: '' value: 42 ].
    result whenever: '' do: [ :v | ('observed ' , v printString) out ].
    timers after: 10 post: #go on: (space channel: 'job/go').
    platform advanceMillis: 20.
    space drain.
    'done' out.
  )
) : (
)
```

Note: verify the `timers after:post:on:` target-channel spelling — `Timers>>dispatch:` posts `p msg` on `p chan` at the channel's own root (`'' `), so `after: … on: (space channel: 'job/go')` delivers `#go` onto `job/go`, which the `job whenever: 'go'` reaction observes. Adjust paths so the reaction and the timer target agree.

- [ ] **Step 7: Run both tests, both paths, then full suite**

Single-file both new tests on A and B (expect PASS), then `OEVM_PATH=AB python3 tools/run_tests_oevm.py` → `Failed: 0`.

- [ ] **Step 8: Commit**

```bash
cd /home/david/workspace/tuvm && git add -A && git commit -m "feat(platform): host virtual-clock timer seam + monotonicMillis (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "feat(kernel): land Timers class + timer forwarders; event-seam test (Milestone D)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 7: Demo-node-shaped integration test (D.7)

**Files:**
- Test: `/home/david/workspace/nsl-tests/11_supervision/08_demo_node_integration.ns`

**Interfaces:** Consumes everything prior: Protocol FSM (Milestone C, in `kernel.ns`), `Timers` (Task 6), the protected reactor (Task 2), the `Supervisor` (Task 5). No new production code — if this fails, the defect is in an earlier task's integration, not here.

- [ ] **Step 1: Write the integration test**

Create `/home/david/workspace/nsl-tests/11_supervision/08_demo_node_integration.ns`. A reduced `TempNode`: a real `Protocol` FSM (`idle`/`converting`) + real timers + publish via `out:value:` + crash on `#timeout`, driven by a mock sensor the test can silence. No i2c, no ISR (phase-2). The mock sensor is a plain nsl fixture: normally it posts `#done` (conversion complete) so the FSM publishes; when silenced, the `converting`-state timeout fires `#timeout` → `self error: 'tmp117 timeout'` → contained crash → supervisor restart; after K=3 silent cycles → quarantine. A healthy reading resets the counter (progress). Structure:

```
(*
expect:
reading 235
restarted
reading 235
quarantined true
done
*)
class DemoNodeIntegration packageUsing: manifest = () (
  (* ... TempNode-shaped job: buildMachine with
       idle   -[#sample]->  startConversion (arm a timeout timer), then #converting
       converting -[#done]->  publish reading, cancel timeout, then #idle
       converting -[#timeout]-> self error: 'tmp117 timeout'  (then #idle)
     driven by chan whenever: '' do: [:e | machine handle: e]
     mock sensor: a block the test calls to post #done, or withholds to force #timeout
     supervisor: register the job with restart:[job resume: nil] progress:(reading chan)
     drive: healthy cycle -> reading; 3 silent cycles -> restart x? then quarantine ... *)
public main: platform args: args = (
  (* build, wire supervisor, drive advanceMillis: + drain across healthy and silent
     cycles, asserting: a reading is published; after silencing, the job crashes and is
     restarted (resume: rehydrates the FSM to #idle); repeated silence quarantines it;
     a healthy reading in between resets the counter. *)
)
) : (
)
```

Implement the body concretely following `tuvm/review/demo-node.ns` (the FSM shape, `startConversion`/`readMilliC`/`publish:`, `resume:` rehydration) but with the mock sensor and NO i2c/`onAlertPost:`. The reading value `235` stands in for `mockI2cWithTemp: 23500` scaled — pick a fixed mock temperature and compute the published value so it is deterministic. Pin `expect:` to the real observed stream once running; the invariants to assert: (a) a reading publishes on a healthy cycle, (b) a silent cycle crashes-and-restarts (resume: brings the FSM back to #idle and the next healthy cycle publishes again), (c) K=3 silent cycles quarantine the job, (d) a healthy reading resets the crash counter.

- [ ] **Step 2: Run both paths, iterate until PASS, pin the deterministic stream**

Single-file A and B. Read the actual output, set `expect:` to it, confirm A==B. If the stream is non-deterministic across paths, the integration has an ordering dependency — fix it, do not paper over with a loose assertion.

- [ ] **Step 3: Full suite** → `Failed: 0`.

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/nsl-tests && git add -A 2>/dev/null; cd /home/david/workspace/nsl-tuvm && git add -A && git commit -m "test(supervision): demo-node-shaped integration — FSM+timers+crash+restart+resume (D.7)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

(Confirm which repo the `nsl-tests/` tree is committed under — it is its own git repo at `/home/david/workspace/nsl-tests`; commit there. Adjust the commit block to `cd /home/david/workspace/nsl-tests && git add -A && git commit ...` if so. Verify with `git -C /home/david/workspace/nsl-tests rev-parse --show-toplevel`.)

---

## Task 8: Protocol snapshot/restore behavioral test (carry-forward #9.1)

**Files:**
- Test: `/home/david/workspace/nsl-tests/10_fsm/02_snapshot_restore.ns`

**Interfaces:** Consumes the landed `Protocol` (`snapshot` = `^ current`, `restore: s` = `current:: s`; `kernel.ns:722-723`). No production code.

- [ ] **Step 1: Write the test**

Create `/home/david/workspace/nsl-tests/10_fsm/02_snapshot_restore.ns`:

```
(*
expect:
at converting
snap #converting
moved #idle
restored #converting
done
*)
(* #9.1 — direct Protocol snapshot/restore behavioral round-trip. 10_fsm/01 only drove
   the transition cycle; deep-sleep persistence (resume:) depends on snapshot/restore,
   so pin them directly: drive the machine off its initial state, snapshot the state
   symbol, drive it further, restore the snapshot, and confirm currentState is the
   snapshotted state. *)
class SnapshotRestore packageUsing: manifest = () (
public main: platform args: args = (
  | machine snap |
  machine:: platform Protocol named: #tmp117.
  machine state: #idle on: #start do: [ :m :e | nil ] then: #converting.
  machine state: #converting on: #done do: [ :m :e | nil ] then: #idle.
  machine startIn: #idle.
  machine handle: #start.
  ('at ' , machine currentState) out.                 (* at converting *)
  snap:: machine snapshot.
  ('snap ' , snap printString) out.                   (* snap #converting *)
  machine handle: #done.
  ('moved ' , machine currentState printString) out.  (* moved #idle *)
  machine restore: snap.
  ('restored ' , machine currentState printString) out. (* restored #converting *)
  'done' out.
)
) : (
)
```

Note: verify whether `currentState` answers the symbol (so `, machine currentState` needs `printString`) — 10_fsm/01 prints `machine currentState printString`, so use `printString` consistently; adjust the first line's concatenation accordingly and fix `expect:` to match.

- [ ] **Step 2: Run both paths** → PASS. **Step 3: Commit** (in `/home/david/workspace/nsl-tests`):

```bash
cd /home/david/workspace/nsl-tests && git add -A && git commit -m "test(fsm): direct Protocol snapshot/restore round-trip (Milestone D #9.1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Task 9: `channel path too long` end-to-end test (optional, carry-forward #9.3)

**Files:**
- Test: `/home/david/workspace/nsl-tests/09_channels/11_path_too_long.ns`

**Interfaces:** Consumes the existing C cap `OEVM_SPACE_MAX_PATH = 64` and the frozen abort string `channel path too long` (`space.c:95`). The kernel→C route (a canonical path longer than the buffer reaching `space_intern` via `out:`/`whenever:`) is not covered by any `.ns` today; `test_space` property 8 covers only the C side.

- [ ] **Step 1: Write the runtime-fail test**

Create `/home/david/workspace/nsl-tests/09_channels/11_path_too_long.ns`:

```
(*
expect-runtime-fail: channel path too long
*)
(* #9.3 — a canonical channel path exceeding OEVM_SPACE_MAX_PATH (64) reaching the C
   substrate via out: drives the kernel->C length cap end-to-end (test_space property 8
   covers only the C side). Build a > 64-char single-segment path and post to it. *)
class PathTooLong packageUsing: manifest = () (
public main: platform args: args = (
  | space long |
  space:: platform space.
  long:: 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'. (* 72 a's *)
  space out: long value: 1.
  'unreached' out.
)
) : (
)
```

Note: confirm 72 chars exceeds the `-D`-configured `OEVM_SPACE_MAX_PATH` used by the test build (default 64). Confirm the path is a single canonical segment (no `/`, no empty/dot segments) so it passes `checkPath:` and reaches `space_intern`, where the length cap aborts. If `out:` validates length in nsl before reaching C, the abort string still matches (the constraint is that the process exits with `channel path too long` on stderr).

- [ ] **Step 2: Run both paths** → PASS (runtime-fail matched). **Step 3: Commit** (in `/home/david/workspace/nsl-tests`).

---

## Task 10: Soak/leak plateau + docs + milestone close

**Files:**
- Test: `/home/david/workspace/nsl-tests/11_supervision/09_soak_leak.ns` (slow; milestone-gate only)
- Modify: `/home/david/workspace/tuvm/review/tests/CLAIMS.md` (flip T-D rows `pending D` → DONE; point at landed paths)
- Modify: `/home/david/workspace/tuvm/review/test-suites-spec.md` (ASSUMED API: `#error` → `#text`; note `%sys/*` → `sys/*`)
- Modify: `/home/david/workspace/tuvm/review/lib/timers.ns` (respell `%sys/timer` → `sys/timer` in the wiring/SYNC NOTE)
- Modify: `/home/david/workspace/tuvm/review/lib/exceptions.md` (respell `%sys/crash` → `sys/crash`)
- Modify: `/home/david/workspace/tuvm/review/program-sequence.md` (Milestone D → DONE at the boundary; phase-1 complete)
- Create: `/home/david/workspace/tuvm/docs/MILESTONE_D.md` (close-out: what landed, footprint delta, gate results — mirror `MILESTONE_A.md`)

**Interfaces:** Consumes all prior tasks. The soak test is the standing leak detector (shared-heap tradeoff — a leaky job can't be evicted by restart, so creep must be caught in test).

- [ ] **Step 1: Write the soak test**

Create `/home/david/workspace/nsl-tests/11_supervision/09_soak_leak.ns`: ≥100k events through several handlers with periodic deliberate crashes, sampling a heap-usage proxy each 10k; assert the sampled usage plateaus (bounded variance after warm-up), not creeps. Use the VM's heap-introspection selector if one exists (grep `kernel.ns` for a `platform`-level heap/free-bytes accessor — the footprint tooling implies one; `oevm.h:396` mentions a "bytes free" total). If no nsl-visible heap gauge exists, assert the weaker invariant: 100k events with periodic crashes complete and the node is still responsive (a final healthy event fires), and mark the test `skip` for the strict-plateau assertion with a comment pointing at the missing gauge. Mark the file slow (leave it in the suite; the runner has no per-file slow-skip, so keep the event count high enough to be meaningful but not so high it dominates the gate — e.g. 100k — and note it in `MILESTONE_D.md`).

- [ ] **Step 2: Run the soak test both paths** → PASS. Confirm the plateau (or the weaker invariant) holds.

- [ ] **Step 3: Update `CLAIMS.md`**

Flip the T-D rows (`~:16,17,20`) from `pending D` to `DONE`, and update the "Proven by" column to the landed `nsl-tests/11_supervision/*.ns` + `10_fsm/02_snapshot_restore.ns` paths, following the row-18 convention (cite the landed file, then the implementing kernel/C site: `nsl-tuvm/lib/kernel.ns` Supervisor/drain atop `tuvm/src/vm.c` `vm_protected_fire` + `tuvm/src/space.c`). Add rows for D.7 (`08_demo_node_integration.ns`), D.8 (`10_fsm/02_snapshot_restore.ns`), and D.9 (`09_channels/11_path_too_long.ns`).

- [ ] **Step 4: Reconcile the ASSUMED API + respell `%sys/*`**

In `test-suites-spec.md` (~`:126-130`), change the crash-record field `#error` to `#text` and note `#channel` remains; change `%sys/crash` → `sys/crash`. In `tuvm/review/lib/timers.ns` and `tuvm/review/lib/exceptions.md`, respell the `%sys/*` reserved-channel spellings to `sys/*` (matching the landed reality; Global Constraints).

- [ ] **Step 5: Update `program-sequence.md` and write `MILESTONE_D.md`**

Mark Milestone D DONE in the phase-1 list (`~"D — Supervisor + let-it-crash"`), note phase-1 (host proof) complete and phase-2 (exceptions-proper + Zephyr) is now entered. Write `tuvm/docs/MILESTONE_D.md` mirroring `MILESTONE_A.md`: what landed (crash boundary, supervisor, timers), the final gate line (`Passed/Failed/Skipped` + the four named gates), the footprint delta (run `footprint.sh` per addendum A2 — bank text + MCU-config RAM), and the closed carry-forwards (#5, #9.1-3).

- [ ] **Step 6: Final full-suite gate, both paths**

Run: `cd /home/david/workspace/nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py`
Expected: `Failed: 0`; `grant: PASS  debugger-integration: PASS  portability-32: PASS  boot-gc-asan: PASS`. Record the counts in `MILESTONE_D.md`.

- [ ] **Step 7: Commit (do NOT push — David merges/pushes on his word)**

```bash
cd /home/david/workspace/tuvm && git add -A && git commit -m "docs: close Milestone D — CLAIMS/spec/program-sequence + MILESTONE_D.md; respell %sys/*→sys/*

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
cd /home/david/workspace/nsl-tests && git add -A && git commit -m "test(supervision): soak/leak plateau (D.5, slow)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01321g4tUk8eSfN4YT7Vgx5A"
```

---

## Self-Review (completed by the plan author)

**Spec coverage** — every spec section maps to a task: §2 crash boundary → T1; §3 crash record → T2 (Step 4); §4 supervisor loop → T2 (protected drain) + T5 (policy); §5 restart+quarantine → T5; §6 owner-tag identity → T4 + T5 (sweep by `==` rootCap); §7 timers + `%sys→sys` respell → T6 + T10; §8 demo-node integration → T7; §9 GC⋂longjmp → T1 (root/scalars) + T3 (D.2 ASAN); §10 tests D.1–D.9 → T2,T3,T5,T6,T7,T8,T9,T10; §11 non-goals — no task builds them (correct). Carry-forwards: #5 → T1; #9.1 → T8; #9.2 → T4/T5; #9.3 → T9.

**Deviations from the spec, flagged for David (see handoff):** (a) crash-record fields are `#text`+`#channel` now; `#job`/`#state` forward-reserved (a capability can't be a value/record field); the supervisor resolves the job by prefix-matching `#channel`. (b) crash observation via a `sys/crash` reaction rather than `take:` (drain would drop the transient records) + a `sys/`-loop guard. (c) quarantine counter resets on job *progress* (an ordinary event), the reactor-native reading of "reset on clean firing". None change the milestone's observable contract; all are refinements the C reality forced.

**Placeholder scan:** the two most code-heavy novel tasks (T1, T2, T4, T5, T6) carry full C and nsl bodies. T7's integration body is described structurally against `demo-node.ns` rather than transcribed line-for-line — deliberately, because its exact `expect:` stream is only knowable by running it; the invariants to assert are enumerated. Several tests carry a "pin the deterministic count once observed" instruction — this is correct TDD for order-sensitive reactor output, not a placeholder (the assertion criteria are explicit; only the exact integer is deferred to observation, and both paths must agree).

**Type/name consistency:** prim ids 83–89 are assigned once, append-only, verified-by-counting; `vm_protected_fire`/`vm_raise`/`g_raised_condition` are named identically across T1's C, `oevm.h`, and `gc.c`; `spaceDropReactionsOwnedBy:`/`space_drop_reactions_owned_by` paired in T4; `Supervisor`/`SupervisedJob`/`register:restart:progress:`/`isQuarantined:` consistent between T5's code and the T5/T7 tests. Selector-existence checks (`detect:ifNone:`, `startsWith:`, `Record` field read, `SMI_VAL`, `NIL_OOP`, `outError`) are called out at each use site to grep-confirm before relying on them — the honest response to writing against a codebase the implementer must verify.
