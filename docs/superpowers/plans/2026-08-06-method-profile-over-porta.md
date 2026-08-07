# Per-method profiling over porta — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let David trigger a bounded per-method profile on a field node from porta, and see the named result in a browser.

**Architecture:** The space carries **one control line in** (`dbg/req`, already inbound-authorized); the console carries **the bulk out** (`mprof: <offset> <count>` lines, already teed over Thread and already durable). C does the dumping, so nsl never touches the counter table and the confined mirror is not involved. Naming happens off-node, in the viewer, by joining offsets against the `.oimg.methods.json` sidecar keyed by build id.

**Tech Stack:** C (the VM), nsl (the kernel seam and the bridge), Python 3 (the console collector, the resolver, the htmx view), Go (porta, read-only for this rung).

**Design of record:** `docs/design/2026-08-06-method-profile-over-porta-design.md` (D1–D10).

## Global Constraints

- **Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file.** Python files in this tree use `# Copyright (c) 2026 Ekorau LLC`.
- **Prim ids are append-only, and a new id is arrived at by COUNTING `PRIM_NAMES` in `nodus/tuvm/src/dispatch.c`** — never by arithmetic on the previous `#define`. This is a standing lesson recorded repeatedly in `nodus/tuvm/src/prim_ids.h`; an off-by-one silently rebinds an existing selector.
- **`OEVM_METHOD_PROFILE` stays off by default.** Every C change in this plan lives inside `#ifdef OEVM_METHOD_PROFILE` so a production build is byte-identical to today.
- **All timestamps and durations on the wire are milliseconds** (design D4.2).
- **Never run two nsl suites concurrently** — they share `/tmp` paths. **This extends to anything else that builds or runs the VM**, including a review agent verifying a claim: the suite rewrites `src/solidified_image.c` per test and restores it afterwards, so a concurrent build sees a half-written file. Observed 2026-08-06 — a suite run alongside a reviewer died at `12_exceptions/10` with exit 1 and no failure message, which reads exactly like a real regression.
- **The nsl suite prints `Passed: N / Failed: 0` AND a line of aux gates.** Read the aux line; it can be red while the count reads zero failures. `footprint: SKIP (no arm-zephyr-eabi toolchain)` is pre-existing and expected.
- **nsl test convention:** inline `(* expect: ... *)` block at the TOP of the file, a class with `main: platform args:`, output via `println`. There are no `.expected` files. Single-test inner loop: `python3 nodus/nsl/tools/run-one.py <file.nsl>`.
- **nsl has no modulo and no `startsWith:`.** Byte-compare over `size`/`at:` — `Kernel>>path:startsWith:` (`nodus/nsl/lib/kernel.nsl:1735`) is the existing helper; reuse it, do not write a second one.
- **`gofmt -l .` must print nothing** before any commit touching Go.

### Gates

Run before every commit that touches the VM, the kernel or the bridge:

```
cd nodus/nsl && python3 tools/run_tests_oevm.py     # expect: Passed: 246+ / Failed: 0, aux line green
cd porta      && go test ./...
cd nodus/tuvm && bash tests/porta_sync_host.sh      # expect: PORTA SYNC OK
```

---

### Task 1: Measure table capacity under a real bridged node

This is a **measurement, not a code change**, and it comes first because design D9 defers a decision to it: whether the 256-slot table holds a real app. Everything downstream is unaffected by the answer, so this does not block Tasks 2–8 — but the answer must exist before anyone argues about `OEVM_MPROF_SLOTS` or Space-Saving displacement (D10).

**It depends on nothing else in this plan.** The existing `OEVM_MPROF_RUNTIME_GATE` / `FLIP_AT` / `DUMP_AT` seam already produces a dump today.

**Files:**
- Create: `docs/results/2026-08-06-mprof-capacity-under-bridge.md`
- Read only: `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl`, `nodus/tuvm/zephyr/CMakeLists.txt:250-262`

**Interfaces:**
- Consumes: nothing.
- Produces: two numbers — `methods=<N>` and `overflow=<M>` — cited by Task 3's commit message and by design D9/D10.

- [ ] **Step 1: Confirm which board is on the cable**

Never assume last session's hardware. Identify by MAC, not by `ttyUSB` number:

```bash
esptool --port /dev/ttyUSB0 read_mac
```

The spike ran on **c6spare** (JTAG `40:4C:CA:44:6E:60`), the disposable board and current `TARGET_DEFAULT_BOARD`. It was left running `MethodProfileNodeC6` and must be reflashed before use. If the hash matches neither `c6` nor `c6bench` nor `c6spare`, stop — that is a fourth board and the registry is wrong.

- [ ] **Step 2: Build `PortaBridgeNodeC6` with the profiler and the runtime gate**

`DUMP_AT` must be generous. A 50k-activation period produced 1.9M log lines on a bridge in the spike and would drown the #56 console collector.

**`run.sh` does not source the west env** — it calls `west build` directly, so the env must already be live. And its flags are `--node <file>.nsl --board esp32c6`; there is no `--app`.

```bash
source ~/zephyr-esp32/.venv/bin/activate
source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd nodus/tuvm/zephyr
export OEVM_METHOD_PROFILE=1
export OEVM_MPROF_RUNTIME_GATE=1
export OEVM_MPROF_FLIP_AT=1000000
export OEVM_MPROF_DUMP_AT=20000000
bash run.sh --flash --node PortaBridgeNodeC6.nsl --board esp32c6
```

**Do not pass `--capture`.** A `collect.py` console collector already holds c6spare's port under systemd, and a second handle splits the stream — `logstore.acquire_lock` exists precisely because two readers on one board were proven to interleave into one file on 2026-08-04. Read the console through `logread.py` instead.

**Confirm `~/zephyr-esp32/zephyr` is on branch `tuvm-c6-radio-fix`** before building. A west re-fetch reverts it, and C6 Thread regresses silently without it.

- [ ] **Step 3: Let it run through at least two dumps, and read the header**

The dump re-arms, so a second window confirms the first was not a warmup artifact. Read the header line, not the rows:

```
mprof[dump]: methods=<N> activations=<A> overflow=<M> cap=256
```

- [ ] **Step 4: Record the result**

Write `docs/results/2026-08-06-mprof-capacity-under-bridge.md` stating: the board, the build, `methods=`, `overflow=`, both dump windows, and which of D9's three branches the number selects:

- `methods` comfortably under ~180 and `overflow=0` → **256 slots is fine**; top-N is a display choice only (Task 2 Step 5 already implements it). No counter change.
- `overflow > 0`, or `methods` above ~180 (the ≤0.7 load factor the code warns about at `nodus/tuvm/src/vm.c:388`) → the counter needs work. Record the number and stop; the choice between a 512-slot table and Space-Saving displacement (D10) is David's, and it is out of scope for this plan.

State the load factor explicitly: `methods / 256`.

- [ ] **Step 5: Commit**

```bash
git add docs/results/2026-08-06-mprof-capacity-under-bridge.md
git commit -m "spike(#165): the table under a real bridge holds N methods, and D9 can be decided"
```

---

### Task 2: Make the resolver honest about overflow, and add top-N

Pure Python, no hardware, no VM build. Design D9 names two defects: the percentage denominator excludes overflowed activations (so every `%` is inflated), and the `INVALID` banner overstates what is wrong — the counts themselves are exact.

**Files:**
- Modify: `nodus/tuvm/tools/mprof_resolve.py`
- Create: `nodus/tuvm/tools/test_mprof_resolve.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `mprof_resolve.py` gains `--top N` (int, default 0 meaning "all"). `resolve(mmap, text, top=0) -> list[tuple[str, int, float]]` returning `(name, count, percent)` rows sorted descending by count — extracted from `main()` so Task 7's view can import it instead of shelling out.

- [ ] **Step 1: Write the failing test**

Create `nodus/tuvm/tools/test_mprof_resolve.py`:

```python
# Copyright (c) 2026 Ekorau LLC
import json
import mprof_resolve


