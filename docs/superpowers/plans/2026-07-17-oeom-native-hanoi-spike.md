# OEOM Native-Substitution Hanoi Spike — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove an OEOM `PrimitiveClosure`-in-vtable binding can service a user selector (`hanoiMove:from:to:temp:`) with native C, dispatch transparently, and measure how much of the ~12× Toit gap it reclaims on Towers of Hanoi.

**Architecture:** A pure-C recursive `hanoiMove:` (`prim_hanoi`) is bound into a firmware-static native vtable at runtime by an acquisition primitive (`installNativeHanoi`, sent to `self`) that scans the bench class's own vtable for the canonical interned selector oop. A bench node (`HanoiAB.ns`) sends the *same* selector to both its bytecode `self` and the native receiver, timing each. Everything is gated behind `OEVM_HANOI_NATIVE`, off by default.

**Tech Stack:** C99 (tuvm VM `src/`), nsl (`.ns` → `.oimg` via `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`), host image-VM (`oevm_image_main.c`), Zephyr/ESP32 (`zephyr/run.sh`, west, esptool).

## Global Constraints

- **All new code behind `#ifdef OEVM_HANOI_NATIVE`, OFF by default.** Production build defaults (debug hooks off, IRAM hot-set on, Path B default) stay untouched.
- **Do NOT touch** `zephyr/HanoiNode.ns` (committed bench node), `src/solidified_image.c` / `.stmap` (tracked Model-A fixture), or the `PRIM_NAMES` array / `prim_ids.h` ordering (positional, mirrored — appending shifts ids). The acquisition primitive is `strcmp`-gated instead (mirrors `dispatch.c:877` `crashRecordTag:text:`).
- **Compiler is `~/workspace/nsl-tuvm/tools/ns_compile_oevm.py`** — NOT `~/workspace/ns-oevm` (stale; the tracked `tests/test_load_run.sh` still names the old path).
- **ESP32 devkit is `/dev/ttyUSB0`.** The jag/Toit WROOM-32E is on `/dev/ttyUSB1` — never let esptool auto-detect (it clobbers USB1). `run.sh` pins USB0.
- **Never run two nsl suites at once** (shared `/tmp`). Commit after each task.
- **Mechanism anchors (verified):** native invocation `dispatch.c:1673`; access-check skipped for primitives `dispatch.c:1667`; symbol-scan template `dispatch.c:868-884`; GC non-heap skip `gc.c:503`; struct layouts `src/oevm.h:98,108,169`; `OEVM_HOT` `src/oevm.h:29`.

---

### Task 1: Native method + acquisition primitive (`src/hanoi_native.c` + dispatch gate)

