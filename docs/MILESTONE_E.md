# Milestone E — provenance and validation note

*A reference note, written once the milestone was green, for phase 2 and later
milestones to cite. Day-by-day narrative lives in `review/program-sequence.md`,
`review/lib/errors.md` (exceptions section), and
`nsl-tuvm/.superpowers/sdd/me-task-{0..6}-*.md`.*

Milestone E's premise (`review/program-sequence.md` phase-2 rung 1, first-step
spec §8 deferral, `review/lib/exceptions.md`'s 5 pinned constraints): land full
`on:do:`/`ensure:` **above** Milestone D's `setjmp` crash boundary without
reworking it, so the language gains predicate-filtered catch and always-run
cleanup while the let-it-crash supervisor semantics are preserved intact.

## 1. What landed

- **Tagged error value + Record-valued raise** (`tuvm/src/dispatch.c`,
  `nsl-tuvm/lib/kernel.ns`). Three C prims, append-only from D's last id 89:
  `raise` (id 90) re-raises the receiver condition unchanged; `error:tag:`
  (id 91) and `error:` (id 9, repointed) build a crash `Record {#tag,#text}`.
  An app receiver cannot reach the `Record` class lexically, so the Record is
  built in C via the **captured kernel instance** (`g_kernel_instance`, a GC
  root set on `main:args:` before any app code, both boot paths) sent
  `crashRecordTag:text:` / `crashRecordText:`. `Space>>postCrash:` rewired to
  carry the raise tag (decision (a) below).
- **`on:do:` — predicate-filtered catch** (`tuvm/src/vm.c` `vm_on_do`, id 92).
  A `vm_protected_fire` sibling: on a caught fault it restores `g_protect_top`
  to the outer level, evaluates the predicate `pred value: e`, runs the handler
  if truthy (its value is the result) else re-raises — cascading frame-by-frame
  to the next enclosing handler.
- **`ensure:` — always-run cleanup** (`tuvm/src/vm.c` `vm_ensure`, id 93). Runs
  the cleanup block on BOTH clean completion and a caught fault; on a fault it
  re-raises AFTER cleanup. Innermost-first ordering falls out of natural C-stack
  nesting. The `{block, policy}` marker is reserved (comment-only; only `always`
  exists in E — a future `ifCurtailed:` is the curtailed-only branch).
- **Re-raise + composition** — `e raise` inside a handler reaches the next
  enclosing handler with the same tag/text; a new raise inside a handler body or
  an `ensure:` cleanup cascades outward without wedging.
- **Integration** — an uncaught error with a non-matching `on:do:` present still
  reaches `sys/crash` unchanged; C5 no-cross-dispatch (one reaction's crash can
  never be caught by another reaction's `on:do:`); a driver-shaped Tier-2
  recovery cycle (`on:do:` a `#timeout`, degrade to last-good, `ensure:` the bus
  release, node keeps running); and `^` inside a handler/predicate/cleanup is a
  compile error (`non-local return`, `07_no_nlr`) — handler bodies are real
  escaping closures.

## 2. Decisions & reconciliations

- **Decision (a): `sys/crash` carries the raise tag; `#crash` retired.** In
  Milestone D the crash record was tagged `#crash`; E posts it with the tag of
  the raised condition. Safe because the supervisor attributes crashes by
  `#channel`, never by tag — no test asserted `#crash`.
- **F1 (whole-branch review, fixed before close): interned default `#error`.**
  A bare `self error: 'x'` originally minted its `#error` tag as a fresh
  non-interned Symbol, so the spec §3/§6 handler idiom `[ :e | e tag = #error ]`
  never matched (Symbol `=` is identity). Fixed: the default tag is now supplied
  by the kernel helper `crashRecordText:` as the image-**interned** `#error`
  literal, so identity matches. Proven by
  `nsl-tests/12_exceptions/14_error_default_tag.ns`.
- **Handlers read `e tag` / `e at: #text ifAbsent: […]`** — there is no
  `Object>>text`; the crash Record carries `#text`.

## 3. Conformance state — final gate

Run: `cd nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py` (both Path A
= solidified image and Path B = `.oimg`, the full behavioral suite + C-unit
tests + integration gates). One clean uninterrupted run:

```
Passed:  170
Failed:  0
Skipped: 12
grant: PASS   debugger-integration: PASS   portability-32: PASS   boot-gc-asan: PASS
```

Delta over Milestone D's 155: +15 behavioral tests — `nsl-tests/03_blocks/04_own_temp_escaping.ns`
(Task 0 regression guard) + `nsl-tests/12_exceptions/01..14` (the exception suite).
`boot-gc-asan` PASS covers the GC∩unwind rooting on the exception paths (spec §8).

## 4. Footprint delta

Measured with `tuvm/tools/footprint.sh` (host-proxy build; `size -t` totals over
the engine objects, `.oimg`/image object excluded; RAM = data+bss, per design
addendum A2). Banked in `tuvm/tools/footprint-after.txt`;
`tuvm/tools/footprint-baseline.txt` (Milestone-A seed) unchanged.

| | text | data+bss (RAM, host proxy) |
|---|---:|---:|
| after Milestone D (`MILESTONE_D.md`) | 41080 | 17262424 |
| after Milestone E (measured at HEAD) | 43250 | 17262488 |
| delta | +2170 bytes | +64 bytes |

Per-object attribution (comparing the two `size -t` runs):

- `vm.o` +1259 text / +32 bss — `vm_on_do` + `vm_ensure` (the two new protected
  siblings) + `vm_condition_text` diagnostic.
- `dispatch.o` +899 text / +32 data — `raise`/`error:tag:`/`on:do:`/`ensure:`
  prim cases + `build_and_raise`/`build_and_raise_default` + the `PRIM_error`
  rewire + the 4 new prim enum/name slots (90-93).
- `gc.o` +12 text / 0 bss — the `g_kernel_instance` root-scan wiring.
- `kernel_boot.o`, `platform.o`, `serial.o`, `udp.o`, `mirror.o`,
  `oimg_loader.o`, `space.o`, and the three `*_posix.o` backend objects:
  unchanged. (The kernel.ns `crashRecordTag:text:`/`crashRecordText:` helpers
  live in the solidified image, not these engine objects.)

Growth is small and entirely attributable to the exception C surface — no
unattributed drift.

## 5. Provenance

Branch `milestone-e`, all three repos. Tips at close (before this note's commit):
- `tuvm` `034eb8a` (F1 fix; the note lands in the same close-out commit series)
- `nsl-tuvm` `b867523` (F1 kernel helper; Tasks 1 kernel work at `24bd330`)
- `nsl-tests` `9cf5fd1` (F1 test 14; the exception suite across the E commits)

Pre-E BASE (whole-branch review merge-base): `tuvm df32dd9` · `nsl-tuvm a02f20f`
· `nsl-tests 5ec726b`.

Every task (0–5) + the F1 fix was individually opus-reviewed; the whole-branch
review (`me-final-review-*.diff`) returned Ready-with-nits — the one IMPORTANT
finding (F1) was fixed before this close; the remaining MINORs (F2 abort-only
`vm_condition_text` cosmetic, F3 a stale `id 9` code comment, F4 the reserved
policy marker) are documented, non-blocking. See
`nsl-tuvm/.superpowers/sdd/me-task-{0..6}-report.md` for the per-task record and
`progress.md` for the ledger.