MAP = {"16": {"id": "m16", "qual": ["Cbor>>u8At:"]},
       "32": {"id": "m32", "qual": ["Cbor>>encode:"]}}

# activations=300 counts only the SLOTTED rows; overflow=100 activations were
# dropped. The true denominator is 400, so u8At: is 50%, not 66.7%.
DUMP = ("mprof[dump]: methods=2 activations=300 overflow=100 cap=256\n"
        "mprof: 16 200\n"
        "mprof: 32 100\n")


def test_percent_denominator_includes_overflow():
    rows = mprof_resolve.resolve(MAP, DUMP)
    assert rows[0] == ("Cbor>>u8At:", 200, 50.0)
    assert rows[1] == ("Cbor>>encode:", 100, 25.0)


def test_percent_without_overflow_uses_activations():
    clean = ("mprof[dump]: methods=2 activations=300 overflow=0 cap=256\n"
             "mprof: 16 200\n"
             "mprof: 32 100\n")
    rows = mprof_resolve.resolve(MAP, clean)
    assert rows[0][2] == 200 / 300 * 100


def test_top_n_truncates_but_keeps_the_denominator():
    rows = mprof_resolve.resolve(MAP, DUMP, top=1)
    assert len(rows) == 1
    assert rows[0] == ("Cbor>>u8At:", 200, 50.0)


def test_unresolved_offset_keeps_the_offset_and_is_not_dropped():
    rows = mprof_resolve.resolve({}, DUMP)
    assert len(rows) == 2
    assert "16" in rows[0][0]
    assert "UNKNOWN" in rows[0][0]
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/tuvm/tools && python3 -m pytest test_mprof_resolve.py -v
```

Expected: FAIL with `AttributeError: module 'mprof_resolve' has no attribute 'resolve'`.

- [ ] **Step 3: Extract `resolve()` and fix the denominator**

In `nodus/tuvm/tools/mprof_resolve.py`, add above `main()`:

```python
def resolve(mmap, text, top=0):
    """Join `mprof:` rows against the sidecar map and return
    [(name, count, percent), ...] sorted by count descending.

    The denominator is activations + overflow, NOT the header's activations.
    A saturated table drops activations entirely: they are counted in aggregate
    by `overflow` but attributed to no slot, so dividing by the slotted sum
    alone inflates every percentage. Adding overflow back makes each percentage
    a correct LOWER BOUND, which is a true statement rather than a flattering
    one. `top` truncates the returned rows and never the denominator.
    """
    header = None
    for m in HDR_RE.finditer(text):
        header = m.groups()                # last window wins
    rows = [(k, int(n)) for k, n in DUMP_RE.findall(text)]

    overflow = int(header[3]) if header else 0
    total = sum(n for _, n in rows) + overflow or 1

    out = []
    for key, n in sorted(rows, key=lambda r: -r[1]):
        if key.startswith("@"):
            name = f"UNKNOWN (not in the image blob: {key})"
        else:
            entry = mmap.get(key)
            name = name_of(entry) if entry is not None else \
                f"UNKNOWN (offset {key} absent from the map — wrong build?)"
        out.append((name, n, 100.0 * n / total))
    return out[:top] if top else out
```

- [ ] **Step 4: Rewrite `main()` to use it, and downgrade the banner**

Replace the body of `main()` from `header = None` through the final `return 0` with:

```python
    header = None
    for m in HDR_RE.finditer(text):
        header = m.groups()
    if not DUMP_RE.findall(text):
        print("no 'mprof:' lines found — was the image built with "
              "-DOEVM_METHOD_PROFILE and did the dump fire?", file=sys.stderr)
        return 1

    top = 0
    if "--top" in argv:
        top = int(argv[argv.index("--top") + 1])

    if header:
        why, methods, acts, overflow, cap = header
        print(f"window={why} methods={methods} activations={acts} cap={cap}")
        if int(overflow):
            # Not INVALID: the slotted counts are EXACT. A method already in the
            # table always increments correctly; only a NEW method arriving at a
            # full table is dropped. What overflow costs is completeness — some
            # methods are missing entirely — and it is already paid for in the
            # denominator above, which makes each percent a lower bound.
            print(f"INCOMPLETE: overflow={overflow} activations were dropped — "
                  f"the table saturated, so methods are MISSING from this table. "
                  f"Counts below are exact; percentages are lower bounds.")
        rows_len = len(DUMP_RE.findall(text))
        if rows_len != int(methods):
            print(f"WARNING: {rows_len} rows for methods={methods} — the log is "
                  f"truncated or holds more than one window.")

    resolved = resolve(mmap, text, top=top)
    unknown = sum(1 for name, _, _ in resolved if name.startswith("UNKNOWN"))
    print(f"\n{'count':>12}  {'%':>6}  method")
    for name, n, pct in resolved:
        print(f"{n:>12}  {pct:>5.1f}%  {name}")
    if top:
        print(f"\nshowing top {top} of {len(resolve(mmap, text))} rows — "
              f"percentages are of ALL activations, not of the shown rows.")
    if unknown:
        print(f"\n{unknown} row(s) unresolved — a map from a DIFFERENT build "
              f"resolves nothing and looks identical to a quiet program.")
    return 0
```

Update the module docstring's last paragraph: replace the sentence beginning "Likewise a non-zero `overflow` is reported as INVALID" with:

```
A non-zero `overflow` is reported as INCOMPLETE, not INVALID: the slotted
counts are exact, but methods are missing and the percentages are lower
bounds (the denominator includes the dropped activations).
```

Also add `--top N` to the usage line at the top of the docstring:

```
    mprof_resolve.py <image.oimg.methods.json> [--top N] [dump.log ...]
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd nodus/tuvm/tools && python3 -m pytest test_mprof_resolve.py -v
```

Expected: 4 passed.

- [ ] **Step 6: Verify the CLI still works end to end on the spike's own log**

The spike's dump is the only real fixture available before Task 1 lands. Any `mprof:`-bearing log will do:

```bash
cd nodus/tuvm/tools
python3 mprof_resolve.py --help 2>&1 | head -5
```

Expected: the docstring, including the `--top N` usage line.

- [ ] **Step 7: Commit**

```bash
git add nodus/tuvm/tools/mprof_resolve.py nodus/tuvm/tools/test_mprof_resolve.py
git commit -m "fix(#165): a saturated table understates nothing but its own completeness

The denominator excluded the dropped activations, so every percentage was
inflated by exactly the overflow. Adding it back makes each percent a correct
lower bound. INVALID becomes INCOMPLETE, because the slotted counts were always
exact — a method already in the table increments correctly, and only a NEW
method at a full table is lost.

Adds --top N and extracts resolve() so the view can import it rather than
shelling out."
```

---

### Task 3: Promote the runtime gate from spike seam to shipping control

Today `g_mprof_enabled` exists **only** under `OEVM_MPROF_RUNTIME_GATE`, and when that macro is absent `mprof_bump` counts unconditionally. Design D1 needs the flag to be a permanent part of `OEVM_METHOD_PROFILE`, defaulting to **off**, with the activation countdown remaining as a separate opt-in measurement seam.

**Do this task after Task 1**, which uses the countdown seam in its current form.

**Files:**
- Modify: `nodus/tuvm/src/vm.c:383-464` (the `OEVM_METHOD_PROFILE` block)
- Test: `nodus/tuvm/tests/test_mprof_gate.c` (create)
- Modify: `nodus/tuvm/tests/CMakeLists.txt` or the C-unit-test list the suite drives (see Step 4)

**Interfaces:**
- Consumes: nothing.
- Produces: three C entry points, declared in `nodus/tuvm/src/oevm.h` inside `#ifdef OEVM_METHOD_PROFILE`:
  - `void mprof_start(void);` — zero the table, zero overflow, set `g_mprof_enabled = 1`
  - `void mprof_stop(void);` — set `g_mprof_enabled = 0`, then `mprof_report("window")`
  - `int mprof_is_running(void);` — answers `g_mprof_enabled`

