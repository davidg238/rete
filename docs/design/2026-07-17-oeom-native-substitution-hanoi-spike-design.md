# OEOM native-substitution spike (Hanoi) — design

*Written 2026-07-17 on `feat/zephyr-model-ab`. A throwaway experiment gated behind
`OEVM_HANOI_NATIVE`, off by default. Not a production feature — a concept probe.*

## Motivation

The green-book framing (interpreter → simple translator → optimizing translator)
maps directly onto tuvm: we sit at the interpreter end, and the measured ~12× VM gap
vs Toit's compiled engine on the identical ESP32-DevKitC V4 (WROOM-32E) board is
roughly the interpreter-vs-translator gap. Towers of Hanoi is the cleanest possible
probe of that gap — all heat is in **one trivial, allocation-free method**
(`HanoiNode.ns:31` `hanoiMove:from:to:temp:`), pure SMI integer recursion, no heap,
no I/O, not even a move counter. Measured baseline: **11.43 µs/move, constant across
n=16→24** (`~/workspaceToit/stic/README.md`, "Hanoi scaling").

OEOM makes native substitution structurally free: dispatch already forks on the
target closure's vtable (`src/vm.c:1080`):

```c
if (obj_vtable((oop)tcl) == PrimitiveClosureVtable) {
    oop (*func)(oop, int, oop*) = (oop (*)(oop, int, oop*))closure_entry(tcl);
    result = func(resolved_rcvr, send_argc, send_args);   // native, no frame, no bytecode
} else { push_frame(...); /* interpret bytecode */ }
```

A "native method" is therefore just a `Binding` whose `method` is a `PrimitiveClosure`
whose `entry_point` is a C function. This spike installs the **first** such binding
that services a *user-level* selector (the existing ~14 builtins get their behavior
from the `PRIM_NAMES` intercept path, not from stored `PrimitiveClosure` bindings), and
measures how much of the 12× a native drop-in reclaims on Hanoi.

**The question this answers:** is the AOT ceiling high enough to ever justify building
a bytecode→native compiler? This is a distant-future concern (the porta/nodus/
agent-first framework will collect months of usage data first, then a bespoke AOT
patch could optimize a proven hot method), so we want the ceiling number now, cheaply.

## Scope guard

- Entirely behind `#ifdef OEVM_HANOI_NATIVE`, **off by default**. Production defaults
  (debug hooks off, IRAM hot-set on, Path B device default) are untouched.
- New files only. `zephyr/HanoiNode.ns` (a committed bench node) is left intact.
- The `.oimg` frozen image format is **not** modified. The native machinery is
  firmware-static and boot-wired, exactly as the existing builtins are — this does
  not violate the "frozen model" posture (`review/WHY-OEOM.md`).

## Design

### 1. The native method — `src/hanoi_native.c` (new, flag-gated)

```c
static volatile uint32_t g_hanoi_moves;        /* DCE fence + correctness check */
static OEVM_HOT void hanoi_c(intptr_t n) {
    if (n > 1) { hanoi_c(n-1); hanoi_c(n-1); }
    g_hanoi_moves++;                           /* == 2^n-1 when the tree completes */
}
oop OEVM_HOT prim_hanoi(oop recv, int argc, oop *args) {   /* args[0]=n (SMI) */
    (void)recv; (void)argc;
    g_hanoi_moves = 0;
    hanoi_c(UNTAG_SMI(args[0]));
    return args[0];                            /* returns n, matching the .ns method */
}
```

- **Zero allocation, holds no oops across any call** → GC-trivial (nothing to trace).
- `OEVM_HOT` routes it into `.iram.text` (gated on `OEVM_HOT_SECTION_IRAM`, forwarded
  from the `OEVM_IRAM_HOT` env knob), matching the interpreter loop — the comparison is
  IRAM-vs-IRAM, so no layout roulette (per benchmark discipline: within-binary deltas,
  IRAM-pinned hot paths only).
- The `volatile` move counter (a) prevents the C compiler from eliding the
  side-effect-free recursion, and (b) is asserted `== 2^n − 1` as a correctness gate.

### 2. The vtable binding — the mechanism under test

Firmware-static storage (`.data`, covered by `mark()`'s `is_read_only` fence at
`src/gc.c:496`, the same fence that makes `true`/`false`/symbols reachable-but-immortal):

```c
static struct Binding hanoi_bindings[1];       /* .symbol wired at boot */
static struct Closure prim_hanoi_closure;      /* vtable=PrimitiveClosureVtable,
                                                  entry_point=prim_hanoi */
static struct Vtable  Vtable_HanoiNative;       /* delegate=NULL, bindings=hanoi_bindings,
                                                  capacity=size=1 */
static struct Object  hanoi_native_singleton;   /* the receiver `hn`;
                                                  vtable=&Vtable_HanoiNative */
```

**Boot ordering constraint (confirmed).** `oevm_image_main.c:21` runs `kernel_boot()`
*before* `oimg_load()`, so the image's symbols/vtables do not exist at `kernel_boot`
time — the native binding **cannot** be wired there. It is wired lazily, on first
acquisition, when the image is fully loaded.

**Wiring, done lazily inside the acquisition primitive (§3).** Lookup compares selectors
by pointer identity (`dispatch.c:310` `binding_symbol(...) == selector`), and runtime
interning does **not** reliably pointer-match `.oimg` blob symbols (`dispatch.c:508`
documents this hazard). So the native binding must hold the *image's* canonical symbol
oop, not a fresh one. Resolution that needs no intern API and no symbol-literal support:
the acquisition primitive is sent to `self` (the `HanoiAB` instance), so its `receiver`
carries `HanoiAB`'s class vtable. Walk that vtable + its delegate chain (exactly as
`lookup()` does) and `strcmp(symbol_name(binding.symbol), "hanoiMove:from:to:temp:")`;
the matching binding's `symbol` **is** the canonical interned oop the compiled
`hn hanoiMove:` send will present. Capture it, fill `hanoi_bindings[0]`, and mark wiring
done (idempotent — subsequent calls skip straight to returning the singleton).

