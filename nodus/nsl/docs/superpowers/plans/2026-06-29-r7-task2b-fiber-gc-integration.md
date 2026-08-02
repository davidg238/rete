# R7 Task 2b — fiber GC integration + Zephyr re-fence (post-review fixes)

Follow-up to Task 2 (ucontext fiber rewrite). The thorough review (4 parallel
reviewers, 2026-06-29) found the POSIX ucontext rewrite dropped the GC
integration the old "hijack" design relied on, and that the hijack was deleted
for ALL targets (breaking the Zephyr embedded path). Suite was green only
because the tiny fiber test never triggers GC and Zephyr isn't host-tested.

## RED test (the gate)
`OEVM_GC_STRESS=1` forces a collection before every allocation (gc.c:216).
Building the Path A runner for `ns-tests/04_somns_basic/45_fibers.ns` and running
it under `OEVM_GC_STRESS=1` SEGFAULTS today; it must print `ABABAB` / `done` and
exit 0 after the fixes. This is added to the harness as a permanent regression
gate (`fibers-gc-stress`), alongside portability-32.

## Findings being fixed (verified against source)
- C1: `entry_closure`/`entry_receiver` not GC roots → closure freed before first start.
- C2: suspended fibers' malloc'd C stacks never scanned → live oops in C locals collected.
- C3: GC while a non-main fiber is active scans `[&anchor … vm_c_stack_base]` (two
      unrelated regions) → SIGSEGV. This is the crash the RED test hits first.
- C4: `c_stack` (256KB) never freed → leak per fiber.
- I1: 32-bit pointer packing (`fib_p >> 32`) is UB on -m32 (portability gate compiles it).
- I3: main fiber's new POSIX fields left uninitialized (gc_allocate doesn't zero).
- M1: `g_fiber_switch_value` not a GC root (defensive).
- Zephyr: 4 hijack blocks deleted unconditionally; Zephyr `#else` still depends on them.
- I2 (deferred → issue): fiber block returning normally / NLR escape → abort(). The
      facade guarantees the entry block ends in `self complete`; document + file an issue
      for raw-prim robustness rather than expand scope here.

## Design
Each fiber records its parked C-stack pointer at suspend; the GC conservatively
scans every fiber's live C-stack span as a root source, in addition to the
already-precise per-fiber Newspeak stack trace. An intrusive registry
(`g_fiber_registry`, linked via a new `reg_next` field) enumerates all live
fibers including main. All new machinery is `#ifndef __ZEPHYR__`. The Zephyr
hijack is restored under `#ifdef __ZEPHYR__` (the design's stated intent).

### Tasks
1. **RED in harness.** Add `fibers-gc-stress` check to `run_tests_oevm.py`: build
   45_fibers Path A runner, run under `OEVM_GC_STRESS=1`, assert `ABABAB`/`done`.
   Confirm it FAILS (segfault) on current HEAD.
2. **Struct + registry + SP capture** (oevm.h, vm.c): add `c_stack_sp` and
   `reg_next` to struct Fiber (POSIX); register fibers (create + main lazy-init);
   capture parked SP in `primitive_fiber_switch` before `swapcontext`; init main's
   new fields (I3). Fix pointer-packing UB (I1).
3. **GC roots** (gc.c): mark `entry_closure`/`entry_receiver` in trace_one (C1);
   mark `g_fiber_switch_value` (M1); refactor `scan_c_stack` into a `[lo,hi)` range
   helper; fix active-fiber top (C3); scan all suspended fibers' C-stack spans (C2).
4. **Reclaim** (gc.c): free `c_stack` + unlink from registry in
   `reclaim_dead_resources` (C4). Verify RED test now GREEN; full suite 73/0/6.
5. **Zephyr re-fence** (vm.c): restore the 4 hijack blocks under `#ifdef __ZEPHYR__`;
   fix false comments (vm.c:766, 932-933). Best-effort `-D__ZEPHYR__` syntax sanity.
6. **Issues + comments**: file I2 (graceful fiber-return), the facade same-depth
   constraint (correct the kernel.ns comment), multi-cap `.oimg` divergence (extend
   ns-oevm#2), and the comment nits (loader serial-XOR-net, format_version enum).

Gate after task 4: `OEVM_GC_STRESS=1` fibers PASS + suite 73/0/6 both paths + portability-32.