- [ ] **Step 1: Write the failing C unit test**

Create `nodus/tuvm/tests/test_mprof_gate.c`:

```c
/* Copyright (c) 2026 Ekorau LLC */
/* #165: the runtime gate is a shipping control, not a spike seam. Counting must
   be OFF until mprof_start(), and mprof_start() must zero a table left over from
   a previous window — a window that inherits the last one's counts reports a
   sum that never happened. */
#include <assert.h>
#include <stdio.h>
#include "oevm.h"

extern void mprof_bump_for_test(const void *m);   /* test-only shim, Step 3 */
extern unsigned mprof_used_for_test(void);

int main(void) {
    static char m1, m2;

    /* 1. Counting is off by default. */
    assert(!mprof_is_running());
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 0);

    /* 2. start() enables it. */
    mprof_start();
    assert(mprof_is_running());
    mprof_bump_for_test(&m1);
    mprof_bump_for_test(&m2);
    assert(mprof_used_for_test() == 2);

    /* 3. stop() disables it, and further activations are not counted. */
    mprof_stop();
    assert(!mprof_is_running());
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 2);

    /* 4. A second start() ZEROES the table rather than accumulating. */
    mprof_start();
    assert(mprof_used_for_test() == 0);
    mprof_bump_for_test(&m1);
    assert(mprof_used_for_test() == 1);
    mprof_stop();

    printf("test_mprof_gate: PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/tuvm
gcc -Wall -std=c99 -Isrc -DOEVM_METHOD_PROFILE \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c \
    src/backend/posix/*.c src/oimg_loader.c src/solidified_image.c \
    tests/test_mprof_gate.c -o /tmp/test_mprof_gate -lm 2>&1 | head -5
```

Expected: FAIL at link with `undefined reference to 'mprof_start'`.

**Do not shortcut this to `src/*.c`.** That glob pulls in both `src/kernel_boot.c` and
`src/solidified_image.c`, whose static `Vtable_*` definitions collide — the link then fails
for a reason that has nothing to do with the test. The list above is exactly the one
`run_c_unit_tests` in `nodus/nsl/tools/run_tests_oevm.py` uses, and for the same reason.

- [ ] **Step 3: Restructure the gate in `nodus/tuvm/src/vm.c`**

Move `g_mprof_enabled` out of the `OEVM_MPROF_RUNTIME_GATE` block so it exists whenever `OEVM_METHOD_PROFILE` does. Immediately after the `g_mprof_used` declaration (`vm.c:394`), add:

```c
/* #165 D1: the shipping control. The spike's OEVM_MPROF_RUNTIME_GATE flipped
   this on an activation countdown because Zephyr has no getenv and a count
   makes two measurement arms exactly reproducible. That seam stays below, opt
   in, for benchmarking. This flag is the one porta drives, and it defaults OFF:
   a node pays the 0.90% only inside a window it was asked for. */
static int g_mprof_enabled = 0;
```

Delete the `static int g_mprof_enabled = 0;` line inside the `#ifdef OEVM_MPROF_RUNTIME_GATE` block (`vm.c:420`) — the two would otherwise collide.

Change the head of `mprof_bump` so the enable check applies unconditionally:

```c
static OEVM_HOT void mprof_bump(const void *m) {
#ifdef OEVM_MPROF_RUNTIME_GATE
    if (--g_mprof_countdown == 0) mprof_phase_advance();
#endif
    if (!g_mprof_enabled) return;
```

Then add the three entry points and the two test shims after `mprof_report`:

```c
/* #165 D1/D6 lifecycle. start = zero + enable; stop = disable + report. Zeroing
   at START rather than at stop is what makes a window mean one window: the
   counts are cumulative by construction, so a second capture that inherited the
   first one's table would report a sum spanning both and no reader could tell. */
void mprof_start(void) {
    int i;
    for (i = 0; i < OEVM_MPROF_SLOTS; i++) { g_mprof_key[i] = NULL; g_mprof_n[i] = 0; }
    g_mprof_used = 0;
    g_mprof_overflow = 0;
    g_mprof_enabled = 1;
}

void mprof_stop(void) {
    g_mprof_enabled = 0;
    mprof_report("window");
}

int mprof_is_running(void) { return g_mprof_enabled; }

/* Test-only shims. mprof_bump is static and OEVM_HOT; exposing it directly
   would put a non-inlinable call on the activation path. */
void mprof_bump_for_test(const void *m) { mprof_bump(m); }
unsigned mprof_used_for_test(void) { return g_mprof_used; }
```

In `nodus/tuvm/src/oevm.h`, beside the existing confined-prim declarations (near line 379), add:

```c
/* #165: per-method profile window control. Present only in a profile build. */
#ifdef OEVM_METHOD_PROFILE
void mprof_start(void);
void mprof_stop(void);
int  mprof_is_running(void);
#endif
```

- [ ] **Step 4: Register the test with the suite**

The nsl suite drives a set of C unit tests and prints each as a named aux gate (`C unit test test_debugger: PASSED`). Find the list that names `test_debugger` and append `test_mprof_gate` beside it:

```bash
rg -n "test_debugger" nodus/nsl/tools/run_tests_oevm.py
```

Add the new test the same way, compiling with `-DOEVM_METHOD_PROFILE`. A C unit test that is never run is worse than no test, because the suite still reads green.

- [ ] **Step 5: Run the test and the full suite**

```bash
cd nodus/tuvm && gcc -Wall -std=c99 -Isrc -DOEVM_METHOD_PROFILE \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c \
    src/backend/posix/*.c src/oimg_loader.c src/solidified_image.c \
    tests/test_mprof_gate.c -o /tmp/test_mprof_gate -lm && /tmp/test_mprof_gate
cd nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: `test_mprof_gate: PASSED`, then `Passed: 246+ / Failed: 0` with the aux line green and `test_mprof_gate` visible in it.

- [ ] **Step 6: Commit**

```bash
git add nodus/tuvm/src/vm.c nodus/tuvm/src/oevm.h nodus/tuvm/tests/test_mprof_gate.c nodus/nsl/tools/run_tests_oevm.py
git commit -m "feat(#165): the profile gate stops being a spike seam and starts being a control

g_mprof_enabled lived inside OEVM_MPROF_RUNTIME_GATE, so a build without the
spike macro counted unconditionally. It is now part of OEVM_METHOD_PROFILE and
defaults OFF; the activation countdown stays behind its own macro as the
benchmarking seam it was written to be.

start() zeroes before enabling. A window inheriting the previous window's table
reports a sum spanning both, and nothing in the output would say so."
```

---

### Task 4: Expose start/stop as two platform prims and a Kernel seam

**Files:**
- Modify: `nodus/tuvm/src/prim_ids.h` (append two ids)
- Modify: `nodus/tuvm/src/dispatch.c` (append two `PRIM_NAMES` entries, in the same order)
- Modify: `nodus/tuvm/src/platform.c` (two cases in `platform_dispatch`)
- Modify: `nodus/nsl/lib/kernel.nsl` (two Kernel methods, beside `heapStats` at line 3192)
- Test: `nodus/tests/23_mprof/01_window_lifecycle.nsl` (create)

**Interfaces:**
- Consumes: `mprof_start()`, `mprof_stop()`, `mprof_is_running()` from Task 3.
- Produces: two nsl selectors on the Kernel, reachable from any app as `platform`:
  - `platform profileStart` → answers `self` (the Kernel), so it chains
  - `platform profileStop` → answers `self`
  - Both are no-ops answering `self` in a build without `OEVM_METHOD_PROFILE`, so an app is portable across profile and production builds.

- [ ] **Step 1: Write the failing nsl test**

Create `nodus/tests/23_mprof/01_window_lifecycle.nsl`:

```
(* expect:
start ok
stop ok
*)
(* #165: the window control is reachable from an ordinary app's platform seam.
   This asserts REACHABILITY and idempotence, not counts — the counts are a C
   unit test (test_mprof_gate). On a build without OEVM_METHOD_PROFILE both
   sends are no-ops that still answer self, which is what makes an app portable
   between a profile build and a production one. *)
