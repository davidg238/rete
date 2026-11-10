# rete#477 — the arena census (2026-11-09)

*The tooling behind [`../../design/2026-11-09-477-arena-fragmentation-design.md`](../../design/2026-11-09-477-arena-fragmentation-design.md),
which holds the findings. The numbers are in #477's comments. Host only, `-m32` (the boards' layout), from master
`1001a48a`.*

| File | What it does |
|---|---|
| `build.sh OUTDIR [cflags]` | Takes `git archive HEAD` of `~/workspace/rete` and patches the census into a scratch `gc.c`. Builds the image VM. `SEG=1` adds the segregation spike; `KERNEL_FROM=` swaps in another `kernel.nsl`. |
| `census.inc` | The census: bytes allocated by class, live-at-peak by class, every object ≥ 1 KB, stack high-water. When memory runs out it prints the free-space picture (pieces, largest, per region under SEG). It prints at exit and on SIGTERM. |
| `seg.py` | The size-segregation spike, applied to the scratch `gc.c`. The measured answer: no gain. |
| `porta_census.sh` | `nodus/oevm/tests/porta_sync_host.sh` as of `1001a48a`, with three changes: it uses the census VM and the `-m32` layout, takes `NSL_OVERRIDE`, and stops the node with SIGTERM before step 8. |
| `run_app.sh TAG [cflags]` | Builds the VM and runs the check-in app. A pass is `stop=1`. |
| AWFY | `board.nsl` comes from `awfy_counts.py`'s `board` profile, as in `../2026-10-12-37-alloc-hdr/minarena.sh`. Run `OUTDIR/vm board.oimg` directly. |

Examples:

```
run_app.sh q32 -DVM_STACK_INIT_WORDS=1024 -DOEVM_HEAP_SIZE=32768   # baseline, passes
run_app.sh q28 -DVM_STACK_INIT_WORDS=1024 -DOEVM_HEAP_SIZE=28672   # baseline, fails: OOM 1032 B RawBytes
```

**Gotchas:**
- The harness takes the suite lock (`/tmp/rete-nsl-suite.lock`) and writes fixed `/tmp/porta_*` paths, so run
  one at a time.
- The stack high-water is the last nonzero word of the Stack. It was checked by undersizing the stack, which
  overflows as expected.
- At the first attempt, a worktree with no `node_modules` link failed to compile, and the old node log read as
  a result. `run_app.sh` now deletes the log first.
