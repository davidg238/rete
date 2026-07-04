# Milestone A footprint delta (Task 10c)

Method: `tuvm/tools/footprint.sh` — host-proxy build (`gcc -Os -std=c99`), summing
`size -t` output over the engine objects (vm, dispatch, gc, kernel_boot, platform,
serial, udp, mirror, oimg_loader, and the posix backend objects). The .oimg/
solidified-image object is excluded (it scales with the program, not the VM). As
of Task 10c the script also totals `data+bss` from the same `size -t` output
(design addendum A2: a RAM regression is a gate failure like a text regression).

## Baseline re-measurement

The banked Task-1 baseline (`tuvm/tools/footprint-baseline.txt`, text=39148)
predates the RAM line, so the seed was re-measured with the *current*
footprint.sh for an apples-to-apples RAM figure. Method: `git -C
/home/david/workspace/tuvm worktree add /tmp/tuvm-seed fa301d3`, copy the current
`tools/footprint.sh` into the worktree, run it there against the seed's own
sources (same file set: vm.c, dispatch.c, gc.c, kernel_boot.c, platform.c,
serial.c, udp.c, mirror.c, oimg_loader.c, backend/posix/*.c — unchanged since the
seed), then `git worktree remove --force /tmp/tuvm-seed`.

**Sanity check (required by the task brief):** re-measured seed text = 39148,
exactly reproducing the banked Task-1 figure (0% deviation, well inside the ±1%
noise tolerance). Method is validated — the numbers below are safe to bank.

## Numbers

| | text | data+bss (RAM) |
|---|---:|---:|
| baseline (seed, fa301d3) | 39148 | 17253508 |
| after (HEAD, 943d9b5) | 35227 | 17252832 |
| delta | -3921 | -676 |
| delta % | -10.02% | -0.0039% |

(dec/hex totals and per-object breakdown: see `tuvm/tools/footprint-baseline.txt`
re-run — reproduced above — and `tuvm/tools/footprint-after.txt`.)

**Expectation check (spec §7):** the engine SHRANK on both axes. No STOP
triggered.

Caveat carried from the script's header: these are host-proxy builds, not the
target arm-zephyr-eabi. The RAM total in particular is dominated by gc.o's
host-side arena (`bss` = 17048384 bytes, identical baseline and after — a fixed
host build artifact, not Milestone-A-affected) which does not represent real MCU
static RAM and is why the RAM delta is tiny in percentage terms against the raw
total. Excluding gc.o's bss, the remaining data+bss baseline is 205124 and after
is 204448 — the same -676 byte delta (gc.o's bss did not move), but against a
much smaller denominator: -0.33%, same direction, clearer signal.

## Attribution (estimate — line-diffstat over src/*.c, not a per-commit footprint
run; labeled as such throughout)

The text bytes were not re-measured after each individual commit (that would
require N extra worktree+build round trips for a bookkeeping note); the
attribution below is a qualitative reading of `git log` + `git show --stat` for
the six deletion/renumber commits, ordered by expected code-size impact:

- **Fibers (Tasks 5-6)** — `f54ecde` (delete fiberCreate/fiberSwitch/currentFiber
  prims, Suspended/Dead/legacy vtables, concurrency image field: vm.c -125/+0
  net over 840 deletions across 9 files) + `1c4c58a` (delete struct Fiber,
  current_fiber, g_bootstrap_fiber wrapper: vm.c 157 lines touched, net -42).
  Largest single contributor — the scheduler/vtable/wrapper machinery was the
  biggest single subsystem removed.
- **NLR (Tasks 2-3)** — `346d557` (delete OP_NLR handler, do_unwind, the four
  unwind globals, resumeWith:/executeCleanup primitives, the baked
  MethodContext>>unwindTo:value: method): vm.c 186 lines touched (net -156
  excluding the regenerated solidified-image fixture, which is program data, not
  engine text). Second-largest contributor.
- **Growable stacks (Task 7)** — `185fc8a` (delete stack_ensure/grow, CAP/GROW
  macros, rebase preludes, straddle guard, dead reload blocks): vm.c -164 lines
  net (200 touched, 36 kept). A concentrated single-file cut, entirely in the
  hot interpreter loop.
- **Reify-on-escape E-series (Task 8n)** — `ebaff0b` (delete
  home_link_is_reified + reified-link decode branding): frame_abi.h + vm.c, only
  38 lines touched net — smallest of the five; most of the Context/closure
  machinery was *kept* per the 2026-07-04 decision, so this cut was narrow by
  design.
- **Renumber/registry compaction (9a/9b)** — `4e24ba2` (compact builtin
  registry + prim ids, drop reserved fiber/NLR rows) + `a025e47` (contiguous
  opcode renumber, drop the two dead SEND_IC/NLR gravestone opcodes) +
  `0f3124d` (oimg loader v8: drop the retired v7 concurrency header word). These
  are mostly *renumbering* (row/enum reshuffles), not raw deletion, so their
  direct text contribution is small; the opcode/gravestone removal in `a025e47`
  and the header-word drop in `0f3124d` are the parts with real byte impact
  (fewer reserved dispatch-table slots, a fixed-shape v8 header versus the v7
  format with a dead tail word).

Net: five deletion/compaction efforts across fibers, NLR, growable stacks, reify,
and the renumber, none individually re-measured, collectively account for the
full -3921 byte / -10.02% text shrink and the -676 byte / -0.33% RAM shrink
(gc.o's host-arena bss excluded from the RAM percentage; included, it is -676
bytes / -0.0039%). The ranking above (fibers > NLR > growable stacks > renumber
> reify) is a diffstat-based estimate, not a measured per-commit breakdown.