class MprofWindowTest = () (
  public main: platform args: args = (
    platform profileStart.
    'start ok' println.
    platform profileStop.
    'stop ok' println.
    ^ 0 )
) : ()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/01_window_lifecycle.nsl
```

Expected: FAIL — a doesNotUnderstand on `profileStart`. Note that a **SKIP proves nothing** and a **CRASH discards every line already printed**; if you see either, fix that before continuing.

- [ ] **Step 3: Append the two prim ids**

**Count `PRIM_NAMES` in `nodus/tuvm/src/dispatch.c` first.** Do not derive the number from the previous `#define`:

```bash
rg -n "PRIM_NAMES\[\]" -A 200 nodus/tuvm/src/dispatch.c | rg -o '"[^"]*"' | wc -l
```

Call that count `C`. The two new ids are `C+1` and `C+2`. Append to `nodus/tuvm/src/prim_ids.h`:

```c
/* #165 Task 4: per-method profile window control (`profileStart`/`profileStop`),
   appended after heapStats — no pre-existing id shifts. The two ids were arrived
   at by COUNTING PRIM_NAMES, not by arithmetic on the previous #define, per the
   standing lesson above. Platform-dispatched. Both are no-ops in a build without
   OEVM_METHOD_PROFILE so an app runs unchanged on a production image. */
#define PRIM_pf_profileStart <C+1>
#define PRIM_pf_profileStop  <C+2>
```

- [ ] **Step 4: Append the names in the SAME order**

At the end of the `PRIM_NAMES[]` table in `nodus/tuvm/src/dispatch.c`, append exactly:

```c
    "profileStart", "profileStop",
```

The enum and this table are positionally 1:1. Appending to one and not the other silently rebinds every id after the gap.

- [ ] **Step 5: Service them in `platform_dispatch`**

In `nodus/tuvm/src/platform.c`, beside `case PRIM_pf_heapStats:` (line 1180), add:

```c
    case PRIM_pf_profileStart: {
        /* #165 D1/D6: open a capture window. No-op without the profiler built in
           — deliberately silent rather than an error, so one app image runs on
           both a profile build and a production build. The absence is visible
           where it matters: no `mprof:` lines ever appear. */
#ifdef OEVM_METHOD_PROFILE
        mprof_start();
#endif
        return receiver;
    }
    case PRIM_pf_profileStop: {
        /* Closes the window and dumps. The dump goes to stderr, which reaches
           the #56 collector because the console is TEED, not because anything
           here routes it. */
#ifdef OEVM_METHOD_PROFILE
        mprof_stop();
#endif
        return receiver;
    }
```

- [ ] **Step 6: Add the Kernel seam**

In `nodus/nsl/lib/kernel.nsl`, immediately after `public heapStats` (line 3192), add:

```
  (* #165: open and close a per-method profile window. The counter is a build
     option (OEVM_METHOD_PROFILE) and these are no-ops without it, so an app may
     send them unconditionally. The rows do NOT come back through here — they go
     to the console, which is already teed off the board (design D4). *)
  public profileStart      = ( platform profileStart. ^ self )
  public profileStop       = ( platform profileStop. ^ self )
```

- [ ] **Step 7: Run the test and the full suite**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/01_window_lifecycle.nsl
cd nodus/nsl && python3 tools/run_tests_oevm.py
```

Expected: the single test PASSes, then `Passed: 247+ / Failed: 0` with the aux line green.

- [ ] **Step 8: Commit**

```bash
git add nodus/tuvm/src/prim_ids.h nodus/tuvm/src/dispatch.c nodus/tuvm/src/platform.c nodus/nsl/lib/kernel.nsl nodus/tests/23_mprof/01_window_lifecycle.nsl
git commit -m "feat(#165): an app can open a profile window, and cannot read the counts

Two platform prims and two Kernel selectors. Deliberately write-only: nsl opens
and closes the window and never reads the table, so the confined mirror is not
involved and supervisor.nsl stays the sole grant holder (design D5).