**Files:**
- Create: `src/hanoi_native.c`
- Modify: `src/dispatch.c` (add strcmp gate in `send_resolve`, after the capability block ~line 1000)
- Test: `tests/hanoi_ab_host.sh` (created in Task 3; this task's check is that the host VM compiles+links with the flag)

**Interfaces:**
- Produces: `oop prim_hanoi(oop, int, oop*)` — native `hanoiMove:`, returns move count as SMI. `oop hanoi_native_acquire(oop receiver)` — lazily wires the binding, returns the native singleton oop.
- Consumes: `PrimitiveClosureVtable`, `VtableVtable`, `SymbolVtable` (extern globals); accessors `obj_vtable`, `vtable_bindings`, `vtable_delegate`, `binding_symbol`, `binding_method`, `symbol_name`, `closure_entry`; macros `IS_SMI`, `TAG_SMI`, `UNTAG_SMI`; `OEVM_HOT`.

- [ ] **Step 1: Create `src/hanoi_native.c`**

```c
/* src/hanoi_native.c
 *
 * Experiment (2026-07-17): OEOM native-substitution spike. Behind OEVM_HANOI_NATIVE
 * (off by default). Installs the first PrimitiveClosure-in-vtable binding that
 * services a *user* selector (hanoiMove:from:to:temp:) and lets a bench send to it,
 * to measure the native-AOT ceiling vs the bytecode interpreter on Towers of Hanoi.
 * Design: docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md
 */
#include "oevm.h"
#include "oimg.h"
#include <string.h>
#include <stdint.h>

#ifdef OEVM_HANOI_NATIVE

/* Pure-C recursion. Zero allocation, holds no oops -> GC-trivial. The volatile
   counter fences dead-code elimination and doubles as the 2^n-1 correctness check. */
static volatile uint32_t g_hanoi_moves;

static OEVM_HOT void hanoi_c(intptr_t n) {
    if (n > 1) { hanoi_c(n - 1); hanoi_c(n - 1); }
    g_hanoi_moves++;
}

/* The native method. Primitive ABI: oop(oop,int,oop*). args[0] = n (SMI).
   Returns the move count as an SMI so the bench can assert == 2^n-1 — which also
   proves the send dispatched HERE (to the PrimitiveClosure) rather than DNU'ing. */
oop OEVM_HOT prim_hanoi(oop receiver, int argc, oop *args) {
    (void)receiver; (void)argc;
    g_hanoi_moves = 0;
    hanoi_c(UNTAG_SMI(args[0]));
    return TAG_SMI((intptr_t)g_hanoi_moves);
}

/* Firmware-static native machinery (.data; mutable so acquire() can fill it once).
   mark() skips these at the is_valid_heap_object guard (gc.c:503) — not blob, not
   rodata, not float-arena, not a heap object => immortal, trace-free. */
static struct Closure prim_hanoi_closure;
static struct Binding hanoi_bindings[1];
static struct Vtable  Vtable_HanoiNative;
static struct Object  hanoi_native_singleton;
static int hanoi_wired;

/* Acquisition + one-time lazy wiring. Sent to `self` (the HanoiAB instance) so
   `receiver` carries HanoiAB's class vtable. Walk it + its delegate chain (as
   lookup() does) to find the canonical interned "hanoiMove:from:to:temp:" symbol
   oop the compiled send will present (dispatch.c:868 template), and bind the native
   closure to it. Idempotent. Runs post-load, so image symbols exist. */
oop hanoi_native_acquire(oop receiver) {
    extern oop PrimitiveClosureVtable;
    extern oop VtableVtable;
    extern oop SymbolVtable;
    if (!hanoi_wired) {
        oop sel = NULL;
        oop curr = obj_vtable(receiver);
        while (curr != NULL && !IS_SMI(curr)) {
            struct Vtable *vt = (struct Vtable *)curr;
            struct Binding *bs = vtable_bindings((oop)vt);
            for (size_t i = 0; i < vt->size; i++) {
                struct Symbol *s = (struct Symbol *)binding_symbol(&bs[i]);
                if (s != NULL && obj_vtable((oop)s) == SymbolVtable &&
                    strcmp(symbol_name(s), "hanoiMove:from:to:temp:") == 0) {
                    sel = (oop)s; break;
                }
            }
            if (sel != NULL) break;
            curr = vtable_delegate((oop)vt);
        }
        if (sel == NULL) return NULL;   /* selector not found: bench misconfigured */

        prim_hanoi_closure.base.vtable = PrimitiveClosureVtable;
        prim_hanoi_closure.entry_point = (void *)prim_hanoi;
        hanoi_bindings[0].symbol = sel;
        hanoi_bindings[0].method = (oop)&prim_hanoi_closure;
        Vtable_HanoiNative.base.vtable = VtableVtable;
        Vtable_HanoiNative.delegate = NULL;
        Vtable_HanoiNative.bindings = hanoi_bindings;
        Vtable_HanoiNative.capacity = 1;
        Vtable_HanoiNative.size = 1;
        Vtable_HanoiNative.is_nested = 0;
        hanoi_native_singleton.vtable = (oop)&Vtable_HanoiNative;
        hanoi_wired = 1;
    }
    return (oop)&hanoi_native_singleton;
}

#endif /* OEVM_HANOI_NATIVE */
```

- [ ] **Step 2: Add the acquisition gate in `src/dispatch.c`**

In `send_resolve`, immediately after the native-capability block that ends at the `if (receiver == cap_mirror) return mirror_dispatch(...)` line (~line 1000), insert:

```c
#ifdef OEVM_HANOI_NATIVE
    /* Experiment: acquisition + lazy wiring of the native Hanoi method. Sent to the
       HanoiAB instance (self); returns the native receiver. strcmp-gated so it needs
       no PRIM_NAMES id (mirrors the crashRecordTag: interception below). */
    if (selector != NULL && !IS_SMI(selector) && SymbolVtable != NULL &&
        obj_vtable(selector) == SymbolVtable &&
        strcmp(symbol_name((struct Symbol *)selector), "installNativeHanoi") == 0) {
        extern oop hanoi_native_acquire(oop);
        return hanoi_native_acquire(receiver);
    }
#endif
```

- [ ] **Step 3: Verify it compiles + links on host (flag ON)**

Run:
```bash
cd ~/workspace/tuvm && gcc -Wall -std=c99 -DOEVM_HANOI_NATIVE -Isrc -fsyntax-only src/hanoi_native.c && echo "SYNTAX OK"
```
Expected: `SYNTAX OK` (no warnings/errors). Full link is exercised in Task 3.

- [ ] **Step 4: Verify the flag-OFF build is untouched**

Run:
```bash
cd ~/workspace/tuvm && gcc -Wall -std=c99 -Isrc -fsyntax-only src/hanoi_native.c && echo "EMPTY TU OK"
```
Expected: `EMPTY TU OK` — with the flag off, `hanoi_native.c` is an empty translation unit and `dispatch.c`'s gate is compiled out.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add src/hanoi_native.c src/dispatch.c
git commit -m "feat(spike): native hanoiMove: PrimitiveClosure + installNativeHanoi (OEVM_HANOI_NATIVE)"
```

---

### Task 2: Bench node (`zephyr/HanoiAB.ns`)

**Files:**
- Create: `zephyr/HanoiAB.ns`

**Interfaces:**
- Consumes: `installNativeHanoi` (unary, → native receiver), `hanoiMove:from:to:temp:` (both bytecode-on-self and native-on-`hn`), `millis`, `to:do:`, `,`, `printString`, `out`.
- Produces: stdout lines `hanoi-bc <n>: <ms>`, `hanoi-native <n>: <ms>`, `hanoi-moves <n>: <count>`, `done`.

- [ ] **Step 1: Create `zephyr/HanoiAB.ns`**

```
(*
HanoiAB — A/B bench: bytecode hanoiMove: vs native (PrimitiveClosure-in-vtable)
hanoiMove:, same binary, same n, means-of-3. Emits per-run ms for each path plus
the native move count (must equal 2^n-1, which also proves the native send reached
prim_hanoi rather than DNU). Experiment behind OEVM_HANOI_NATIVE; see
docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md.
Depths 16 and 20 (bytecode ~0.75s / ~12s; native sub-ms to low-ms).
*)
class HanoiAB packageUsing: manifest = () (
  public main: platform args: args = (
    | hn |
    hn:: self installNativeHanoi.
    self runAB: 16 native: hn.
    self runAB: 20 native: hn.
    ('done') out.
    ^ 0
  )
  runAB: n native: hn = (
    | moves |
    self time3Bc: n.
    self time3Native: n on: hn.
    moves:: hn hanoiMove: n from: 1 to: 2 temp: 3.
    ('hanoi-moves ' , n printString , ': ' , moves printString) out.
    ^ n
  )
  time3Bc: n = (
    | t0 t1 |
    1 to: 3 do: [:i |
      t0:: 0 millis.
      self hanoiMove: n from: 1 to: 2 temp: 3.
      t1:: 0 millis.
      ('hanoi-bc ' , n printString , ': ' , (t1 - t0) printString) out.
    ].
    ^ n
  )
  time3Native: n on: hn = (
    | t0 t1 |
    1 to: 3 do: [:i |
      t0:: 0 millis.
      hn hanoiMove: n from: 1 to: 2 temp: 3.
      t1:: 0 millis.
      ('hanoi-native ' , n printString , ': ' , (t1 - t0) printString) out.
    ].
    ^ n
  )
  hanoiMove: n from: src to: dst temp: tmp = (
    n > 1 ifTrue: [
      self hanoiMove: n - 1 from: src to: tmp temp: dst.
      self hanoiMove: n - 1 from: tmp to: dst temp: src.
    ].
    ^ n
  )
) : (
)
```

- [ ] **Step 2: Verify it compiles to `.oimg`**

Run:
```bash
cd ~/workspace/tuvm && gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl && /tmp/dl > /tmp/layout.json \
 && python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py zephyr/HanoiAB.ns /tmp/hanoi_ab.oimg --format=oimg --layout=/tmp/layout.json \
 && echo "COMPILE OK: $(stat -c %s /tmp/hanoi_ab.oimg) bytes"
```
Expected: `COMPILE OK: <n> bytes`. If the compiler rejects `to:do:` or `installNativeHanoi` (unknown selector is fine — it resolves at runtime), fix syntax and rerun. `installNativeHanoi` must appear as a normal unary send in the compiled image (it is intercepted at runtime, not compile time).

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/HanoiAB.ns
git commit -m "test(spike): HanoiAB A/B bench node (bytecode vs native hanoiMove:)"
```

---

### Task 3: Host sanity harness (`tests/hanoi_ab_host.sh`) — Phase 1 deliverable

**Files:**
- Create: `tests/hanoi_ab_host.sh`

**Interfaces:**
- Consumes: `src/hanoi_native.c` + `dispatch.c` gate (Task 1), `zephyr/HanoiAB.ns` (Task 2).
- Produces: exit 0 + `HOST SANITY PASS` when native move-count == 2^n−1 for n∈{16,20}.

- [ ] **Step 1: Create `tests/hanoi_ab_host.sh`**

```bash
#!/usr/bin/env bash
# Host sanity for the OEOM native-substitution spike (Phase 1). Builds the image VM
# with OEVM_HANOI_NATIVE, runs HanoiAB, and asserts the native hanoiMove: returns
# 2^n-1 — which also proves the timed native call dispatched to prim_hanoi (a DNU
# or wrong receiver would not return the exact move count). Host timings are NOT the
# deliverable (host clock may not advance); correctness + dispatch fidelity are.
set -euo pipefail
cd "$(dirname "$0")/.."
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"

echo "[1/4] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

echo "[2/4] compile HanoiAB.ns -> .oimg"
python3 "$NSL" zephyr/HanoiAB.ns /tmp/hanoi_ab.oimg --format=oimg --layout=/tmp/layout.json

echo "[3/4] build image VM with -DOEVM_HANOI_NATIVE"
gcc -Wall -std=c99 -DOEVM_HANOI_NATIVE -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" src/mirror.c src/hanoi_native.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_hanoi -lm

echo "[4/4] run + assert move counts"
OUT="$(/tmp/oevm_hanoi /tmp/hanoi_ab.oimg)"
echo "--- program output ---"; echo "$OUT"; echo "----------------------"
ok=1
for n in 16 20; do
  want=$(( (1 << n) - 1 ))
  if echo "$OUT" | grep -q "hanoi-moves ${n}: ${want}"; then
    echo "PASS moves n=$n == $want"
  else
    echo "FAIL moves n=$n (want $want)"; ok=0
  fi
done
if [ "$ok" = 1 ]; then echo "HOST SANITY PASS"; else echo "HOST SANITY FAIL"; exit 1; fi
```

- [ ] **Step 2: Make executable + run**

Run:
```bash
cd ~/workspace/tuvm && chmod +x tests/hanoi_ab_host.sh && ./tests/hanoi_ab_host.sh
```
Expected: ends with `HOST SANITY PASS`; lines `PASS moves n=16 == 65535` and `PASS moves n=20 == 1048575`.

- [ ] **Step 3: If it fails — debug on host (this is why Phase 1 is host-first)**

Likely failure modes and checks:
- `hanoi-moves` line missing / value `0` or `nil`: the native send DNU'd → `installNativeHanoi` didn't wire (selector-scan miss) or `hn` is nil. Add a temporary `fprintf(stderr,...)` in `hanoi_native_acquire` to confirm `sel != NULL`, then rebuild.
- Segfault: run under gdb — `gdb --args /tmp/oevm_hanoi /tmp/hanoi_ab.oimg`, `run`, `bt`. Check the static struct field assignments against `src/oevm.h:98,108,169`.
Fix, rerun Step 2 until `HOST SANITY PASS`.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm && git add tests/hanoi_ab_host.sh
git commit -m "test(spike): host sanity harness for native Hanoi (asserts 2^n-1)"
```

---

### Task 4: Zephyr/ESP32 build wiring

**Files:**
- Modify: `zephyr/CMakeLists.txt` (add `hanoi_native.c` to Model-B sources; forward `OEVM_HANOI_NATIVE`)

**Interfaces:**
- Consumes: `OEVM_HANOI_NATIVE` env var (opt-in).
- Produces: an ESP32 app image that compiles `src/hanoi_native.c` and defines the flag when `OEVM_HANOI_NATIVE=1`.

- [ ] **Step 1: Add `hanoi_native.c` to the Model-B source list**

In `zephyr/CMakeLists.txt`, in the `elseif(TUVM_MODEL STREQUAL "B")` branch's `target_sources(app PRIVATE ...)` block (the one listing `oimg_loader.c` + `kernel_boot.c` + `blob.S`), add the line:

```cmake
    ${TUVM_SRC}/src/hanoi_native.c
```

- [ ] **Step 2: Forward the opt-in flag**

In `zephyr/CMakeLists.txt`, near the other `if(DEFINED ENV{...})` forwarding blocks (after the `OEVM_IRAM_HOT` block ~line 145), add:

```cmake
# Experiment (2026-07-17): OEOM native-substitution Hanoi spike. OFF by default.
# export OEVM_HANOI_NATIVE=1 to build the native hanoiMove: PrimitiveClosure path
# (src/hanoi_native.c + the strcmp gate in dispatch.c). See
# docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md.
if("$ENV{OEVM_HANOI_NATIVE}" STREQUAL "1")
  target_compile_definitions(app PRIVATE OEVM_HANOI_NATIVE=1)
endif()
```

- [ ] **Step 3: Verify a clean configure+build (flag ON), no flash**

Run:
```bash
cd ~/workspace/tuvm && OEVM_HANOI_NATIVE=1 zephyr/run.sh --model B --node HanoiAB.ns --pristine --build-dir build-esp32-hanoi
```
Expected: bakes `zephyr/HanoiAB.oimg`, west build completes to `zephyr/build-esp32-hanoi/zephyr/zephyr.bin` with no errors. (No `--flash`, so nothing is written to the device yet.)

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/CMakeLists.txt
git commit -m "build(spike): Zephyr wiring for OEVM_HANOI_NATIVE (Model B)"
```

---

### Task 5: ESP32 headline run — Phase 2 deliverable

**Files:**
- Create: `docs/superpowers/plans/2026-07-17-hanoi-ab-results.md` (raw capture + computed ratio)

**Interfaces:**
- Consumes: Tasks 1–4. Produces: the `hanoi-bc / hanoi-native` ratio at n=16 and n=20, and the `2^n-1` re-assert on-device.

- [ ] **Step 1: Flash + capture (means-of-3 emitted by the node itself)**

Run:
```bash
cd ~/workspace/tuvm && OEVM_HANOI_NATIVE=1 OEVM_IRAM_HOT=1 \
  zephyr/run.sh --model B --node HanoiAB.ns --pristine --build-dir build-esp32-hanoi --flash --capture 90
```
Expected serial: three `hanoi-bc 16:`, three `hanoi-native 16:`, `hanoi-moves 16: 65535`, then the same for n=20 (`hanoi-moves 20: 1048575`), then `done`. Both hot paths are IRAM-pinned (`OEVM_IRAM_HOT=1`).
Note: `/dev/ttyUSB0` is pinned by `run.sh`; do not let esptool auto-detect (USB1 is the jag board).

- [ ] **Step 2: Record raw output + compute ratios**

Create `docs/superpowers/plans/2026-07-17-hanoi-ab-results.md` with: the verbatim serial capture; the mean of each 3-run group; `ratio = mean(hanoi-bc) / mean(hanoi-native)` for n=16 and n=20; and a one-line comparison to the 11.43 µs/move baseline and the ~12× Toit gap. Assert on-device `hanoi-moves 16: 65535` and `hanoi-moves 20: 1048575` are present.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add docs/superpowers/plans/2026-07-17-hanoi-ab-results.md
git commit -m "results(spike): ESP32 native-vs-bytecode Hanoi A/B ratio"
```

---

### Task 6: Promote findings (close-out per CLAUDE.md promotion rule)

**Files:**
- Modify: `~/workspaceToit/stic/README.md` (Hanoi scaling section — add a "native-AOT ceiling" line)
- Modify: `~/workspace/tuvm/review/program-sequence.md` (ladder — record the spike outcome)

- [ ] **Step 1: Add the native-AOT ceiling line to the stic Hanoi section**

Under the "Hanoi scaling (tuvm/nsl, ESP32)" section in `~/workspaceToit/stic/README.md`, add one line stating the measured native `hanoiMove:` time and the `hanoi-bc / hanoi-native` ratio at n=16/20, noting it is the fully-inlined AOT ceiling (recursion collapsed to C), and that native-C Hanoi ≈ Toit's compiled speed so the ratio indicates how much of the ~12× a bespoke AOT drop-in reclaims on this workload.

- [ ] **Step 2: Record the outcome in the ladder**

Add a rung/entry in `~/workspace/tuvm/review/program-sequence.md` recording: the spike proved OEOM native-in-vtable dispatch works for a user selector (`dispatch.c:1673`), the go/no-go signal (ratio), and that the dated design doc holds the "why". Cite absolute paths.

- [ ] **Step 3: Commit both repos**

```bash
cd ~/workspace/tuvm && git add review/program-sequence.md \
 && git commit -m "docs(spike): promote native-Hanoi ceiling finding to the ladder"
cd ~/workspaceToit/stic && git add README.md \
 && git commit -m "bench(hanoi): native-AOT ceiling line (tuvm OEOM substitution spike)"
```

---

## Self-Review

**1. Spec coverage:**
- §1 native method + `OEVM_HOT` + move-count fence → Task 1 Step 1 ✓
- §2 `PrimitiveClosure`-in-vtable + firmware-static storage + GC safety → Task 1 Step 1 ✓
- §2 lazy self-scan wiring (boot-order fix + symbol identity) → Task 1 Step 1 (`hanoi_native_acquire`) ✓
- §3 acquisition primitive sent to self, strcmp-gated (no PRIM_NAMES churn) → Task 1 Step 2 ✓
- §4 A/B bench, same selector, means-of-3, depths 16/20 → Task 2 ✓
- §5 ceiling caveat → carried into Task 5 Step 2 + Task 6 Step 1 promotion wording ✓
- Run plan: host-first (Task 3) then ESP32 (Task 5) ✓
- Success criteria: correctness `2^n-1` (Tasks 3,5), dispatch fidelity via exact move-count return (Task 3 Step 1 rationale), headline ratio (Task 5) ✓
- Scope guard: flag-gated, new files, `HanoiNode.ns` untouched → Global Constraints + Task 4 opt-in ✓
- Promotion rule → Task 6 ✓

**2. Placeholder scan:** No TBD/TODO. All C, nsl, bash, and CMake shown in full. Task 5 Step 2 / Task 6 describe prose to write (results narrative) — inherent to a results/promotion step, not code placeholders.

**3. Type consistency:** `prim_hanoi(oop,int,oop*)` matches the ABI cast at `dispatch.c:1673` and the acquisition `extern oop hanoi_native_acquire(oop)` matches its definition and call site. Struct fields (`base.vtable`, `delegate`, `bindings`, `capacity`, `size`, `is_nested`, `entry_point`, `symbol`, `method`) match `src/oevm.h:98,108,169`. Selector string `"hanoiMove:from:to:temp:"` identical in `hanoi_native.c`, `HanoiAB.ns`, and the design. Output tokens `hanoi-bc` / `hanoi-native` / `hanoi-moves` identical in `HanoiAB.ns` and `hanoi_ab_host.sh`.
