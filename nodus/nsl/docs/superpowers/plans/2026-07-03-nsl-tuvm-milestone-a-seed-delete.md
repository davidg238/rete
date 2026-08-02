# Milestone A — Seed + Delete (the verified-simplification milestone) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Strip the fiber / NLR / reify-on-escape / growable-stack "balloon" out of the seeded `tuvm` VM and its `nsl-tuvm` compiler, leaving a plain `fixed-stack + current_frame + relative-SMI lexical-link` core that boots and runs a pared conformance subset green — and bank the engine-footprint delta.

**Architecture:** Delete the four mutually-entangled features **together** (they interlock: the `if (ctx != current_frame)` reload blocks serve both fiber-switch *and* stack-grow; the `unwind_target_reached` checks serve NLR), collapsing the entanglement into one simple execution model. Work is **feature-vertical**: each task changes the compiler and/or VM for one feature and is verified end-to-end by the conformance suite before the next. Behavioural deletion comes first (leaving enum/registry slots temporarily reserved so images keep building); a single final **clean renumber** compacts opcodes + the builtin registry across both repos once the live set is known.

**Tech Stack:** C99 VM (`~/workspace/tuvm`), Python 3 compiler + harness + Node/Ohm parser (`~/workspace/nsl-tuvm`), `.ns` conformance corpus (`~/workspace/nsl-tests`). Host-only (`TARGET=posix`), GNU `gcc`/`size`.

## Global Constraints