No-ops without OEVM_METHOD_PROFILE, so one app image runs on a profile build and
a production build alike."
```

---

### Task 5: Route a `dbg/req` line to the window, self-limiting by duration

This is `dbg/*`'s **first implementation since v1 was deleted**. RECORDS §4.5 has been pinned-but-unbuilt since the rewrite.

The bridge already *accepts* `dbg/req` — `Kernel>>mayApply:` (`nodus/nsl/lib/kernel.nsl:1729`) returns true for it — but nothing consumes it. `applyEntry` currently routes every accepted path to `space put:` / `space out:` (`kernel.nsl:1719-1720`).

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl` — add ONE pure parsing method to the Kernel, beside `profileStart`
- Modify: `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl` — the channel wiring, the window start, and the timer that ends it
- Test: `nodus/tests/23_mprof/02_dbg_req_profile.nsl` (create)

**Interfaces:**
- Consumes: `platform profileStart` / `platform profileStop` from Task 4.
- Produces: `Kernel>>parseProfileMs: line` — a **pure function** answering the window length in ms for a well-formed `profile <ms>` line, or `0` for anything else. Nothing else in the plan depends on it, but keeping it pure is what makes it testable without constructing a bridge.

**Two corrections to what an earlier draft of this task said. Both are load-bearing.**

**1. There is no `applyEntry` change to make.** `Kernel>>mayApply:` (`nodus/nsl/lib/kernel.nsl:1729`) already returns true for `dbg/req`, and `dbg/req` is a **stream** (RECORDS §4.5), so `applyEntry` already delivers it with `space out:`. An app therefore just reacts to it, exactly the way `PortaBridgeNodeC6.nsl:109` already reacts to `goal/name`:

```
(space channel: 'goal/name') whenever: '' do: [ :v | self nameChanged: v ].
```

Leave the kernel's inbound path alone. It already does the right thing.

**2. Do NOT create a second `Timers` instance.** `Timers` allocates from `nextToken ::= 1` **per instance**, but the C seam's timer registry is keyed **globally** by token — so a second instance would hand out token 1 while the app's repeating cadence timer (`PortaBridgeNodeC6.nsl:119`) already holds token 1, and the two would alias. `Timers>>dispatch:` drops unknown tokens silently by design, so this would not crash; it would misfire quietly, which is worse.

The app already holds the one legitimate instance (`timers:: platform timers` at line 115, with `space whenever: 'sys/timer' do: [ :e | timers dispatch: e ]` at line 116). **Use that one.** This is also why the orchestration lives in the app rather than in the kernel's `PortaBridge`.

- [ ] **Step 1: Write the failing nsl test**

Create `nodus/tests/23_mprof/02_dbg_req_profile.nsl`:

```
(* expect:
profile 5000 -> 5000
profile 250 -> 250
profile -> 0
reboot now -> 0
profile 0 -> 0
profile abc -> 0
*)
(* #165 D2/D6: a dbg/req line opens a self-limiting window. Parsing is a PURE
   function on the Kernel so it can be tested without standing up a bridge; the
   orchestration that acts on it lives in the app, where the one legitimate
   Timers instance is.

   A zero, missing or non-numeric duration answers 0 and the caller does
   nothing. Refused rather than defaulted: a defaulted window length is an
   invented constant, and a zero-length one would enable counting and never
   stop it. *)
class ParseProfileMsTest = () (
  public main: platform args: args = (
    self report: 'profile 5000' for: (platform parseProfileMs: 'profile 5000').
    self report: 'profile 250'  for: (platform parseProfileMs: 'profile 250').
    self report: 'profile'      for: (platform parseProfileMs: 'profile').
    self report: 'reboot now'   for: (platform parseProfileMs: 'reboot now').
    self report: 'profile 0'    for: (platform parseProfileMs: 'profile 0').
    self report: 'profile abc'  for: (platform parseProfileMs: 'profile abc').
    ^ 0 )

  report: label for: ms = (
    (label, ' -> ', ms printString) println.
    ^ self )
) : ()
```

Note `profile abc` must answer 0, not the 0 that a digit-free parse would coincidentally produce for the right reason — write the parser so a token containing any non-digit is rejected outright rather than skipped over. The existing `Debugger>>parseInt:` in `nodus/nsl/lib/supervisor.nsl` *skips* non-digits, which would make `profile a5` answer 5. Do not copy that behaviour here.

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/02_dbg_req_profile.nsl
```

Expected: FAIL — doesNotUnderstand on `handleDbgReq:`.

- [ ] **Step 3: Add the pure parser to the Kernel**

In `nodus/nsl/lib/kernel.nsl`, beside the `profileStart` / `profileStop` accessors added in Task 4, add:

```
  (* #165 D2/D6: parse a dbg/req line into a window length in ms. PURE — it
     starts nothing and touches no state, so it is testable without a bridge and
     the app stays in charge of the window's lifecycle.

     Answers 0 for anything that is not a well-formed `profile <positive int>`:
     a wrong verb, a missing duration, a zero duration, or a duration with any
     non-digit in it. Refused rather than defaulted — a defaulted length is an
     invented constant, and a zero-length window would enable counting and never
     stop. The caller does nothing on 0, and the line still lands in the space,
     so a request that could not be understood is visible rather than eaten. *)
  public parseProfileMs: line = (
    | rs verb tok n i b |
    rs:: self ReadStream on: line.
    verb:: rs upTo: 32.
    verb = 'profile' ifFalse: [ ^ 0 ].
    tok:: rs upTo: 32.
    tok == nil ifTrue: [ ^ 0 ].
    tok size = 0 ifTrue: [ ^ 0 ].
    (* Every character must be a digit. supervisor.nsl's parseInt: SKIPS
       non-digits, which would read 'a5' as 5; that is wrong for a wire value. *)
    n:: 0.
    i:: 1.
    [ i <= tok size ] whileTrue: [
      b:: tok at: i.
      (b < 48 or: [ b > 57 ]) ifTrue: [ ^ 0 ].
      n:: (n * 10) + (b - 48).
      i:: i + 1 ].
    ^ n )
```

Check how the Kernel refers to its own `ReadStream` in the surrounding code before writing `self ReadStream on:` — other Kernel methods may use a different form. Match what is already there.

- [ ] **Step 3b: Wire the app (this is where the window actually runs)**

In `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl`, following the existing pattern at lines 109–119:

Add a slot for the end-of-window channel beside the existing `space bridge timers ticks` slots (line 38), then in the same setup method that already holds lines 109–119:

```
    (* #165 D2: react to porta's dbg/req. The kernel already accepts this path
       (mayApply:) and already delivers it as a stream (space out:), so nothing
       inbound changes — this is just a reader, exactly like the goal/name
       reaction above. *)
    (space channel: 'dbg/req') whenever: '' do: [ :v | self dbgReq: v ].

    (* D6: the end-of-window signal. Timers post a MESSAGE to a CHANNEL — there
       is no after:do: — so the window's end is an ordinary channel event. *)
    profChan:: space channel: 'sys/profileEnd'.
    profChan whenever: '' do: [ :m | self profileWindowEnd ].
```

and the three handlers:

```
  (* Each of these is a SELF-SEND from inside the whenever: block above, not
     inline work. A cross-class call inside a reified block cannot resolve its
     outer chain (rete#133), so the block must do nothing but send to self. *)
  dbgReq: line = (
    | ms |
    ms:: platform parseProfileMs: line.
    ms > 0 ifTrue: [ self profileWindowStart: ms ].
    ^ self )

  (* SELF-LIMITING (D6). The node stops on its own, so a node that never hears
     from porta again still stops counting and still reports. An explicit stop
     message would leave a field node paying the counter's cost forever the
     first time one was lost, with nobody aware.

     `timers` is the app's ONE instance (line 115). Do NOT mint a second: token
     allocation is per-instance but the C timer registry is keyed globally, so a
     second instance would alias this one's tokens and misfire silently. *)
  profileWindowStart: ms = (
    platform profileStart.
    timers after: ms post: #profileEnd on: profChan.
    ^ self )

  profileWindowEnd = (
    platform profileStop.
    ^ self )
```

- [ ] **Step 4: Confirm nothing in the kernel's inbound path needed changing**

Re-read `Kernel>>applyEntry` and `Kernel>>mayApply:` (`nodus/nsl/lib/kernel.nsl`, around lines 1715–1731) and confirm you did **not** modify them. `mayApply:` already returns true for `dbg/req` and `applyEntry` already delivers a stream entry with `space out:`, which is what the app's `whenever:` reacts to.

If you found yourself needing to change either, stop and report — it means the reaction is not receiving the value, and the fix is in the app's channel path, not in the kernel's authority check.

- [ ] **Step 5: Run the test and the full suite**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/02_dbg_req_profile.nsl
cd nodus/nsl && python3 tools/run_tests_oevm.py
cd nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: the test PASSes with all four lines, `Passed: 248+ / Failed: 0` aux green, and `PORTA SYNC OK`.

**The host suite is structurally blind to the ISR seam** — posix timers bypass the pulse mailbox — so a green run here says the parse and the wiring are right and says **nothing** about the window actually closing on a device. That is Task 6's hardware gate.

- [ ] **Step 6: Commit**

```bash
git add nodus/nsl/lib/kernel.nsl nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl nodus/tests/23_mprof/02_dbg_req_profile.nsl
git commit -m "feat(#165): dbg/req gets its first implementation since v1, and the window closes itself

porta was already authorized to write dbg/req (§3, and mayApply: has returned
true for it all along) — nothing consumed it. A 'profile <ms>' line now opens a
window that stops on its own.

Self-limiting rather than stop-on-request: a lost stop message would leave a
field node counting at 0.90% forever with nobody aware. An unparseable line is
NOT consumed and falls through to the space, because a silently eaten debug
request is indistinguishable from one that never arrived."
```

---

### Task 6: Hardware gate — a real window, triggered from porta

The host suite cannot witness this. Design D6's whole claim is about a device.

**Files:**
- Create: `docs/results/2026-08-06-mprof-window-hardware.md`
- Read only: `nodus/tuvm/tests/target/run.sh`, `nodus/tuvm/zephyr/tools/console/logread.py`

**Interfaces:**
- Consumes: Tasks 3–5.
- Produces: a witnessed `mprof[window]:` header in the console log, and the elapsed time between the request and the dump.

- [ ] **Step 1: Flash a profile build of `PortaBridgeNodeC6` to c6bench**

```bash
source ~/zephyr-esp32/.venv/bin/activate
source ~/zephyr-esp32/zephyr/zephyr-env.sh
cd nodus/tuvm/zephyr
export OEVM_METHOD_PROFILE=1
unset OEVM_MPROF_RUNTIME_GATE OEVM_MPROF_FLIP_AT OEVM_MPROF_DUMP_AT
bash run.sh --flash --node PortaBridgeNodeC6.nsl --board esp32c6
```

Unsetting the countdown macros is the point: this build must produce **no** `mprof:` lines until asked.

- [ ] **Step 2: Confirm silence before the request**

```bash
python3 nodus/tuvm/zephyr/tools/console/logread.py --since 2m | rg "mprof" | wc -l
```

Expected: `0`. **This is the control.** A gate that cannot distinguish "the window worked" from "it was already dumping" measures nothing — and a query that returns nothing because it was pointed at the wrong sink looks identical to a quiet node. Confirm the same command returns non-zero for an unrelated known-present pattern (e.g. `rg "checkin"`) before trusting the zero.

- [ ] **Step 3: Write `dbg/req` from porta**

```bash
porta put node/<id>/dbg/req 'profile 10000'
```

If `porta put` refuses a non-`goal/` path, read `porta/internal/portacli/` and use whatever write path the CLI exposes for `dbg/req`; the wire and the store have no verbs, so this is a path write and nothing more. If the CLI genuinely cannot address `dbg/req`, **stop and report it** — that is a missing capability, and design D2 assumes porta can write this path.

- [ ] **Step 4: Witness the dump, and time it**

```bash
python3 nodus/tuvm/zephyr/tools/console/logread.py --since 2m | rg "mprof\[window\]"
```

Expected: exactly one `mprof[window]: methods=… activations=… overflow=… cap=256` header, arriving ~10 s after the request, followed by the rows.

Assert all three, and record each:
1. **Zero `mprof:` lines before the request** (Step 2's control).
2. **Exactly one window header**, not several — a re-arming dump would mean the countdown seam is still compiled in.
3. **Elapsed ≈ 10 s**, matching the requested duration. A dump that arrives instantly means the timer never armed and `profileStop` ran inline.

- [ ] **Step 5: Resolve it, end to end**

```bash
python3 nodus/tuvm/tools/mprof_resolve.py \
  nodus/tuvm/zephyr/build/PortaBridgeNodeC6.oimg.methods.json \
  --top 20 \
  <(python3 nodus/tuvm/zephyr/tools/console/logread.py --since 2m)
```

Expected: 20 named rows, percentages summing to less than 100 (they are of *all* activations), and no `UNKNOWN` rows. Any `UNKNOWN` means the sidecar is from a different build than the image on the board — reflash and repeat rather than reading the table.

- [ ] **Step 6: Record the result and commit**

Write `docs/results/2026-08-06-mprof-window-hardware.md` with the board id, all three assertions and their measured values, the elapsed time, and the top-20 table.

```bash
git add docs/results/2026-08-06-mprof-window-hardware.md
git commit -m "test(#165): a field node profiles on request, and was silent before it was asked"
```

---

### Task 7: `platform buildId` — close #74

Design D3 makes this a hard dependency: the sidecar must be chosen by the build actually on the node, and in the field the nodes will not all be running the same build.

**Files:**
- Modify: `nodus/tuvm/src/prim_ids.h`, `nodus/tuvm/src/dispatch.c`, `nodus/tuvm/src/platform.c` (one more prim, same pattern as Task 4)
- Modify: `nodus/nsl/lib/kernel.nsl` (a `buildId` accessor)
- Modify: `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl:63`, `nodus/tuvm/zephyr/ShipsBellAlarmNodeC6.nsl:133` (add the field to `obs/build`)
- Test: `nodus/tests/23_mprof/03_build_id.nsl` (create)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `platform buildId` answering a String; `obs/build` gains a `#build` field carrying it.

- [ ] **Step 1: Do NOT invent an id — one already exists and is already baked in**

This step was open when the plan was written and is now closed by reading the code. **Invent nothing.**

`nodus/tuvm/zephyr/cmake/gen_build_id.cmake` already bakes `git describe --always --dirty` plus a build timestamp into a generated TU as:

```c
const char tuvm_build_id[] = "<describe>-<YYYYmmddTHHMMSS>";
```

It is regenerated on **every** build (the custom command lists an output it never creates, which forces the re-run), it is already `extern`'d at `nodus/tuvm/zephyr/src/main.c:1529`, and it is already printed at boot by `main.c:1803`. `run.sh` archives the ELF under the same string by reading the matching `.txt`, so the id already keys the symbol archive.

Confirmed live on c6spare 2026-08-06: `tuvm: build-id porta-v1-final-846-g4315fb0-dirty-20260806T204341`.

So the whole of #74 is **surfacing a string that already exists** to nsl.

**It is Zephyr-only.** The generated TU is added by the Zephyr `CMakeLists.txt`, so a host build has no such symbol and will not link against a bare `extern`. Provide a host value with a `#ifdef __ZEPHYR__` split rather than a weak symbol — the codebase already splits on that macro elsewhere, and a weak symbol that silently resolves to the wrong definition is exactly the failure mode this id exists to prevent:

```c
#ifdef __ZEPHYR__
extern const char tuvm_build_id[];
#define OEVM_BUILD_ID tuvm_build_id
#else
#define OEVM_BUILD_ID "host"
#endif
```

**Do not reach for a GNU build-id.** `nodus/tuvm/zephyr/CMakeLists.txt:54` already records why in full: any `SHT_NOTE` input section makes GNU ld synthesise a `PT_NOTE` spanning the whole rodata region, the Espressif image builder mishandles it, and the board boots and then emits garbage. That was tried on hardware and rejected (#144), with three-boot A/B/A attribution in the design doc §7.

Read `gh issue view 74` for anything else it asks for beyond this, but the scheme question is settled.

- [ ] **Step 2: Write the failing nsl test**

Create `nodus/tests/23_mprof/03_build_id.nsl`:

```
(* expect:
buildId is a String: true
buildId is non-empty: true
*)
(* #74/#165 D3: the node names its own build, so a reader can pick the matching
   .oimg.methods.json sidecar. A mismatched sidecar resolves nothing and looks
   identical to a quiet program — this is the field that removes the chance to
   get it wrong by hand. Asserts SHAPE, not the value: the value changes every
   build, and a test that pinned it would fail on every rebuild. *)
class BuildIdTest = () (
  public main: platform args: args = (
    | b |
    b:: platform buildId.
    ('buildId is a String: ', (b isString) printString) println.
    ('buildId is non-empty: ', (b size > 0) printString) println.
    ^ 0 )
) : ()
```

If `isString` does not exist in this dialect, assert on `b size > 0` and `(b at: 1) > 0` instead — check `nodus/nsl/lib/kernel.nsl` for the available predicates before writing the test rather than assuming.

- [ ] **Step 3: Run it to verify it fails**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/03_build_id.nsl
```

Expected: FAIL — doesNotUnderstand on `buildId`.

- [ ] **Step 4: Add the prim, counting `PRIM_NAMES` again**

Same three-file pattern as Task 4 Steps 3–5. **Re-count `PRIM_NAMES`** — it grew by two in Task 4, and reusing that task's arithmetic is exactly the mistake `prim_ids.h` warns about.

In `platform.c`:

```c
    case PRIM_pf_buildId: {
        /* #74: the node names its own build so a reader can pick the matching
           .oimg.methods.json sidecar. NOT a GNU build-id — a PT_NOTE segment
           over DROM produces a booting-but-garbled console on the C6 (#144). */
        extern const char *oevm_build_id(void);
        return string_from_cstr(oevm_build_id());
    }