GC safety of the mutable-at-wire statics: they live in `.data` (writable, so the lazy
fill is legal), which is neither blob, rodata, float-arena, nor a valid heap object — so
`mark()` skips them at the `is_valid_heap_object` guard (`gc.c:503`) before ever touching
them. The singleton, its vtable, and the `PrimitiveClosure` are therefore immortal and
trace-free.
2. Fill `hanoi_bindings[0] = { that symbol, &prim_hanoi_closure }`.
3. Point `prim_hanoi_closure.entry_point = prim_hanoi`, vtable = `PrimitiveClosureVtable`.
4. Point `Vtable_HanoiNative` at the bindings; point `hanoi_native_singleton` at it.

### 3. Handing the receiver to nsl

One acquisition primitive `installNativeHanoi` added to `dispatch.c`'s `PRIM_NAMES`,
**sent to `self`** (the `HanoiAB` instance — so its `receiver` carries `HanoiAB`'s class
vtable for the symbol scan in §2). It lazily wires the binding on first call, then
returns `&hanoi_native_singleton`. This is a **one-shot** acquisition run once before the
timing loop. The measured hot calls dispatch through the vtable `PrimitiveClosure` fork
(`vm.c:1080`), **not** the intercept path — so the thing being timed is genuinely "native
code stored in a vtable binding", which is the claim. (Confirm at run time via a
trace/assert that the timed call reaches `prim_hanoi`, not a DNU fall-through.)

### 4. The benchmark — `zephyr/HanoiAB.ns` (new; `HanoiNode.ns` untouched)

Same binary, same `n`, means-of-3:

```smalltalk
hn := self installNativeHanoi.   "wires the binding once, returns the native receiver"
"bytecode:" self hanoiMove: n from: 1 to: 2 temp: 3.   "timed ×3 via millis"
"native:  " hn   hanoiMove: n from: 1 to: 2 temp: 3.   "timed ×3 via millis"
```

`HanoiAB` defines its own bytecode `hanoiMove:from:to:temp:` (verbatim from
`HanoiNode.ns:31`) so `self hanoiMove:` is the interpreted path, and its class vtable is
what `installNativeHanoi` scans for the canonical selector.

Emits `hanoi-bc <n>: <ms>`, `hanoi-native <n>: <ms>`, and the native move-count for the
`2^n − 1` check. Depths **16 and 20** (bytecode ≈ 0.75 s / ≈ 12 s; native expected
sub-ms to low-ms). n=24 optional (bytecode ≈ 192 s).

### 5. What the number means — the honest ceiling caveat

The native version **fully inlines the recursion in C**, collapsing the entire send
tree into C calls with zero interpreter work. It therefore measures the **optimistic
ceiling** of bespoke AOT (recursion inlined) — *not* a mechanical bytecode→native
transliteration that would still dispatch each recursive self-send. That is
deliberately the right first question: is the ceiling high enough to justify a
compiler at all? Because native-C Hanoi ≈ Toit's compiled speed (both native), the
hypothesis is that this reclaims **most or all** of the ~12× on this workload. A large
reclaim = the go-signal; a small one would kill the idea cheaply.

## Run plan

Two phases — host first to de-risk, device for the headline:

1. **Host sanity (first).** Build with `OEVM_HANOI_NATIVE` on. Easier to debug a crash
   (gdb/asan, fast rebuild) than an ESP32 reset loop. Goals here are *correctness and
   dispatch fidelity only*: native move-count `== 2^n − 1`, native `hanoiMove:` returns
   `n`, and the timed call provably reaches `prim_hanoi` via the `PrimitiveClosureVtable`
   fork (not the `PRIM_NAMES` intercept, not a DNU fall-through). `OEVM_HOT` is a no-op
   on host (no IRAM/XIP), which is fine — host timings are not the deliverable, so the
   symbol-identity wiring (§2 step 1) and the dispatch path are what we're validating.
2. **ESP32 headline (after host is clean).** Model B / `.oimg`, WROOM-32E — the board the
   11.43 µs/move and 12× numbers come from. Build with `OEVM_HANOI_NATIVE` on and
   `OEVM_IRAM_HOT` on (both hot paths IRAM-pinned). Flash, capture serial, compute
   means-of-3, re-assert the `2^n − 1` check, report the `hanoi-bc / hanoi-native` ratio.

## Success criteria

1. **Correctness:** native move-count `== 2^n − 1` for every depth; native `hanoiMove:`
   returns `n`.
2. **Dispatch fidelity:** the timed native call reaches `prim_hanoi` via the
   `PrimitiveClosureVtable` fork (confirmed by instrumentation/trace, not the
   `PRIM_NAMES` intercept).
3. **Headline:** `hanoi-bc / hanoi-native` ratio at n=16 and n=20, stated against the
   ~12× Toit gap and the 11.43 µs/move baseline.

## Non-goals

- No bytecode→native compiler. No solidifier change. No runtime rebind of solidified
  (const/XIP) user-class vtables. No change to production build defaults. No claim
  beyond Hanoi — DeltaBlue-style spread-heat workloads are explicitly out of scope for
  this probe.

## Promotion (close-out)

On completion, the headline ratio and the "OEOM native-in-vtable dispatch works for a
user selector" finding are promoted to `~/workspaceToit/stic/README.md` (Hanoi scaling
section, a "native-AOT ceiling" line) and the ladder
(`~/workspace/tuvm/review/program-sequence.md`). This dated design stays as the "why".