- **Repos (exact paths):** VM = `/home/david/workspace/tuvm`; compiler+stdlib+harness = `/home/david/workspace/nsl-tuvm`; conformance corpus = `/home/david/workspace/nsl-tests`. The originals `~/workspace/oevm` and `~/workspace/ns-oevm` are **FROZEN — never modify them** (historical record only).
- **No pre-fiber *commit* exists, but the pre-fiber *design* does:** `oevm`'s single root commit (`4465e36 "Phase 4 completed - Fiber Scheduling Architecture"`, verified: no other branches/tags/roots) already contains fibers + `OP_NLR` + `home_ctx` + `test_fibers.c`. Phases 1–3 (the clean pre-fiber core) were built before `git init`; only their **walkthrough docs** were committed (`docs/dev_process/phase{1,2,3}_walkthrough.md`, present in the `tuvm` seed). So deletion is the only path to the stripped core, and **those phase docs are the cross-check reference** for "does the stripped result match the intended pre-fiber shape?" (see the "Cross-validation" pass after Task 10).
- **Design authority:** `/home/david/workspace/nsl-tuvm/docs/design/2026-07-03-nsl-tuvm-first-step-design.md` §2, §4 (Milestone A), §5. This plan implements Milestone A only.
- **Locked decisions (David, 2026-07-03):** (1) **Deep refactor** — remove the `Fiber` wrapper entirely; the interpreter becomes plain `Stack` + `current_frame`. (2) **Delete the inert deferred-lift `Context`/upvalue machinery now** (it emits nothing and the reactive model designs away mutable upvalue capture). (3) **Clean renumber** — compact opcodes + builtin registry, bump `BUILTIN_REGISTRY_VERSION`, regenerate all images; no gravestone slots in the end state.
- **KEEP (do not touch as "balloon"):** the register interpreter core + the flat/heap-language-stack loop (`struct Stack`, `stack_alloc`, `push_frame`/`OP_RET`/`do_ret_deliver`/`exit_frame`, the relative-SMI dynamic + lexical link encoding), precise GC, `.oimg` loader + format, capability/confinement model, the `backend/<target>/` native seam (`platform`/`serial`/`udp`), the in-image mirror/debugger, and the vestigial access-control subsystem (`check_access_allowed`, `test_access.c`, `test_enclosures.c` — tracked separately as oevm#36, **out of scope**).
- **Run the suite SOLO and in the FOREGROUND** — the harness is not concurrency-safe (spec §7).
- **Every task ends green:** the pared conformance subset passes (Path A, and Path B where noted) and you commit. Never leave the tree red between tasks.
- **Provenance:** commit messages record what was deleted and why (Milestone A); the final task writes a provenance note.

---

## File Structure

**`tuvm` (VM, C):**
- `src/vm.c` — interpreter core; holds `stack_ensure`/grow, `OP_NLR`, `do_unwind`, the unwind globals, the fiber prims, the exec-context globals, the reload blocks, `home_ctx_frame`, the reify branches. **Heaviest edit surface.**
- `src/dispatch.c` — prim enum + `PRIM_NAMES` + dispatch switch; `context_alloc`; `make_closure`; the four fiber vtable globals.
- `src/gc.c` — root marking + `trace_one` for fiber vtables, unwind roots, `Context`.
- `src/oevm.h` — `struct Fiber`/`Closure`/`Context`/`MethodContext`, the opcode enum, prim prototypes.
- `src/frame_abi.h` — the L0 link encoding; strip the reified-`Context` discriminator, keep relative-SMI.
- `src/builtins.def` — frozen builtin registry (fiber vtables 6/12/13/14; NLR method symbols 15–19).
- `src/kernel_boot.c` — Model-B firmware boot: fiber vtable defs, `unwindTo:value:` bytecode method, `Symbol_unwindTo_value`.
- `src/solidified_image.c` (+ `.stmap`) — **generated**; regenerated by the compiler, never hand-edited except via regeneration.
- `src/oimg_loader.c` — `BUILTIN_REGISTRY_VERSION`; `img_concurrency_on_sel_ref` (format v7 fiber field).
- `src/prim_ids.h` — platform prim ids shifted +1 by the fiber prim insertion.
- `tests/` — delete `test_fibers.c`, `test_context.c`, `test_ctx_ops.c`, `test_stack.c`; update `test_frame_abi.c`; prune `test_debugger.c` fiber coupling.
- `tools/run_c_tests.sh` — **NEW** aggregate C-unit-test runner (none exists today).
- `tools/footprint.sh` — **NEW** engine-footprint measurer (compile engine objects, `size -t`, image excluded).

**`nsl-tuvm` (compiler + stdlib + harness, Python/Node/`.ns`):**
- `tools/ns_compile_oevm.py` — `OP_NLR` emit site (`~764–773`), opcode table (`OP_NLR = 5` at `:16`), `_contains_nlr` guard (`~2021–2038`, `~2082–2090`), `unwindTo:value:` install (`~1375–1377`) + symbol wiring (`~2238–2240`).
- `tools/kernel_methods.py` — the `unwindTo:value:` NLR bytecode method (delete).
- `tools/check_kernel_parity.py` — parity guard for the C copy of the NLR method (delete NLR bits).
- `tools/builtins_registry.py` + `tools/gen_builtins.py` — registry order (fiber vtables 6/12/13/14; NLR method slots).
- `tools/solidify.py` — fiber vtable extern/def/registration (`~114–116`, `125`, `257–291`).
- `tools/run_tests_oevm.py` — corpus glob + `OEVM_DIR`; the fiber/stack gates (`run_c_unit_test_fibers`, `run_fibers_gc_stress_gate`, `run_stack_overflow_cap_gate`, `run_growable_small_init_gate`).
- `lib/kernel.ns` — `class Concurrency` (`~270–323`), `class Fiber` (`~325–333`), `concurrency` factory (`~337`); `Collection>>detect:ifNone:`/`detect:` (`~49–50`, NLR-entangled — rewrite).
- `docs/superpowers/plans/pared-conformance-subset.md` — **NEW** the exclusion manifest (which `.ns` tests are out of the pared subset and why).

**`nsl-tests` (corpus):**
- Excluded from the pared subset (fiber/NLR/growable/host-deferred): `04_somns_basic/45_fibers.ns`, `55_fiber_capture.ns`, `59_grow_under_fibers.ns`, `58_growable_stack.ns`, `44_nlr_multiframe.ns`; already-`skip-oevm` networking `43_udp_mesh.ns`, `44_udp_thread.ns`. Kept-as-compile-fail: `03_blocks/03_nlr_compile_fail.ns` (now must fail for methods/modules too, not just scripts).

---

### Task 1: Green baseline — aggregate test harness, pared subset manifest, footprint baseline

Stand up the missing test/measurement infrastructure and prove the **seed** (pre-deletion) is green on the pared subset, so every later deletion is verifiable and the footprint delta has a reference. **No product code changes in this task.**

**Files:**
- Create: `/home/david/workspace/tuvm/tools/run_c_tests.sh`
- Create: `/home/david/workspace/tuvm/tools/footprint.sh`
- Create: `/home/david/workspace/nsl-tuvm/docs/superpowers/plans/pared-conformance-subset.md`
- Modify: `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py` (corpus glob → `../nsl-tests`; `OEVM_DIR` default → `../tuvm`)

**Interfaces:**
- Produces: `run_c_tests.sh` (runs KEEP C unit tests, exits non-zero on any failure), `footprint.sh` (prints `text` total of the engine object set, image excluded), and a recorded baseline footprint number in the commit message. Later tasks re-run both.

- [ ] **Step 1: Confirm the three repos exist and are on their seed commits**

Run:
```bash
for r in tuvm nsl-tuvm nsl-tests; do echo "== $r =="; git -C /home/david/workspace/$r log --oneline -1; done
```
Expected: `tuvm` → `fa301d3 seed: tuvm forked from oevm@aea0af1 …`; `nsl-tuvm` → `abd9915 seed: nsl-tuvm forked from ns-oevm@188426e …`; `nsl-tests` shows a seed commit. If any repo is missing or not a fresh seed, STOP and report — the plan assumes all three exist.

- [ ] **Step 2: Write the aggregate C-unit-test runner**

Create `/home/david/workspace/tuvm/tools/run_c_tests.sh`. It compiles+runs each KEEP test with the correct link-model (Model A links `solidified_image.c`; Model B links `kernel_boot.c`; backend-only tests link just the posix backend file), and fails on the first non-zero exit:
```bash
#!/usr/bin/env bash
# Aggregate C unit-test runner for tuvm (host/posix). No aggregate runner existed
# pre-Milestone-A; this is it. KEEP tests only — fiber/NLR/reify/stack tests are
# deleted in later Milestone-A tasks and must NOT be listed here.
set -u
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-gcc}; STD="-std=c99 -Isrc"; OUT=/tmp/tuvm_ctest; fail=0
ENG_A="src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/solidified_image.c"
ENG_B="src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c"
run() { # $1=name  $2=link-set  $3...=extra srcs
  local name=$1; shift; local link=$1; shift
  printf '%-28s ' "$name"
  if $CC $STD tests/$name.c $link "$@" -lm -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/tmp/tuvm_ctest.out 2>&1; then
    echo PASS
  else echo FAIL; sed 's/^/    /' /tmp/tuvm_ctest.log /tmp/tuvm_ctest.out; fail=1; fi
}
# Model-A-linked core tests
for t in test_arena test_coalesce test_float_arena test_float_gc test_gc_mark_iterative \
         test_gc_trigger test_method_cache test_int_compare test_oevm test_solidified \
         test_layout test_oimg; do run $t "$ENG_A"; done
# Model-B-linked tests
for t in test_kernel_boot test_millis test_platform; do run $t "$ENG_B"; done
# Backend-only smoke tests
run test_serial "src/serial.c src/backend/posix/serial_posix.c"
run test_udp    "src/udp.c src/backend/posix/udp_posix.c"
# Header-only frame ABI test
printf '%-28s ' test_frame_abi
if $CC $STD tests/test_frame_abi.c -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/dev/null 2>&1; then echo PASS; else echo FAIL; cat /tmp/tuvm_ctest.log; fail=1; fi
exit $fail
```
Then `chmod +x /home/david/workspace/tuvm/tools/run_c_tests.sh`.

- [ ] **Step 3: Run the C-unit runner against the seed and record which tests pass**

Run:
```bash
/home/david/workspace/tuvm/tools/run_c_tests.sh; echo "exit=$?"
```
Expected: all listed tests PASS, `exit=0`. (`test_oimg` links `gc.c` standalone with stubbed vtables — if it needs a different link-set, adjust its `run` line to match its header-comment command.) If any KEEP test fails on the untouched seed, STOP and report — that is a pre-existing breakage, not something this milestone should mask.

- [ ] **Step 4: Write the footprint measurer**

Create `/home/david/workspace/tuvm/tools/footprint.sh` — compiles the engine object set at `-Os` (image + libc **excluded**) and prints the `size -t` total; this is the number to bank a delta against:
```bash
#!/usr/bin/env bash
# Engine footprint (host proxy for arm-zephyr-eabi rom_report). Sums `text` of the
# engine objects; the .oimg/solidified image object is EXCLUDED (it scales with the
# program, not the VM). Run before and after the deletion; diff the total text.
set -eu
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-gcc}; TMP=$(mktemp -d)
$CC -Os -std=c99 -Isrc -c \
  src/vm.c src/dispatch.c src/gc.c src/kernel_boot.c src/platform.c \
  src/serial.c src/udp.c src/mirror.c src/oimg_loader.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c \
  src/backend/posix/udp_posix.c
size -t vm.o dispatch.o gc.o kernel_boot.o platform.o serial.o udp.o \
        mirror.o oimg_loader.o platform_posix.o serial_posix.o udp_posix.o
rm -f *.o; rmdir "$TMP" 2>/dev/null || true
```
Then `chmod +x /home/david/workspace/tuvm/tools/footprint.sh`.

- [ ] **Step 5: Bank the baseline footprint**

Run:
```bash
/home/david/workspace/tuvm/tools/footprint.sh | tee /home/david/workspace/tuvm/tools/footprint-baseline.txt
```
Expected: a `size -t` table; the final `(TOTALS)` row's **text** column is the seed engine footprint. This file is the reference for the Task 10 delta. Commit it.

- [ ] **Step 6: Rewire the conformance runner to the new repos**

In `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py`: change the corpus glob (reported near lines 642–648) from `../ns-tests` to `../nsl-tests`, and change the `OEVM_DIR` default (the VM checkout it builds in) from `../oevm`/`../ns-oevm` to `../tuvm`. Grep to find the exact strings:
```bash
grep -n "ns-tests\|OEVM_DIR\|\.\./oevm\|\.\./ns-oevm" /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py
```
Edit each occurrence so the runner compiles with the `tuvm` VM against the `nsl-tests` corpus.

- [ ] **Step 7: Write the pared-subset manifest**

Create `/home/david/workspace/nsl-tuvm/docs/superpowers/plans/pared-conformance-subset.md` listing every excluded test **with a one-line reason**, so "green" has a precise definition. Exclusions:
```markdown
# Pared conformance subset (Milestone A)
Host-only, non-fiber, non-NLR, non-growable. Excluded from "green":
- 04_somns_basic/45_fibers.ns          — fibers (deleted)
- 04_somns_basic/55_fiber_capture.ns   — fibers (deleted)
- 04_somns_basic/59_grow_under_fibers.ns — fibers + growable stack (deleted)
- 04_somns_basic/58_growable_stack.ns  — growable stack (deleted); depth 2000 needs realloc
- 04_somns_basic/44_nlr_multiframe.ns  — multi-frame NLR (deleted); ^ out of escaping closure
- 04_somns_basic/43_udp_mesh.ns        — networking, skip-oevm (host-deferred, spec §8)
- 04_somns_basic/44_udp_thread.ns      — networking, skip-oevm (host-deferred, spec §8)
Kept, but semantics change:
- 03_blocks/03_nlr_compile_fail.ns     — must now compile-FAIL for methods/modules too
Re-verify after the tuvm delete (VM-side reify removal may affect capture):
- 04_somns_basic/56_upward_closure.ns, 57_loop_var_capture.ns, 54_deep_recursion.ns (fixed cap)
```
Exclude these from the runner by adding `skip-oevm: true` to each excluded file's metadata header (the mechanism `run_test_file` already honours), OR by a runner-side skip list — pick the `skip-oevm:` route (self-documenting, lives with the test).

- [ ] **Step 8: Establish the conformance baseline on the seed**

Run the suite against the untouched seed with the excluded tests skipped:
```bash
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: the pared subset is GREEN (the fiber/NLR/growable tests report SKIP, everything else PASS). Note: fiber/NLR features still exist in the seed VM, so the non-excluded tests genuinely run and pass. Record the pass/skip/fail counts. If anything in the pared subset fails on the seed, STOP and report.

- [ ] **Step 9: Commit the baseline**

```bash
git -C /home/david/workspace/tuvm add tools/run_c_tests.sh tools/footprint.sh tools/footprint-baseline.txt
git -C /home/david/workspace/tuvm commit -m "test(milestone-a): aggregate C-test runner + footprint baseline

Bank the pre-deletion engine footprint and stand up a runnable KEEP-test
suite so the balloon deletion is verifiable at every step."
git -C /home/david/workspace/nsl-tuvm add tools/run_tests_oevm.py docs/superpowers/plans/pared-conformance-subset.md nsl-tests 2>/dev/null || true
git -C /home/david/workspace/nsl-tuvm commit -m "test(milestone-a): rewire runner to tuvm+nsl-tests; pared-subset manifest"
git -C /home/david/workspace/nsl-tests add -A && git -C /home/david/workspace/nsl-tests commit -m "test(milestone-a): skip-oevm the fiber/NLR/growable tests (pared subset)"
```

---

### Task 2: Remove NLR — compiler side

Make the compiler stop emitting `OP_NLR` and stop installing the `unwindTo:value:` runtime method, turn `^` inside an escaping closure into a **compile error** (spec §2), and rewrite the one NLR-entangled kernel combinator (`detect:ifNone:`) so `lib/kernel.ns` still compiles. Do the compiler side first so the VM handler simply goes unused (images keep building) before Task 3 removes it.

**Files:**
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (`detect:ifNone:`/`detect:`, ~49–50)
- Modify: `/home/david/workspace/nsl-tuvm/tools/ns_compile_oevm.py` (OP_NLR emit ~769–770; `_contains_nlr` guard ~2021–2038, ~2082–2090; `unwindTo:value:` install ~1375–1377; symbol wiring ~2238–2240)
- Delete: `/home/david/workspace/nsl-tuvm/tools/kernel_methods.py`
- Modify: `/home/david/workspace/nsl-tuvm/tools/check_kernel_parity.py` (drop the `unwindTo:value:` parity assertion)
- Test: `/home/david/workspace/nsl-tests/03_blocks/03_nlr_compile_fail.ns` (must still compile-fail)

**Interfaces:**
- Consumes: the pared-subset runner from Task 1.
- Produces: a compiler that emits `OP_RET` for method-level `^`, raises a compile error for `^` in an escaping (non-inlined) block, and no longer references `OP_NLR`, `kernel_methods`, or `unwindTo:value:`. The VM's `OP_NLR` handler still exists but is now dead (removed in Task 3).

- [ ] **Step 1: Write the failing test — `detect:ifNone:` must work without NLR**

Add a corpus test `/home/david/workspace/nsl-tests/04_somns_basic/60_detect_no_nlr.ns` exercising both hit and miss without any `^`-in-block:
```smalltalk
(*
expect:
3
none
*)
| c |
c:: OrderedCollection new.
c add: 1; add: 2; add: 3.
Transcript showln: (c detect: [ :e | e > 2 ] ifNone: [ 0 ]).
Transcript showln: (c detect: [ :e | e > 9 ] ifNone: [ #none ]).
```
(Match `Transcript`/`showln:` to the dialect's actual print selector — grep an existing `04_somns_basic/*.ns` for the print idiom and mirror it exactly.)

- [ ] **Step 2: Run it against the seed to confirm current behaviour**

```bash
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm \
  python3 /home/david/workspace/nsl-tuvm/tools/ns_compile_oevm.py 04_somns_basic/60_detect_no_nlr.ns /home/david/workspace/tuvm/src/solidified_image.c
```
Expected: compiles today (the seed still has NLR), so `detect:ifNone:` works via NLR. This confirms the test is valid before the rewrite.

- [ ] **Step 3: Rewrite `detect:ifNone:` off NLR (sentinel, no `^`-in-block)**

In `/home/david/workspace/nsl-tuvm/lib/kernel.ns`, replace the NLR-based body (currently `self do: [ :e | (b value: e) ifTrue: [ ^ e ] ]. ^ n value`) with a sentinel-carrying loop that returns at method level only:
```smalltalk
detect: b ifNone: n = (
  | found result |
  found:: false.
  self do: [ :e |
    (found not and: [ b value: e ]) ifTrue: [ found:: true. result:: e ] ].
  found ifTrue: [ ^ result ].
  ^ n value )
```
`detect: b` (~line 50) stays `^ self detect: b ifNone: [ self errorNotFound ]` — it delegates, so it inherits the fix. Confirm no other `lib/kernel.ns` method uses `^` inside a block passed to another method:
```bash
grep -n "\[ *\^" /home/david/workspace/nsl-tuvm/lib/kernel.ns
```
Expected after edit: no `^` inside a non-inlined block. (Matches inside `ifTrue:`/`whileTrue:`/`to:do:` inlined blocks are fine — those become `OP_RET`.)

- [ ] **Step 4: Make `^`-in-escaping-closure a compile error; stop emitting `OP_NLR`**

In `ns_compile_oevm.py`, at the emit site (~764–773) where `if "_block" in self.name:` currently emits `OP_NLR`, replace the NLR emission with a compile error:
```python
if "_block" in self.name:
    raise CompileError(
        "non-local return: '^' inside an escaping closure is not allowed "
        "(no-NLR model). Use a collection combinator or a sentinel-and-return.")
# method-level return:
self.emit_ABC(OP_RET, ...)   # unchanged args
```
Extend `_contains_nlr` usage (~2082–2090) so the rejection fires for **methods and modules**, not only top-level scripts — the guard should walk every non-inlined block body, not just script bodies. Then delete the now-unused `OP_NLR = 5` opcode constant reference at `:16` **only if nothing else reads it**; otherwise leave the constant defined but unemitted until the Task 9 renumber (grep first: `grep -n OP_NLR tools/*.py`).

- [ ] **Step 5: Stop installing the `unwindTo:value:` runtime method**

Remove the `add_unwind_to_value(...)` install call (~1375–1377) and the `Symbol_unwindTo_value`/`unwindTo:value:` symbol wiring (~2238–2240) in `ns_compile_oevm.py`. Delete `tools/kernel_methods.py`. In `tools/check_kernel_parity.py`, remove the assertion that compares the compiler's `unwindTo:value:` against the C copy (that C copy is deleted in Task 3). Grep to be sure nothing else imports the deleted module:
```bash
grep -rn "kernel_methods\|unwindTo\|add_unwind" /home/david/workspace/nsl-tuvm/tools/
```
Expected: no remaining references except the registry slot (handled in Task 9).

- [ ] **Step 6: Verify `detect:` works and NLR is now a compile error**

```bash
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `60_detect_no_nlr.ns` PASSES; `03_blocks/03_nlr_compile_fail.ns` still reports the expected compile-fail; `44_nlr_multiframe.ns` is SKIP (excluded). The rest of the pared subset stays green. If `03_nlr_compile_fail.ns` now fails for the wrong reason (e.g. crash instead of clean `CompileError`), fix the error path in Step 4.

- [ ] **Step 7: Commit**

```bash
git -C /home/david/workspace/nsl-tuvm add lib/kernel.ns tools/ns_compile_oevm.py tools/check_kernel_parity.py
git -C /home/david/workspace/nsl-tuvm rm tools/kernel_methods.py
git -C /home/david/workspace/nsl-tuvm commit -m "feat(milestone-a): drop NLR codegen; ^ in escaping closure is a compile error

Rewrite detect:ifNone: off NLR (sentinel); stop emitting OP_NLR and installing
unwindTo:value:. VM-side NLR removed in the next task."
git -C /home/david/workspace/nsl-tests add 04_somns_basic/60_detect_no_nlr.ns && \
  git -C /home/david/workspace/nsl-tests commit -m "test: detect:ifNone: without NLR"
```

---

### Task 3: Remove NLR — VM side

Delete the now-dead non-local-return machinery from `tuvm`: the `OP_NLR` handler, the `do_unwind` label, the unwind globals + GC roots, `resumeWith:`, the no-op `executeCleanup` seam, the five `unwind_target_reached` checks, and the `unwindTo:value:` bytecode method baked into `kernel_boot.c`. Leave the `OP_NLR` **enum slot reserved** (renamed) so opcode numbers don't shift until the Task 9 renumber.

**Files:**
- Modify: `/home/david/workspace/tuvm/src/vm.c` (globals `:77–80`, `:83`; `OP_NLR` handler `:708–773`; `do_unwind` `:1094–1152`; `resumeWith:` `:166–177`; `executeCleanup` `:156–164`; the five checks at `:640,:698,:769,:1009,:1058`; boot pinning of `Symbol_unwindTo_value` `:316,:340–343`)
- Modify: `/home/david/workspace/tuvm/src/gc.c` (`mark(Symbol_unwindTo_value)` `:689`; `mark(unwind_return_value)` `:723–727`)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (`OP_NLR` enum `:249` → reserve)
- Modify: `/home/david/workspace/tuvm/src/kernel_boot.c` (`unwindTo:value:` symbol + bytecode method + binding: `:58,:68–90,:105–107,:141,:166,:198`)
- Modify: `/home/david/workspace/tuvm/src/builtins.def` (NLR method symbols 15–19 — leave reserved for Task 9)

**Interfaces:**
- Consumes: a compiler (Task 2) that never emits `OP_NLR` and never sends `resumeWith:`/`unwindTo:value:`.
- Produces: a VM with no unwind channel. `OP_NLR` enum value renamed `OP_NLR_RETIRED` (reserved, unhandled). `builtins.def` fiber/NLR slots still present (compacted in Task 9). The `if (unwind_target_reached) goto do_unwind;` checks are gone from the send/return dispatch path.

- [ ] **Step 1: Reserve the opcode slot and delete the handler**

In `src/oevm.h:249`, rename `OP_NLR` to `OP_NLR_RETIRED` with a comment `/* reserved; compacted in Milestone A renumber */` (keeps every later opcode's number stable this task). In `src/vm.c`, delete the entire `case OP_NLR_RETIRED:` handler body (`:708–773`) — replace with `case OP_NLR_RETIRED: fprintf(stderr, "OP_NLR_RETIRED executed (should be unreachable)\n"); abort();` so a stale image fails loudly rather than silently.

- [ ] **Step 2: Delete `do_unwind` and the five unwind checks**

Delete the `do_unwind:` label block (`:1094–1152`). Delete each of the five `if (unwind_target_reached) goto do_unwind;` lines (`:640,:698,:769,:1009,:1058`). Delete the unwind globals (`:77–80`: `unwind_target_reached`, `unwind_return_value`, `unwind_target_frame`) and `Symbol_unwindTo_value` (`:83`) plus its lazy boot allocation (`:340–343`) and the boot-pinning predicate reference (`:316`). Delete `primitive_method_context_resume` (`resumeWith:`, `:166–177`) and the no-op `primitive_method_context_execute_cleanup` (`executeCleanup`, `:156–164`).

- [ ] **Step 3: Delete the unwind GC roots**

In `src/gc.c`, delete `mark(Symbol_unwindTo_value)` (`:689`) and the `mark(unwind_return_value)` root (`:723–727`). Confirm no dangling `extern` of the deleted globals remains:
```bash
grep -rn "unwind_target_reached\|unwind_return_value\|unwind_target_frame\|Symbol_unwindTo_value\|execute_cleanup\|context_resume" /home/david/workspace/tuvm/src/
```
Expected: no hits (except possibly a comment). Remove any leftover `extern` declarations in `oevm.h`.

- [ ] **Step 4: Delete the baked `unwindTo:value:` bytecode method from Model-B boot**

In `src/kernel_boot.c`, delete the `unwindTo:value:` symbol constant (`:58`), the bytecode method body (`:68–90`), its binding into `MethodContextVtable` (`:105–107`), and the `Symbol_unwindTo_value` wiring (`:141,:166,:198`). Leave the `builtins.def` symbol rows 15–19 in place for now (Task 9 compacts them); the registry index must stay valid so `test_kernel_boot` still asserts the right count until the renumber.

- [ ] **Step 5: Build the VM and run the C unit tests**

```bash
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm && echo BUILD_OK
./tools/run_c_tests.sh; echo "exit=$?"
```
Expected: `BUILD_OK`; `run_c_tests.sh` all PASS (`test_fibers`/`test_context`/`test_stack` are not in the runner). Note `test_frame_abi` still asserts the reified discriminator — that's fine until Task 8.

- [ ] **Step 6: Regenerate the image and run the conformance suite**

```bash
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: pared subset GREEN. (The compiler no longer emits `OP_NLR`, so the retired handler is never hit.) If any test aborts with "OP_NLR_RETIRED executed", the compiler is still emitting it — return to Task 2 Step 4.

- [ ] **Step 7: Commit**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "feat(milestone-a): delete NLR unwind machinery from the VM

Remove OP_NLR handler, do_unwind, unwind globals+roots, resumeWith:,
executeCleanup, and the baked unwindTo:value: method. Opcode slot reserved
(OP_NLR_RETIRED) pending the Milestone-A renumber."
```

---

### Task 4: Remove fibers — compiler, stdlib, and harness side

Delete the `Concurrency`/`Fiber` stdlib classes and the fiber builtin-vtable definitions the compiler emits, and remove the fiber/growable-stack gates from the harness. Do this before the VM prim deletion (Task 5) so the compiler stops referencing fiber prims and stops emitting the fiber vtables that Task 5 removes.

**Files:**
- Modify: `/home/david/workspace/nsl-tuvm/lib/kernel.ns` (delete `class Concurrency` ~270–323, `class Fiber` ~325–333, `concurrency` factory ~337)
- Modify: `/home/david/workspace/nsl-tuvm/tools/solidify.py` (fiber vtable extern/def/registration ~114–116, 125, 257–291)
- Modify: `/home/david/workspace/nsl-tuvm/tools/builtins_registry.py` (fiber vtable rows 6/12/13/14 — leave reserved for Task 9) and regenerate via `tools/gen_builtins.py`
- Modify: `/home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py` (delete `run_c_unit_test_fibers` ~128–147, `run_fibers_gc_stress_gate` ~150–210, `run_stack_overflow_cap_gate`, `run_growable_small_init_gate` ~284/~330, and their calls in `suite_ok` ~724,740,742,749,757–759)

**Interfaces:**
- Consumes: the Task 1 runner.
- Produces: a compiler that emits no fiber vtables (registration removed) and a stdlib with no `Concurrency`/`Fiber`; a harness with no fiber/growable gates. The `builtins_registry` fiber slots stay as reserved placeholders until Task 9.

- [ ] **Step 1: Delete the `Concurrency`/`Fiber` classes and factory from the stdlib**

In `/home/david/workspace/nsl-tuvm/lib/kernel.ns`, delete `class Concurrency` (~270–323), `class Fiber` (~325–333), and the `concurrency = ( ^ Concurrency onKernel: self )` factory (~337). Grep for stragglers referencing the removed prims/classes:
```bash
grep -n "Concurrency\|Fiber\|fiberCreate\|fiberSwitch\|currentFiber\|concurrency" /home/david/workspace/nsl-tuvm/lib/kernel.ns
```
Expected: no remaining references.

- [ ] **Step 2: Remove fiber-vtable emission from the solidifier**

In `tools/solidify.py`, delete the fiber vtable extern decls (~114–116), remove the fiber entries from the system-vtable set (~125), and delete the definition/registration block (~257–291, including `compiler.add_vtable("FiberVtable")` at ~291). In `tools/builtins_registry.py`, comment the fiber vtable rows (indices 6/12/13/14) as `RESERVED (Milestone A renumber)` so numbering holds this task; regenerate:
```bash
cd /home/david/workspace/nsl-tuvm && python3 tools/gen_builtins.py && \
  grep -n "Fiber" tools/builtins_registry.py
```
Expected: fiber rows marked reserved, no live fiber vtable registration.

- [ ] **Step 3: Delete the fiber/growable harness gates**

In `tools/run_tests_oevm.py`, delete the four gate functions (`run_c_unit_test_fibers`, `run_fibers_gc_stress_gate`, `run_stack_overflow_cap_gate`, `run_growable_small_init_gate`) and every reference to them in `suite_ok` (~724,740,742,749,757–759). Grep to confirm:
```bash
grep -n "fiber\|growable\|stack_overflow\|test_fibers\|45_fibers\|58_growable" /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py
```
Expected: no hits.

- [ ] **Step 4: Regenerate and run the conformance suite**

```bash
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: pared subset GREEN; no fiber gate output. The VM still defines fiber prims/vtables (removed in Task 5) but the compiler no longer registers them — the image just doesn't use them.

- [ ] **Step 5: Commit**

```bash
git -C /home/david/workspace/nsl-tuvm add -A && git -C /home/david/workspace/nsl-tuvm commit -m "feat(milestone-a): remove Concurrency/Fiber stdlib + fiber gates

Delete the cooperative-scheduler classes, stop emitting fiber vtables, and
drop the fiber/growable harness gates. VM prims removed next."
```

---

### Task 5: Remove fibers — VM prim, scheduler, and lifecycle-vtable side

Delete the fiber primitives (`fiberCreate:`, `fiberSwitch:value:`, `currentFiber`), the three lifecycle vtables (`Runnable`/`Suspended`/`Dead`) plus the legacy `FiberVtable`, their GC marking/tracing, and the `img_concurrency_on_sel_ref` v7 boot field. Leave `struct Fiber`/`current_fiber` as the **sole retained execution context** (fiber 0) — the deep refactor that removes the wrapper entirely is Task 6.

**Files:**
- Modify: `/home/david/workspace/tuvm/src/vm.c` (`primitive_fiber_create` `:1304–1374`, `primitive_fiber_switch` `:1376–1422`)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (prim enum `PRIM_fiberCreate/Switch/currentFiber` ~565–566; `PRIM_NAMES` ~590; dispatch cases `:774–788`; vtable globals `:18,27–30`)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (fiber prim prototypes `:405–408`; vtable externs `:316,323–325`)
- Modify: `/home/david/workspace/tuvm/src/gc.c` (fiber vtable externs `:464–467`; the 4-way vtable test in `trace_one` `:485–496`; `mark(*FiberVtable)` `:671,683,693–702`)
- Modify: `/home/david/workspace/tuvm/src/kernel_boot.c` (fiber vtable fwd decls `:39–41`, defs `:122–124`, global assigns `:153–155`, registry `:184,190–192`)
- Modify: `/home/david/workspace/tuvm/src/builtins.def` (fiber vtable rows 6/12/13/14 — leave reserved for Task 9)
- Modify: `/home/david/workspace/tuvm/src/oimg_loader.c` (`img_concurrency_on_sel_ref` `:97–98,153,228–231`; format v7 gate)
- Modify: `/home/david/workspace/tuvm/src/prim_ids.h` (undo the fiber-prim `+1` shift comment/numbering — reserved until Task 9)

**Interfaces:**
- Consumes: a compiler (Task 4) that registers no fiber vtables and sends no fiber prims.
- Produces: a VM with no fiber prims/scheduler; `current_fiber` remains the single execution context on the (single) `RunnableFiberVtable`. `img_concurrency_on_sel_ref` gone; loader ignores/rejects v7-only fiber field. Registry rows reserved (Task 9 compacts).

- [ ] **Step 1: Delete the fiber prims and their dispatch**

In `src/vm.c`, delete `primitive_fiber_create` (`:1304–1374`) and `primitive_fiber_switch` (`:1376–1422`). In `src/dispatch.c`, delete the `case PRIM_fiberCreate`/`PRIM_fiberSwitch`/`PRIM_currentFiber` dispatch arms (`:774–788`), remove the three `PRIM_NAMES` strings (`:590`), and remove the three enum entries (~565–566) — **but leave a reserved placeholder comment** so following prim ids don't shift until Task 9 (or shift them now and fix `prim_ids.h` + the compiler's prim table together — defer that to the Task 9 renumber to avoid a cross-repo sync mid-milestone). In `oevm.h`, delete the fiber prim prototypes (`:405–408`).

- [ ] **Step 2: Collapse the four fiber vtables to one retained execution-context vtable**

The interpreter still needs a vtable for `current_fiber` (fiber 0) until Task 6. Keep `RunnableFiberVtable` as the sole execution-context vtable; delete `SuspendedFiberVtable`, `DeadFiberVtable`, and legacy `FiberVtable`. Edit: `dispatch.c` globals (`:18,27–30`) — keep only `RunnableFiberVtable`; `oevm.h` externs (`:316,323–325`) — keep only `RunnableFiberVtable`; `kernel_boot.c` (`:39–41,122–124,153–155,190–192`) — keep only the Runnable def/assign/registry row. In `builtins.def`, mark rows 6/13/14 (FiberVtable/Suspended/Dead) `RESERVED`; keep row 12 (Runnable) live.

- [ ] **Step 3: Fix GC marking/tracing for the collapsed vtable set**

In `src/gc.c`: reduce the `trace_one` 4-way fiber-vtable test (`:485–496`) to the single `RunnableFiberVtable` case (still marks `fib->status`, `fib->stack_obj`); delete the `mark()` lines for the three removed vtables (`:693–702`) and the removed externs (`:464–467`), keeping `mark(RunnableFiberVtable)` and `mark(current_fiber)`. The `g_bootstrap_fiber` rooting rationale comment (`:758–777`) can be trimmed to "roots the sole execution context" (its scheduler justification is now moot).

- [ ] **Step 4: Remove the fiber image-boot field**

In `src/oimg_loader.c`, delete `img_concurrency_on_sel_ref` (`:97–98`), its read (`:153`), and the concurrency-capability grant (`:228–231`). Decide the v7 gate: since we clean-renumber and regenerate all images (Task 9), **drop v7 parsing** — treat the field as absent. Confirm no other reader:
```bash
grep -rn "concurrency_on_sel\|format_version >= 7\|fiberCreate\|fiberSwitch\|SuspendedFiber\|DeadFiberVtable\|FiberVtable" /home/david/workspace/tuvm/src/
```
Expected: only `RunnableFiberVtable` (the retained exec context) and reserved-slot comments remain.

- [ ] **Step 5: Build and run both test layers**

```bash
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm && echo BUILD_OK
./tools/run_c_tests.sh; echo "exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `BUILD_OK`; C tests PASS; conformance pared subset GREEN.

- [ ] **Step 6: Commit**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "feat(milestone-a): delete fiber prims, scheduler, lifecycle vtables

Remove fiberCreate/fiberSwitch/currentFiber prims, Suspended/Dead/legacy
FiberVtable, the concurrency image field. current_fiber (fiber 0) retained as
the sole execution context on RunnableFiberVtable; wrapper removed next task."
```

---

### Task 6: Deep refactor — remove the `Fiber` wrapper; exec context becomes `Stack` + `current_frame`

Eliminate the `struct Fiber` execution-context wrapper. After this task the running state is exactly: `g_active_stack` (the `Stack` heap object, a GC root), `vm_stack`/`vm_stack_ptr` (its data + cursor), and `current_frame`. Rehome the debugger single-step fields onto the retained state. Simplify the `if (ctx != current_frame)` reload blocks — with fibers gone their only remaining job is stack-object identity after a (soon-to-be-removed) grow.

**Files:**
- Modify: `/home/david/workspace/tuvm/src/vm.c` (globals `:50–57`; lazy bootstrap `:322–336`; the reload blocks `:616–636,680–700,~993,~1040`; OP_RET/exit paths `:1155,1191–1194`; single-step fields usage)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (`struct Fiber` `:196–207`; `current_fiber` extern `:327`; single-step prim proto `:372`)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (`RunnableFiberVtable` global `:27` → rename/retire; `current_fiber` `:30`)
- Modify: `/home/david/workspace/tuvm/src/gc.c` (`current_fiber`/`g_bootstrap_fiber` roots `:95,699–705,729–731,758–777`; `trace_one` exec-context case `:485–496`)
- Modify: `/home/david/workspace/tuvm/src/kernel_boot.c` (drop the last fiber-vtable registration)
- Modify: `/home/david/workspace/tuvm/src/vm.c`, `src/mirror.c` (debugger single-step: `primitive_fiber_set_single_step` `:1584–1605`, `mirror.c:77` `fiber:singleStep:`)

**Interfaces:**
- Consumes: a VM (Task 5) where `current_fiber` is the only remaining `Fiber` and no scheduler switches it.
- Produces: no `struct Fiber`, no `current_fiber`/`g_bootstrap_fiber`. New/retained exec-context state: `oop g_active_stack`, `oop *vm_stack`, `size_t vm_stack_ptr`, `struct MethodContext *current_frame`, plus `int g_single_step`/`int g_hook_step_request` moved off the old `Fiber` struct to file-scope globals. Debugger selector `fiber:singleStep:` renamed `singleStep:` (or kept as an alias). GC roots `g_active_stack` directly.

- [ ] **Step 1: Move single-step state off `struct Fiber`**

The `single_step` and `hook_step_request` fields of `struct Fiber` (`oevm.h:197`) implement the debugger, not scheduling. Introduce file-scope globals in `vm.c`: `int g_single_step = 0; int g_hook_step_request = 0;`. Update `primitive_fiber_set_single_step` (`vm.c:1584–1605`) and `primitive_request_step` to read/write the globals. In `mirror.c:77`, keep the reflective entry but rename its selector `fiber:singleStep:` → `singleStep:` (grep the compiler/tests for senders and update, or keep `fiber:singleStep:` as a deprecated alias to avoid touching `test_debugger.c` yet):
```bash
grep -rn "fiber:singleStep\|singleStep\|single_step\|hook_step_request" /home/david/workspace/tuvm/src/ /home/david/workspace/nsl-tuvm/
```

- [ ] **Step 2: Replace the `Fiber` exec-context globals with plain stack state**

In `vm.c:50–57`, the state is already mostly stack-centric (`g_active_stack`, `vm_stack`, `vm_stack_ptr`, `current_frame`). Delete `g_bootstrap_fiber` (`:51–55`) and the `current_fiber` global (`dispatch.c:30`, `oevm.h:327`). Everywhere `((struct Fiber*)current_fiber)->stack_obj` is read (e.g. the grow repoint `vm.c:235`, and the bootstrap), replace with `g_active_stack` directly. Delete `struct Fiber` from `oevm.h:196–207`.

- [ ] **Step 3: Rewrite the lazy bootstrap to allocate a `Stack`, not a `Fiber`**

At `vm.c:322–336`, the bootstrap currently does `current_fiber = gc_allocate(RunnableFiberVtable, …)` then wires its `stack_obj`. Replace with a direct `Stack` allocation:
```c
/* Lazy exec-context bootstrap (was "fiber 0"): a single Stack object + cursor.
   No Fiber wrapper — the flat loop runs directly on g_active_stack. */
if (g_active_stack == NULL) {
    g_active_stack = stack_alloc(VM_STACK_INIT_WORDS);   /* existing core allocator */
    vm_stack     = ((struct Stack *)g_active_stack)->data;
    vm_stack_ptr = 0;
    current_frame = NULL;
}
```
Retire `RunnableFiberVtable` (the last fiber vtable): remove its def/assign/registry row from `kernel_boot.c` and its global from `dispatch.c:27`. `Stack` uses the existing `StackVtable` (unchanged, KEEP).

- [ ] **Step 4: Simplify the reload blocks**

At the four `if (ctx != current_frame)` reload sites (`vm.c:616–636` OP_SEND, `:680–700` OP_BLOCK_INVOKE, `~993` OP_SEND_IMPLICIT, `~1040` OP_SEND_SUPER), the branch previously recovered from *either* a fiber switch *or* a Phase-3 stack grow (see the comment at `:618–622`). Fibers are gone; growable stacks are removed in Task 7. For **this** task keep the branch but delete the fiber-switch half of its logic and its comment, leaving only the stack-object/`current_frame` reload (which Task 7 removes entirely once the stack is fixed-capacity). Update the `:618–622` comment to note the grow-recovery is pending removal in the growable-stack task.

- [ ] **Step 5: Fix GC roots**

In `gc.c`: delete the `current_fiber` extern/root (`:95,699–705,729–731`) and the `g_bootstrap_fiber` root + rationale (`:758–777`); add `mark(g_active_stack)` as the execution-context root (if not already marked via the Stack path — verify the Stack tracer at `:497–540` covers frames). Reduce/delete the `trace_one` fiber-vtable case (`:485–496`) — the exec context is now a plain `Stack`, traced by the existing Stack tracer. Grep:
```bash
grep -rn "current_fiber\|g_bootstrap_fiber\|struct Fiber\|RunnableFiberVtable" /home/david/workspace/tuvm/src/
```
Expected: **zero** hits (the wrapper is fully gone).

- [ ] **Step 6: Build and run both test layers**

```bash
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm && echo BUILD_OK
./tools/run_c_tests.sh; echo "exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `BUILD_OK`; C tests PASS (`test_debugger` still green if the `fiber:singleStep:` alias was kept); conformance GREEN. This is the highest-risk task — if a GC-stress conformance test crashes, suspect the `g_active_stack` rooting in Step 5 (the exec context must stay a precise root).

- [ ] **Step 7: Commit**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "refactor(milestone-a): remove the Fiber wrapper; exec context = Stack + current_frame

Delete struct Fiber, current_fiber, g_bootstrap_fiber. The flat loop now runs
directly on g_active_stack. Debugger single-step state moved to file globals."
```

---

### Task 7: Remove growable stacks

Delete `stack_ensure`/grow, the CAP/GROW config macros (keep `VM_STACK_INIT_WORDS` as the fixed size), the in-frame rebase preludes, and the grow×NLR straddle guard. Replace the grow decision with a fixed-capacity overflow abort. With fibers already gone, the reload blocks from Task 6 collapse fully.

**Files:**
- Modify: `/home/david/workspace/tuvm/src/vm.c` (`stack_ensure` `:188–237`; config macros `:41–49`; grow preludes `:243–250,355–362`; grow docstring `:182–197`; the reload blocks from Task 6; straddle guard remnants)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (`struct Stack.capacity` — keep field, stop using it for grow)

**Interfaces:**
- Consumes: a VM (Task 6) whose exec context is a single `Stack`.
- Produces: a fixed-capacity stack (`VM_STACK_INIT_WORDS`); pushing past it calls a new `stack_overflow_abort()`. No realloc/rebase anywhere. The `if (ctx != current_frame)` reload blocks are removed (a fixed stack never moves).

- [ ] **Step 1: Replace `stack_ensure` with a fixed-cap bounds check**

Delete `stack_ensure` (`vm.c:188–237`) and its grow docstring (`:182–197`). Add a small overflow abort:
```c
static void stack_overflow_abort(size_t need, size_t cap) {
    fprintf(stderr, "Stack Overflow: need %zu words, cap %zu (fixed stack, "
                    "no grow in the no-NLR runtime)\n", need, cap);
    abort();
}
```
Delete the CAP/GROW macros (`vm.c:41–49`: `VM_STACK_CAP_WORDS`, `VM_STACK_GROW_NUM`, `VM_STACK_GROW_DEN`); keep `VM_STACK_INIT_WORDS` (`:32–40`) as the fixed capacity.

- [ ] **Step 2: Replace the two grow call sites with the bounds check**

At `push_frame` (`vm.c:243–250`) and in `run_interpreter` (`:355–362`), delete the "capture old base/cap → call `stack_ensure` → rebase in-stack `args` by `delta`" preludes. Replace each with a pre-push bounds test against the fixed cap:
```c
if (vm_stack_ptr + frame_words > ((struct Stack *)g_active_stack)->capacity)
    stack_overflow_abort(vm_stack_ptr + frame_words,
                         ((struct Stack *)g_active_stack)->capacity);
```
(Use the actual local names for frame size at each site.) Since the stack never moves, no rebase is needed.

- [ ] **Step 3: Delete the reload blocks and the straddle guard**

The `if (ctx != current_frame)` reload blocks (trimmed in Task 6) exist only to recover a moved stack — a fixed stack never moves, so delete them entirely at all four sites (OP_SEND/OP_BLOCK_INVOKE/OP_SEND_IMPLICIT/OP_SEND_SUPER). Delete the "unwind target stranded by stack growth" straddle guard remnant (was `vm.c:1103–1120`, already thinned by the Task 3 `do_unwind` deletion — confirm nothing references it). Grep:
```bash
grep -rn "stack_ensure\|VM_STACK_CAP\|VM_STACK_GROW\|straddle\|rebase\|delta =" /home/david/workspace/tuvm/src/vm.c
```
Expected: no grow/rebase/straddle hits.

- [ ] **Step 4: Build and run both test layers**

```bash
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm && echo BUILD_OK
./tools/run_c_tests.sh; echo "exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `BUILD_OK`; C tests PASS; conformance GREEN. `58_growable_stack.ns` (depth 2000) stays SKIP. Verify `54_deep_recursion.ns` (depth 800) still fits under `VM_STACK_INIT_WORDS` — if it now overflows, note it in the subset manifest and either raise `VM_STACK_INIT_WORDS` to the documented ~16384 words or exclude it (record the choice).

- [ ] **Step 5: Commit**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "feat(milestone-a): delete growable stacks; fixed-capacity stack

Remove stack_ensure/grow, CAP/GROW macros, rebase preludes, straddle guard,
and the now-dead reload blocks. Overflow aborts loudly. INIT_WORDS is the fixed
size."
```

---

### Task 8: Remove reify-on-escape (E-series) + the inert deferred-lift `Context` layer

Delete the reified-`Context` discriminator branch and the inert deferred-lift upvalue machinery (locked decision: it emits nothing and the reactive model designs it away). Keep the relative-SMI lexical link (`home_ctx` as `TAG_SMI(offset)`) that `OP_LD_LEXICAL`/`primitive_closure_home` rely on.

**Files:**
- Modify: `/home/david/workspace/tuvm/src/frame_abi.h` (delete `home_link_is_reified` `:58`, the reified `struct Context` section `:69–82`; keep relative encode/decode `:56–67`)
- Modify: `/home/david/workspace/tuvm/src/vm.c` (`home_ctx_frame` reified branch `:282–286`; `OP_MK_CONTEXT`/`OP_LD_CTX`/`OP_ST_CTX` `:557–576`; `get_top_level_method_closure` E1 note `:1424–1444` — keep function, drop the "E-series" wording)
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (`struct Context` fwd `:225–229`; `OP_MK_CONTEXT/LD_CTX/ST_CTX` enum `:271–281`; `Closure.home_generation` `:142`, `Closure.captured_env` `:144`)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (`ContextVtable` global `:17,20`; `context_alloc` `:63–87`; `make_closure` `home_generation`/`captured_env` writes `:643,645`)
- Modify: `/home/david/workspace/tuvm/src/gc.c` (`Context` trace branch `:526–535`; `ContextVtable` mark `:697–704`)
- Modify: `/home/david/workspace/tuvm/src/kernel_boot.c` + `builtins.def` (ContextVtable registry row — reserve for Task 9)

**Interfaces:**
- Consumes: a compiler that emits none of `OP_MK_CONTEXT`/`OP_LD_CTX`/`OP_ST_CTX` (confirmed inert by the exploration) and a VM (Task 6) with a fixed relative-SMI link.
- Produces: `home_ctx` is now a total 2-way discriminator (`NULL` | relative-SMI); no `struct Context`, no `ContextVtable`, no `context_alloc`, no `home_generation`/`captured_env`. `OP_MK_CONTEXT`/`OP_LD_CTX`/`OP_ST_CTX` enum values reserved (Task 9 compacts).

- [ ] **Step 1: Confirm the compiler emits none of the Context opcodes**

```bash
grep -rn "OP_MK_CONTEXT\|OP_LD_CTX\|OP_ST_CTX\|MK_CONTEXT\|captured_env\|home_generation" /home/david/workspace/nsl-tuvm/tools/
```
Expected: no emit sites (only, at most, an opcode constant table entry). If any emit site exists, STOP — the decision to delete assumed these are inert.

- [ ] **Step 2: Strip the reified branch from the link decode and `frame_abi.h`**

In `frame_abi.h`: delete `home_link_is_reified` (`:58`) and the entire reified `struct Context` section (`:69–82`); update the header doc so the `home_ctx` discriminator is documented as 2-way (`NULL` | relative-SMI). In `vm.c` `home_ctx_frame` (`:272–287`), delete the reified-Context branch (`:282–286`, the `fprintf`+`abort`); keep the `NULL` and relative-SMI branches (the load-bearing decode).

- [ ] **Step 3: Delete the deferred-lift Context opcodes and machinery**

In `vm.c`, delete the `OP_MK_CONTEXT`/`OP_LD_CTX`/`OP_ST_CTX` handlers (`:557–576`). In `oevm.h`, reserve those enum values (rename `_RETIRED`, comment) and delete the `struct Context` fwd decl (`:225–229`) and the `Closure` field `captured_env` (`:144`). In `dispatch.c`, delete `context_alloc` (`:63–87`), the `ContextVtable` global (`:17,20`), and the `captured_env` write in `make_closure` (`:645`). In `gc.c`, delete the `Context` trace branch (`:526–535`) and `mark(ContextVtable)` (`:697–704`). Mark the `ContextVtable` `builtins.def`/`kernel_boot.c` registry row RESERVED.

> **CAUTION — `home_generation` is NOT necessarily reify-only (pre-fiber cross-check).** The Phase 1 design doc (`docs/dev_process/phase1_walkthrough.md`) describes `struct Closure`'s `home_generation` as a "generation lifecycle tag" present in the **pre-fiber core**, and it pairs with the `current_generation_tag` "lifecycle protection" global (`vm.c:75`) — a stale-home-frame guard, independent of reify. The reactive model makes this guard *more* relevant (a `whenever:do:` handler closure outlives its setup frame, so a relative-SMI `home_ctx` into a returned frame is exactly the dangling access this tag detects), and we have just removed the reified `Context` that was the other half of that safety story. **Do NOT delete `home_generation` reflexively.** First determine whether it is genuinely dead: `grep -rn "home_generation\|current_generation_tag\|generation_tag" src/`. If it is only *written* (never read to guard an access), removing it is safe. If any read guards a closure/home access, **KEEP the field and the writer** (`make_closure:643`) and delete only the reify-specific *interpretation*; record the decision in the Task 10 provenance note. When in doubt, keep it — the field is near-free and re-removing later is cheap; a lost stale-home guard is a latent memory bug.

- [ ] **Step 3b: Confirm the closure/lexical core still matches the pre-fiber design**

Cross-read `docs/dev_process/phase1_walkthrough.md` (struct `Object`/`Symbol`/`Vtable`/`MethodContext`/`Closure`/`InlineCache` layout) and `phase2_walkthrough.md` against the post-strip `oevm.h`. Confirm every struct the pre-fiber design lists still exists with its non-fiber fields intact (the strip removed `struct Fiber` and the reify additions, not the Phase 1–2 core). `test_layout.c` enforces these offsets — it must stay green.

- [ ] **Step 4: De-brand the retained E1 access helper**

`get_top_level_method_closure` (`vm.c:1424–1444`) is now just an invariant check (`home_ctx == NULL` for top-level method closures) used by the vestigial access subsystem (oevm#36, out of scope — KEEP). Remove only the "Reify-on-escape E1" wording from its comment; leave the function. Confirm the reify surface is gone:
```bash
grep -rn "reify\|home_generation\|struct Context\|ContextVtable\|context_alloc\|home_link_is_reified" /home/david/workspace/tuvm/src/
```
Expected: no live references (comments referencing the retired feature history are acceptable).

- [ ] **Step 5: Build and run both test layers**

```bash
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/backend/posix/platform_posix.c src/serial.c src/backend/posix/serial_posix.c \
    src/udp.c src/backend/posix/udp_posix.c src/mirror.c src/oimg_loader.c \
    src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm && echo BUILD_OK
./tools/run_c_tests.sh; echo "exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `BUILD_OK`; conformance GREEN including `56_upward_closure.ns`/`57_loop_var_capture.ns` (they capture via the relative link, not the reified Context — if either now fails, the closure capture path depended on `captured_env`; re-verify Step 3 kept `home_ctx`-based capture intact). `test_frame_abi` may now FAIL its reified-discriminator assertion — that is expected and fixed in Task 10.

- [ ] **Step 6: Commit**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "feat(milestone-a): delete reify-on-escape + inert deferred-lift Context

home_ctx is now a total 2-way discriminator (NULL | relative-SMI). Remove
struct Context, ContextVtable, context_alloc, home_generation, captured_env,
and the MK/LD/ST_CTX opcodes (reserved pending renumber)."
```

---

### Task 9: Clean renumber — compact opcodes + builtin registry across both repos

With the live opcode/builtin set now known, compact away every reserved slot in one coordinated change, bump `BUILTIN_REGISTRY_VERSION`, and regenerate all images. This is a single atomic cross-repo change (the compiler's opcode/registry constants must match the VM exactly, or every image is garbage).

**Files:**
- Modify: `/home/david/workspace/tuvm/src/oevm.h` (opcode enum — remove `_RETIRED` slots, renumber contiguously)
- Modify: `/home/david/workspace/tuvm/src/dispatch.c` (`PRIM_*` enum + `PRIM_NAMES` — compact fiber prim gaps) and `src/prim_ids.h` (undo the `+1` fiber shift)
- Modify: `/home/david/workspace/tuvm/src/builtins.def` (remove reserved fiber/NLR/Context rows; renumber) and `src/oimg_loader.c` (`BUILTIN_REGISTRY_VERSION` bump)
- Modify: `/home/david/workspace/tuvm/src/kernel_boot.c` (registry indices)
- Modify: `/home/david/workspace/nsl-tuvm/tools/ns_compile_oevm.py` (opcode table — match the new VM numbers), `tools/builtins_registry.py` + `tools/gen_builtins.py` (compact rows), `tools/solidify.py` (indices)
- Regenerate: `/home/david/workspace/tuvm/src/solidified_image.c` (+ `.stmap`) via the compiler

**Interfaces:**
- Consumes: all deletions from Tasks 2–8 (every removed feature's slot is currently reserved).
- Produces: a compact, gap-free opcode enum and builtin registry with a bumped `BUILTIN_REGISTRY_VERSION`; regenerated images; the compiler's constants byte-for-byte agree with the VM.

- [ ] **Step 1: Establish the single source of truth for the two numberings**

Confirm which file is authoritative for each numbering and that the other side derives from it:
```bash
grep -n "OP_" /home/david/workspace/tuvm/src/oevm.h | head -60
grep -n "OP_" /home/david/workspace/nsl-tuvm/tools/ns_compile_oevm.py | head -60
grep -rn "BUILTIN\|REGISTRY_VERSION" /home/david/workspace/tuvm/src/builtins.def /home/david/workspace/tuvm/src/oimg_loader.c /home/david/workspace/nsl-tuvm/tools/builtins_registry.py
```
Record the current (gapped) numbers on both sides so the renumber is a mechanical, mirrored edit.

- [ ] **Step 2: Compact the opcode enum (VM + compiler together)**

In `oevm.h`, remove the `_RETIRED` opcode slots (`OP_NLR_RETIRED`, `OP_MK_CONTEXT_RETIRED`, `OP_LD_CTX_RETIRED`, `OP_ST_CTX_RETIRED`, and any others reserved in Tasks 3/8) and renumber the enum contiguously from 0. Mirror the exact same final numbers in `ns_compile_oevm.py`'s opcode table (`:16` region). They MUST match value-for-value.

- [ ] **Step 3: Compact the builtin registry (VM + compiler together)**

In `builtins.def`, remove the reserved fiber vtable rows (6/13/14, and the retired Runnable row now that the wrapper is gone), the NLR method symbol rows (15–19 that are NLR-only — keep generic `==`/`sender` if still used; grep first), and the `ContextVtable` row; renumber remaining rows contiguously. Bump `BUILTIN_REGISTRY_VERSION` in `oimg_loader.c`. Mirror the identical row order in `builtins_registry.py`, regenerate with `gen_builtins.py`, and update `solidify.py` indices and `kernel_boot.c` `builtin_registry[...]` assignments.

- [ ] **Step 4: Compact the prim ids**

In `dispatch.c` remove the fiber prim enum gaps and the three `PRIM_NAMES` holes; in `prim_ids.h` undo the fiber-induced `+1` platform-prim shift (restore the pre-fiber numbering per the `:8–9` comment). Mirror any prim-id constants the compiler emits.

- [ ] **Step 5: Regenerate images and rebuild**

```bash
cd /home/david/workspace/nsl-tuvm && python3 tools/gen_builtins.py
# regenerate the solidified image from the kernel/stdlib via a representative test
cd /home/david/workspace/nsl-tests && \
  python3 /home/david/workspace/nsl-tuvm/tools/ns_compile_oevm.py 04_somns_basic/01_method_call.ns \
    /home/david/workspace/tuvm/src/solidified_image.c
cd /home/david/workspace/tuvm && \
  gcc -Wall -Wextra -std=c99 -Isrc src/vm.c src/gc.c src/dispatch.c src/platform.c \
    src/solidified_image.c src/oevm_main.c -o oevm_runner -lm && ./oevm_runner
```
Expected: builds and prints the test's expected output (e.g. `42`). A garbled result means the two numberings disagree — diff Steps 2–4.

- [ ] **Step 6: Full suite, both paths, plus C tests**

```bash
/home/david/workspace/tuvm/tools/run_c_tests.sh; echo "c-exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: C tests PASS; conformance GREEN on **both** Path A and Path B. (`tools/check_kernel_parity.py` was deleted in the Task 2 review-fix pass — it had been reduced to an always-succeeding no-op with nothing left to check; the conformance suite plus `check_no_reflective_emit`/grep proofs remain the gate.) Also run `tools/check_no_ambient_reflection.sh` and `tests/test_portability_32.sh` if present (the latter may skip with exit 77 if `gcc-multilib` is absent — that's acceptable).

- [ ] **Step 7: Commit (both repos, one logical change)**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "refactor(milestone-a): clean renumber opcodes + builtin registry

Compact every reserved fiber/NLR/Context slot; bump BUILTIN_REGISTRY_VERSION;
regenerate images. No gravestone slots remain."
git -C /home/david/workspace/nsl-tuvm add -A && git -C /home/david/workspace/nsl-tuvm commit -m "refactor(milestone-a): mirror the tuvm opcode+registry renumber"
```

---

### Task 10: Test cleanup, footprint delta, provenance — Milestone A done

Remove the deleted-feature C tests, fix `test_frame_abi` for the 2-way discriminator, prune the debugger test's fiber coupling, measure and **bank the footprint delta**, and write the provenance/validation note. This is the milestone's success gate (spec §7).

**Files:**
- Delete: `/home/david/workspace/tuvm/tests/test_fibers.c`, `tests/test_context.c`, `tests/test_ctx_ops.c`, `tests/test_stack.c`
- Modify: `/home/david/workspace/tuvm/tests/test_frame_abi.c` (drop the reified-discriminator assertions; assert 2-way `NULL`|relative)
- Modify: `/home/david/workspace/tuvm/tests/test_debugger.c` (remove `fiber:singleStep:`/`current_fiber` coupling; use `singleStep:` on the retained state)
- Create: `/home/david/workspace/nsl-tuvm/docs/superpowers/plans/milestone-a-footprint-delta.md`
- Modify: `/home/david/workspace/tuvm/docs/` provenance (a short `MILESTONE_A.md` note)

**Interfaces:**
- Consumes: the fully-deleted, renumbered VM+compiler (Tasks 2–9) and the Task 1 baseline footprint.
- Produces: a green KEEP-test suite (no deleted-feature tests), a recorded footprint delta, and a provenance note — Milestone A validation criteria all met.

- [ ] **Step 1: Delete the dead C tests**

```bash
git -C /home/david/workspace/tuvm rm tests/test_fibers.c tests/test_context.c tests/test_ctx_ops.c tests/test_stack.c
```
Confirm `tools/run_c_tests.sh` does not reference any of them (it was written KEEP-only in Task 1):
```bash
grep -n "test_fibers\|test_context\|test_ctx_ops\|test_stack" /home/david/workspace/tuvm/tools/run_c_tests.sh
```
Expected: no hits.

- [ ] **Step 2: Fix `test_frame_abi.c` for the 2-way discriminator**

`test_frame_abi.c` asserts `home_ctx` is a total 3-way discriminator (relative/reified/none). Remove the reified-branch assertions and the `home_link_is_reified` usage; assert the discriminator is now total over `{NULL, relative-SMI}` only. Then:
```bash
gcc -std=c99 -Isrc /home/david/workspace/tuvm/tests/test_frame_abi.c -o /tmp/tfa && /tmp/tfa && echo FRAME_ABI_OK
```
Expected: `FRAME_ABI_OK`.

- [ ] **Step 3: Prune `test_debugger.c` fiber coupling**

Remove the `fiber:singleStep:`/`current_fiber`/`FiberVtable` references; drive single-step via the retained `singleStep:` path (or the kept alias). Rebuild+run it via its header command; expected PASS. If the debugger test is too entangled to fix quickly, mark the fiber-single-step sub-case skipped with a `TODO(milestone-a)` and record it in the footprint-delta note — do not delete the whole debugger test (the mirror/debugger is KEEP).

- [ ] **Step 4: Run the full KEEP C suite and the conformance suite one final time**

```bash
/home/david/workspace/tuvm/tools/run_c_tests.sh; echo "c-exit=$?"
cd /home/david/workspace/nsl-tests && OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB \
  python3 /home/david/workspace/nsl-tuvm/tools/run_tests_oevm.py 2>&1 | tail -30
```
Expected: `c-exit=0`; conformance pared subset GREEN on Path A and B.

- [ ] **Step 5: Measure and bank the footprint delta**

```bash
/home/david/workspace/tuvm/tools/footprint.sh | tee /home/david/workspace/tuvm/tools/footprint-after.txt
diff /home/david/workspace/tuvm/tools/footprint-baseline.txt /home/david/workspace/tuvm/tools/footprint-after.txt || true
```
Expected: the `(TOTALS)` text column is **smaller** than the baseline (the balloon is gone — spec §7 predicts the engine shrinks). Write `docs/superpowers/plans/milestone-a-footprint-delta.md` recording baseline text, after text, the delta in bytes and %, and which features accounted for it. If the engine did **not** shrink, STOP and investigate — that contradicts the milestone's premise.

- [ ] **Step 6: Write the provenance / validation note**

Create `/home/david/workspace/tuvm/docs/MILESTONE_A.md`: what was deleted (fibers, NLR, reify/E-series, growable stacks, inert Context), what was kept (flat loop, precise GC, `.oimg`, capability seam, mirror/debugger, access subsystem), the renumber (bumped `BUILTIN_REGISTRY_VERSION`), the footprint delta, and the pared-subset definition. State the seed provenance (`tuvm ← oevm@aea0af1`, `nsl-tuvm ← ns-oevm@188426e`) and that no pre-fiber commit existed (root = the fiber commit).

- [ ] **Step 7: Final commit — Milestone A complete**

```bash
git -C /home/david/workspace/tuvm add -A && git -C /home/david/workspace/tuvm commit -m "test(milestone-a): drop dead tests, fix frame_abi/debugger, bank footprint delta

Milestone A complete: fibers/NLR/reify/growable deleted, pared conformance
green (Path A+B), engine footprint delta recorded. See docs/MILESTONE_A.md."
git -C /home/david/workspace/nsl-tuvm add -A && git -C /home/david/workspace/nsl-tuvm commit -m "docs(milestone-a): footprint delta + pared-subset final"
```

---

---

## Cross-validation against the pre-fiber design (the "double-check")

**Why this exists:** David asked to compare the stripped repo against the pre-fiber commit as a sanity check. That commit does not exist in git (root = the fiber commit; phases 1–3 predate `git init`). The valid substitute is the pre-fiber **design record** — `docs/dev_process/phase1_walkthrough.md` (core C structures), `phase2_walkthrough.md` (dispatch/send + inline cache), `phase3_walkthrough.md` (mirrors/reflection + reified stack walk) — all in the `tuvm` seed. Run this pass **after Task 10**, before declaring Milestone A done. It catches "did the strip sweep up something the pre-fiber core needed?" — the class of bug that already surfaced for `home_generation`.

- [ ] **CV-1: Struct inventory.** For each struct the Phase 1 doc lists (`Object`, `Symbol`, `Binding`, `Vtable`, `MethodContext`, `Closure`, `InlineCache`), confirm it still exists in the stripped `oevm.h` with its pre-fiber (non-fiber, non-reify) fields. Expected removed: `struct Fiber`, `struct Context`, `Closure.captured_env`. Expected retained: everything else. `test_layout.c` green is the machine check.
- [ ] **CV-2: Dispatch/send.** Confirm the Phase 2 send path (`send()`, method lookup, the inline cache, `does_not_understand`) is intact and only the fiber-switch/NLR-unwind branches were removed. `test_method_cache.c` + `test_oevm.c` green.
- [ ] **CV-3: Mirrors/reflection.** Confirm the Phase 3 reflective surface (`vtable`/`delegate`/`bindings`/`home`/`sender`/`pc`, `BoxedBinding`, the reified stack walk) still works — the mirror/debugger is KEEP. `test_debugger.c` (post-prune) green; a mirror-based stack walk still enumerates frames.
- [ ] **CV-4: Lifecycle guard.** Explicitly resolve the `home_generation`/`current_generation_tag` question from Task 8 Step 3: is stale-home-frame protection still present (kept) or provably unnecessary (removed)? Record the answer and its justification in `docs/MILESTONE_A.md`.
- [ ] **CV-5: Delta review (VM).** `git -C /home/david/workspace/tuvm diff fa301d3 --stat` (seed → stripped). Read the file-level deltas and confirm every change maps to an intended Milestone-A deletion (fibers/NLR/reify/growable/renumber) with no collateral edits to KEEP subsystems (GC core, `.oimg` loader, capability seam, backend). Anything unexplained is a regression to investigate before sign-off.
- [ ] **CV-6: Compiler fiber-delta cross-check (a real pre-fiber reference exists).** Unlike the VM, `ns-oevm` (the compiler's frozen original) *does* have a pre-fiber commit: the `Concurrency`/`Fiber` stdlib facade was added late at `88fdf6e` ("feat(concurrency): pure-.ns Concurrency/Fiber facade + scheduler (R7)"), parent `d1f07e6`. Confirm our Task 4 stdlib strip is the inverse of that addition:
  ```bash
  # what fibers ADDED to the stdlib in the original:
  git -C /home/david/workspace/ns-oevm show 88fdf6e -- lib/kernel.ns
  # what our strip REMOVED — should cover the same Concurrency/Fiber region:
  git -C /home/david/workspace/nsl-tuvm log -p --follow -- lib/kernel.ns | grep -A2 -B2 "Concurrency\|class Fiber"
  ```
  The two should be mirror images (modulo later touch-ups between `88fdf6e` and the seed). If our strip removed *less* than `88fdf6e` added, a fiber straggler remains; if it removed *more*, we may have cut a non-fiber method — investigate. **Note (scope, not a gap):** there is **no** pre-NLR commit in `ns-oevm` (NLR codegen is in the root commit `f4c18df`), so the Task 2 NLR strip has no git reference and is validated only by the conformance suite + `03_nlr_compile_fail.ns`. The R7 boundary is 69 commits before the seed and lacks later keep-worthy work (mirror confinement, net capability, derecursify E1) — it is a **delta reference only, never a revert target**.

## Milestone A Done-When (spec §7 subset)

- [ ] `tuvm`/`nsl-tuvm`/`nsl-tests` exist as seeded repos with recorded provenance (Task 1, Task 10).
- [ ] The balloon (fibers, NLR, reify/E-series, growable stacks, inert Context) is deleted; `grep` finds no live references (Tasks 3,5,6,7,8).
- [ ] The pared conformance subset is green on the host, Path A and B (Task 9,10).
- [ ] The footprint delta is measured and recorded; the engine shrank (Task 10).
- [ ] Opcodes + builtin registry are cleanly renumbered with no gravestone slots (Task 9).

**Out of scope (later milestones):** the message algebra (Milestone B), channels+reactor (C), supervisor+let-it-crash+host job (D), and everything in spec §8. The vestigial access-control subsystem (oevm#36) is explicitly retained, not deleted, in Milestone A.