```

Use whatever String constructor `platform.c` already uses elsewhere — grep for an existing `case` that returns a String and copy its exact call, rather than assuming `string_from_cstr` exists.

- [ ] **Step 5: Add the Kernel accessor and put it on `obs/build`**

In `nodus/nsl/lib/kernel.nsl`, beside `profileStart`:

```
  (* #74: the build actually running, so a reader can pick the matching sidecar. *)
  public buildId           = ( ^ platform buildId )
```

In both `nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl:63` and `nodus/tuvm/zephyr/ShipsBellAlarmNodeC6.nsl:133`, add `#build` beside the existing `#chip` field. Read the surrounding record construction and match it exactly — `obs/build` is a pinned record and its shape is documented in `protocol/PROTOCOL-NSL-RECORDS.md`.

- [ ] **Step 6: Update the record catalogue**

`obs/build` gaining a field is a **shape change to a document of record**. There is no backward compatibility to preserve — both ends are reflashed together — so edit `protocol/PROTOCOL-NSL-RECORDS.md`'s `obs/build` row **in place**, adding `build` beside `chip`. Do not add a versioned field or a second row.

- [ ] **Step 7: Run everything**

```bash
cd nodus/nsl && python3 tools/run-one.py tests/23_mprof/03_build_id.nsl
cd nodus/nsl && python3 tools/run_tests_oevm.py
cd porta      && go test ./...
cd nodus/tuvm && bash tests/porta_sync_host.sh
```

Expected: all green. `porta_sync_host.sh` prints `obs/build carries the node's provenance` — confirm it still passes with the new field, since it decodes that record.

- [ ] **Step 8: Commit**

```bash
git add nodus/tuvm/src/prim_ids.h nodus/tuvm/src/dispatch.c nodus/tuvm/src/platform.c nodus/nsl/lib/kernel.nsl nodus/tuvm/zephyr/PortaBridgeNodeC6.nsl nodus/tuvm/zephyr/ShipsBellAlarmNodeC6.nsl protocol/PROTOCOL-NSL-RECORDS.md nodus/tests/23_mprof/03_build_id.nsl
git commit -m "feat(#74): a node names its own build, so the sidecar cannot be picked wrong

obs/build carried #chip and nothing else, so joining a profile's offsets to
names meant choosing a .oimg.methods.json by hand — and a mismatched sidecar
resolves nothing while looking exactly like a quiet program.

RECORDS is edited in place rather than versioned: both ends are reflashed
together, so a shape change is an edit and a rebuild.

Closes #74."
```

---

### Task 8: The htmx view, served by the console collector

Design D8. The view sits on the data rather than the data moving to the view.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/view.py`
- Create: `nodus/tuvm/zephyr/tools/console/test_view.py`
- Modify: `nodus/tuvm/zephyr/tools/console/README.md`

**Interfaces:**
- Consumes: `logstore.LogStore` (`days()`, `path_for(day)`), `mprof_resolve.resolve()` from Task 2.
- Produces: an HTTP server with three routes — `GET /` (the table), `POST /capture` (trigger), `GET /health`.

- [ ] **Step 1: Write the failing test**

Create `nodus/tuvm/zephyr/tools/console/test_view.py`:

```python
# Copyright (c) 2026 Ekorau LLC
import view


LOG = ("2026-08-06T10:00:00.000-07:00 checkin ok\n"
       "2026-08-06T10:00:01.000-07:00 mprof[window]: methods=2 activations=300 overflow=0 cap=256\n"
       "2026-08-06T10:00:01.001-07:00 mprof: 16 200\n"
       "2026-08-06T10:00:01.002-07:00 mprof: 32 100\n")

MAP = {"16": {"id": "m16", "qual": ["Cbor>>u8At:"]},
       "32": {"id": "m32", "qual": ["Cbor>>encode:"]}}


def test_latest_window_is_extracted_from_surrounding_traffic():
    rows = view.latest_window(LOG, MAP, top=10)
    assert rows[0][0] == "Cbor>>u8At:"
    assert rows[0][1] == 200


def test_no_window_reports_absence_rather_than_an_empty_table():
    rows = view.latest_window("2026-08-06T10:00:00.000-07:00 checkin ok\n", MAP)
    assert rows is None


def test_render_marks_an_incomplete_window():
    html = view.render(view.latest_window(LOG, MAP), overflow=5)
    assert "INCOMPLETE" in html


def test_render_of_no_window_says_so():
    html = view.render(None, overflow=0)
    assert "no capture" in html.lower()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_view.py -v
```

Expected: FAIL with `ModuleNotFoundError: No module named 'view'`.

- [ ] **Step 3: Implement `view.py`**

```python
# Copyright (c) 2026 Ekorau LLC
"""#165 D8: the profile view, served next to the data.

The console collector already holds everything the view needs — the log history
(logstore), the offset->name join (mprof_resolve) and the symbolizer. Serving
from here rather than from porta's Go mux moves only the TRIGGER across the
seam, which is one small write; serving from porta would have moved the bulk,
and made the log file layout a contract.

Starting a capture is porta's, because porta owns the dbg/req write.
"""
import html as _html
import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import mprof_resolve                                          # noqa: E402

import logstore                                               # noqa: E402


def latest_window(text, mmap, top=0):
    """Rows of the LAST mprof window in `text`, or None if there is none.

    None rather than [] on purpose: an empty table and a node that was never
    asked to profile render identically otherwise, and the operator cannot tell
    'it reported nothing' from 'it was never asked'.
    """
    if not mprof_resolve.DUMP_RE.search(text):
        return None
    return mprof_resolve.resolve(mmap, text, top=top)


def render(rows, overflow):
    if rows is None:
        return ("<p id='table'>No capture in this window. Start one, then "
                "reload.</p>")
    banner = ""
    if overflow:
        banner = (f"<p class='warn'>INCOMPLETE: {overflow} activations were "
                  f"dropped — methods are missing from this table. Counts are "
                  f"exact; percentages are lower bounds.</p>")
    body = "".join(
        f"<tr><td>{n}</td><td>{pct:.1f}%</td><td>{_html.escape(name)}</td></tr>"
        for name, n, pct in rows)
    return (f"<div id='table'>{banner}<table>"
            f"<tr><th>count</th><th>%</th><th>method</th></tr>{body}"
            f"</table></div>")
```

Then the server, in the same file:

```python
PAGE = """<!doctype html><meta charset=utf-8><title>node profile</title>
<script src="/htmx.min.js"></script>
<style>body{font-family:system-ui;margin:2rem;max-width:60rem}
table{border-collapse:collapse;width:100%}td,th{padding:.25rem .75rem;
text-align:left;border-bottom:1px solid #ccc}td:first-child,td:nth-child(2)
{text-align:right;font-variant-numeric:tabular-nums}
.warn{background:#fee;padding:.5rem;border-left:3px solid #c00}</style>
<h1>node profile — %(board)s</h1>
<form hx-post="/capture" hx-target="#table">
  <label>window (ms) <input name="ms" value="10000" size="8"></label>
  <button type="submit">capture</button>
</form>
<p>A capture takes the window you ask for, plus one check-in for the request to
reach the node. Reload after it elapses.</p>
%(table)s
"""


class Handler(BaseHTTPRequestHandler):
    board = "c6bench"
    sidecar = None
    node = None
    top = 64

    def _log_text(self):
        store = logstore.LogStore(board=self.board)
        days = store.days()
        if not days:
            return ""
        return open(store.path_for(days[-1]), encoding="utf-8",
                    errors="replace").read()

    def _table(self):
        text = self._log_text()
        mmap = json.load(open(self.sidecar)) if self.sidecar else {}
        rows = latest_window(text, mmap, top=self.top)
        overflow = 0
        for m in mprof_resolve.HDR_RE.finditer(text):
            overflow = int(m.group(4))          # last window wins
        return render(rows, overflow)

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        payload = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/health":
            return self._send(200, "ok", "text/plain; charset=utf-8")
        if self.path == "/htmx.min.js":
            # Vendored, never a CDN: this runs on a bench network that may have
            # no route out, and a view that blanks when the internet is down is
            # useless at exactly the moment it is wanted.
            vendored = os.path.join(os.path.dirname(__file__), "htmx.min.js")
            return self._send(200, open(vendored, encoding="utf-8").read(),
                              "application/javascript")
        if self.path != "/":
            return self._send(404, "not found", "text/plain; charset=utf-8")
        self._send(200, PAGE % {"board": _html.escape(self.board),
                                "table": self._table()})

    def do_POST(self):
        if self.path != "/capture":
            return self._send(404, "not found", "text/plain; charset=utf-8")
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8")
        ms = 10000
        for pair in body.split("&"):
            k, _, v = pair.partition("=")
            if k == "ms" and v.isdigit() and int(v) > 0:
                ms = int(v)
        # porta owns the dbg/req write, so the trigger crosses the seam here and
        # nowhere else. A failure is REPORTED, not swallowed: a capture that
        # never started and one that returned nothing look identical otherwise.
        try:
            subprocess.run(
                ["porta", "put", f"node/{self.node}/dbg/req", f"profile {ms}"],
                check=True, capture_output=True, timeout=10)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired,
                FileNotFoundError) as exc:
            return self._send(200, f"<p id='table' class='warn'>capture NOT "
                                   f"started: {_html.escape(str(exc))}</p>")
        self._send(200, f"<p id='table'>capture requested ({ms} ms). Reload "
                        f"once it elapses.</p>")


def main(argv):
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--board", default="c6bench")
    p.add_argument("--node", required=True, help="node id for the dbg/req path")
    p.add_argument("--sidecar", required=True, help="<image>.oimg.methods.json")
    p.add_argument("--port", type=int, default=8087)
    p.add_argument("--top", type=int, default=64)
    a = p.parse_args(argv[1:])
    Handler.board, Handler.node = a.board, a.node
    Handler.sidecar, Handler.top = a.sidecar, a.top
    print(f"profile view on http://localhost:{a.port} (board={a.board})")
    HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

Vendor htmx beside the file — the page must not reach a CDN:

```bash
cd nodus/tuvm/zephyr/tools/console
curl -sL https://unpkg.com/htmx.org@2/dist/htmx.min.js -o htmx.min.js
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_view.py -v
```

Expected: 4 passed.

- [ ] **Step 5: Verify the page renders in a browser**

`file://` is blocked, so serve it and look at it. Do not judge the layout from the HTML source — **view the render**, because text collides silently:

```bash
cd nodus/tuvm/zephyr/tools/console
python3 view.py --board c6bench --node <node-id> \
  --sidecar ../../build/PortaBridgeNodeC6.oimg.methods.json --port 8087
```

Open `http://localhost:8087`, confirm the table renders, then confirm it scrolls rather than clipping by comparing `scrollHeight` to `clientHeight`.

- [ ] **Step 6: Document it**

Add a section to `nodus/tuvm/zephyr/tools/console/README.md` giving the invocation, the three routes, and the one-line statement that the trigger goes through porta because porta owns the `dbg/req` write.

- [ ] **Step 7: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/view.py nodus/tuvm/zephyr/tools/console/test_view.py nodus/tuvm/zephyr/tools/console/htmx.min.js nodus/tuvm/zephyr/tools/console/README.md
git commit -m "feat(#165): the profile becomes something you can see

An htmx view served by the console collector, which already holds the log
history, the offset->name join and the symbolizer. Serving it from porta's Go
mux would have moved the BULK across the seam and made the log layout a
contract; this way only the trigger crosses, and the trigger is one write.

'No capture in this window' is rendered explicitly rather than as an empty
table, because a node that reported nothing and a node that was never asked
must not look the same."
```

---

### Task 9: Close-out — promote the knowledge to a mainline document

**An issue is not closed until its durable knowledge reaches a document of record.** #164 currently fails this: nothing in `review/` or `docs/next-steps-overview.md` mentions the profiler at all. This task closes both #164 and #165.

**Files:**
- Modify: `review/program-sequence.md` (the ladder)
- Modify: `docs/next-steps-overview.md` (Part 2, item 3 — the #56 rung)
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md` §4.5
- Modify: `docs/design/2026-08-06-method-profile-over-porta-design.md` (status line)

- [ ] **Step 1: Add the rung to the ladder**

In `review/program-sequence.md`, following the existing entry format exactly, record: the counter (#164), its cost (0.90% on a C6), the capacity measurement from Task 1, the window control, and the console-out / space-in split. Name the two things that would otherwise have to be rediscovered: **counts are a property of the program, not the processor** (a ratio measured on host holds on a C6; only per-activation *time* needs the device), and **`pumpEvents`/`Space>>drain` dominate any host profile** because `pulse_pump` is non-blocking on posix.

- [ ] **Step 2: Amend RECORDS §4.5**

Design D7: scope the "stateless relay" claim to the path. Replace "Porta stays a **stateless relay**; the debug session lives in the node" with wording that says porta does not interpret or own the **`dbg/req`/`dbg/resp` session**, and that this is a claim about that path and not about developer-facing assets (console history, images, symbol sidecars), which live off the space by the same rule that keeps profiler blobs off it.

Delete the "Not yet implemented on the nsl node" paragraph — it is false as of Task 5.

- [ ] **Step 3: Update `docs/next-steps-overview.md`**

Part 2 item 3 lists #56's four capabilities as DONE. Add profiling as a fifth line with its status, and update the Part 1 bench notes if Task 1 changed which board holds what.

- [ ] **Step 4: Flip the design doc's status**

Change the `**Status:**` line of `docs/design/2026-08-06-method-profile-over-porta-design.md` from `design COMPLETE` to `IMPLEMENTED <date>`, and record under D9 the number Task 1 measured and which branch it selected.

- [ ] **Step 5: Run the full gates one last time and commit**

```bash
cd nodus/nsl && python3 tools/run_tests_oevm.py
cd porta      && go test ./...
cd nodus/tuvm && bash tests/porta_sync_host.sh
```

```bash
git add review/program-sequence.md docs/next-steps-overview.md protocol/PROTOCOL-NSL-RECORDS.md docs/design/2026-08-06-method-profile-over-porta-design.md
git commit -m "docs(#164,#165): the profiler reaches the ladder, and §4.5 stops overclaiming

Neither issue could close while its knowledge lived only in a dated design doc,
a results doc and an issue thread.

RECORDS §4.5 said 'porta stays a stateless relay' without saying WHAT of — which
reads as a ban on porta holding console history or symbol sidecars, and is not.
Scoped to the dbg session. Its 'not yet implemented on the nsl node' paragraph
is deleted; it is implemented now.

Closes #164. Closes #165."
```

- [ ] **Step 6: Comment the deferred work onto its issues**

D10 (scope-by-method-identity) and D9's counter branch are deferred, not dropped. Comment on #165 with what was deferred and what would earn it back, and file a new issue for D10 citing the design section, so the deferral is falsifiable rather than vague.

---

## Deferred by design — do not implement in this plan

- **D10, localized profiling scopes.** Gated on Task 1's measurement. The accepted shape is scope-by-method-identity at `push_frame` + `OP_RET`, **not** the existing breakpoint machinery — those checks are compiled out of Zephyr builds and cost 10–16% per bytecode (`nodus/tuvm/zephyr/CMakeLists.txt:267`).
- **Space-Saving displacement / a 512-slot table.** Gated on Task 1. Do not pick one before the number exists.
- **An htmx view on porta's own mux.** D8 defers it; a link from porta costs nothing later.
- **Time attribution per method, caller attribution, call graphs, sampling.** Out of scope since #164.
- **`diag/spans/ctl`'s authority contradiction (#159).** D2 removes this feature's dependence on it; the contradiction stays open on its own merits.
